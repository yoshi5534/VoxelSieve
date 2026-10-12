#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/denoise.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/model.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/synthetic.hpp"

namespace voxelsieve {
namespace {

constexpr std::array<double, 3> kBoxSize{10.0, 6.0, 4.0};

/// A box with three lunkers of three voxels radius, unsharp, with correlated noise like a
/// reconstructed scan.
SyntheticSpec noisySpec() {
  SyntheticSpec spec;
  spec.lunker_count = 3;
  spec.lunker_radius_mm = 0.3;
  spec.blur_sigma_mm = 0.06;
  spec.noise_sigma = 1500.0;
  spec.noise_correlation_mm = 0.07;
  return spec;
}

class DenoiseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_denoise_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  /// Sieves `scan` into a dataset of small bricks, so tiles and halos cross brick boundaries.
  Dataset sieve(const SyntheticScan& scan, const std::string& name) {
    DatasetOptions options;
    options.brick_size = 32;
    (void)writeDataset(scan, dir_ / name, options);
    return Dataset::open(dir_ / name);
  }

  static std::vector<float> level0(const Dataset& dataset) {
    const auto dims = dataset.info().dims;
    std::vector<float> values(static_cast<std::size_t>(dims[0] * dims[1] * dims[2]));
    dataset.readRegion(0, {{0, 0, 0}, dims}, values, -1.0F);
    return values;
  }

  std::filesystem::path dir_;
};

TEST_F(DenoiseTest, NoiseIsMeasuredWithItsCorrelation) {
  SyntheticSpec spec;
  spec.noise_sigma = 800.0;
  spec.noise_correlation_mm = 0.07;  // 0.7 voxels: neighbours correlate by exp(-1 / 1.96) = 0.60
  const SyntheticScan scan(boxMesh(kBoxSize), spec);
  const Dataset dataset = sieve(scan, "scan.vsieve");
  const NoiseModel noise = estimateNoise(dataset, 3);
  EXPECT_NEAR(noise.sigma, 800.0, 80.0);
  EXPECT_EQ(noise.radius, 3);
  EXPECT_EQ(noise.pairVariance(0, 0, 0), 0.0);
  for (const auto& [dx, dy, dz] : {std::array{1, 0, 0}, {0, 1, 0}, {0, 0, -1}}) {
    EXPECT_NEAR(noise.correlation(dx, dy, dz), 0.60, 0.08);
  }
  // exp(-4 / 1.96) = 0.13 two voxels apart, nothing three apart.
  EXPECT_NEAR(noise.correlation(0, 2, 0), 0.13, 0.08);
  EXPECT_NEAR(noise.correlation(3, 0, 0), 0.0, 0.08);
  EXPECT_THROW((void)noise.pairVariance(4, 0, 0), std::out_of_range);

  const NoiseModel white = NoiseModel::white(100.0, 2);
  EXPECT_EQ(white.pairVariance(2, -1, 0), 20000.0);
  EXPECT_EQ(white.correlation(1, 0, 0), 0.0);
  EXPECT_EQ(white.pairVariance(0, 0, 0), 0.0);
}

TEST_F(DenoiseTest, NonLocalMeansRemovesNoiseAndKeepsSmallPores) {
  const SyntheticSpec spec = noisySpec();
  const SyntheticScan scan(boxMesh(kBoxSize), spec);
  SyntheticSpec clean_spec = spec;
  clean_spec.noise_sigma = 0.0;
  const SyntheticScan clean(boxMesh(kBoxSize), clean_spec);
  const Dataset noisy = sieve(scan, "noisy.vsieve");

  const DenoiseInfo info = denoiseDataset(noisy, dir_ / "denoised.vsieve");
  EXPECT_EQ(info.method, DenoiseMethod::kNonLocalMeans);
  EXPECT_NEAR(info.noise.sigma, spec.noise_sigma, 0.1 * spec.noise_sigma);
  const Dataset denoised = Dataset::open(dir_ / "denoised.vsieve");

  const DenoiseScore before = scoreDenoising(noisy, clean);
  const DenoiseScore after = scoreDenoising(denoised, clean);
  EXPECT_NEAR(before.rmse_material, spec.noise_sigma, 0.05 * spec.noise_sigma);
  EXPECT_GT(after.edge_voxels, 0);
  EXPECT_EQ(after.voxels, before.voxels);
  EXPECT_LT(after.rmse_material, 0.4 * before.rmse_material);
  EXPECT_LT(after.rmse_edges, 0.6 * before.rmse_edges);

  // The lunkers stay: each is found where it is, and the noise leaves no false pores.
  const PorosityResult pores = analyzePorosity(denoised);
  ASSERT_EQ(pores.pores.size(), scan.defects().size());
  for (const Defect& lunker : scan.defects()) {
    const auto origin = scan.originMm();
    const auto truth =
        scan.voxelSize().toVoxels({lunker.center_mm[0] - origin[0], lunker.center_mm[1] - origin[1],
                                   lunker.center_mm[2] - origin[2]});
    double nearest = 1e9;
    for (const DetectedPore& pore : pores.pores) {
      nearest = std::min(
          nearest, std::hypot(pore.center_voxels[0] - truth[0], pore.center_voxels[1] - truth[1],
                              pore.center_voxels[2] - truth[2]));
    }
    EXPECT_LT(nearest, 2.0);
  }
}

TEST_F(DenoiseTest, TilesGiveTheSameResult) {
  const SyntheticScan scan(boxMesh(kBoxSize), noisySpec());
  const Dataset noisy = sieve(scan, "noisy.vsieve");
  DenoiseOptions options;
  options.noise_sigma = 1500.0;
  options.tile = 64;
  (void)denoiseDataset(noisy, dir_ / "large.vsieve", options);
  options.tile = 12;  // tiles across brick boundaries
  (void)denoiseDataset(noisy, dir_ / "small.vsieve", options);
  EXPECT_EQ(level0(Dataset::open(dir_ / "large.vsieve")),
            level0(Dataset::open(dir_ / "small.vsieve")));
}

TEST_F(DenoiseTest, KeepsBricksVoxelsAndValueType) {
  const SyntheticScan scan(boxMesh(kBoxSize), noisySpec());
  const Dataset noisy = sieve(scan, "noisy.vsieve");
  const DatasetInfo& input = noisy.info();
  ASSERT_EQ(input.value_type, ValueType::kUInt16);
  const DenoiseInfo info = denoiseDataset(noisy, dir_ / "denoised.vsieve");
  const DatasetInfo stored = readDatasetInfo(dir_ / "denoised.vsieve");
  EXPECT_EQ(stored.value_type, ValueType::kUInt16);
  EXPECT_EQ(stored.active_voxel_count, input.active_voxel_count);
  EXPECT_EQ(stored.threshold, input.threshold);
  ASSERT_EQ(stored.levels.size(), input.levels.size());
  for (std::size_t l = 0; l < input.levels.size(); ++l) {
    EXPECT_EQ(stored.levels[l].bricks, input.levels[l].bricks);
  }
  EXPECT_TRUE(std::filesystem::exists(dir_ / "denoised.vsieve" / "overview.vdb"));
  // Removed air stays removed.
  const auto before = level0(noisy);
  const auto after = level0(Dataset::open(dir_ / "denoised.vsieve"));
  for (std::size_t i = 0; i < before.size(); ++i) {
    ASSERT_EQ(before[i] < 0.0F, after[i] < 0.0F);
  }

  DenoiseOptions options;
  options.output.value_type = ValueType::kFloat;
  (void)denoiseDataset(noisy, dir_ / "float.vsieve", options);
  EXPECT_EQ(readDatasetInfo(dir_ / "float.vsieve").value_type, ValueType::kFloat);
  options.output.value_type = ValueType::kUInt8;  // would quantise 16-bit grey values
  EXPECT_THROW((void)denoiseDataset(noisy, dir_ / "u8.vsieve", options), std::invalid_argument);
  EXPECT_THROW((void)denoiseDataset(noisy, dir_ / "denoised.vsieve"), std::invalid_argument);
  options = {};
  options.non_local_means.similarity_limit = 0.0;
  EXPECT_THROW((void)denoiseDataset(noisy, dir_ / "bad.vsieve", options), std::invalid_argument);
  options = {};
  options.method = DenoiseMethod::kModel;  // without a model
  EXPECT_THROW((void)denoiseDataset(noisy, dir_ / "bad.vsieve", options), std::invalid_argument);
}

TEST_F(DenoiseTest, DerivedDatasetsRebuildTheCoarseLevels) {
  const SyntheticScan scan(boxMesh(kBoxSize), noisySpec());
  const Dataset noisy = sieve(scan, "noisy.vsieve");
  const DatasetInfo info = writeDerivedDataset(
      noisy, dir_ / "constant.vsieve",
      [](const std::array<std::int64_t, 3>& /*brick*/, const Box& /*box*/,
         std::vector<float>& values) { std::fill(values.begin(), values.end(), 1234.4F); });
  EXPECT_EQ(info.levels.size(), noisy.info().levels.size());
  const Dataset constant = Dataset::open(dir_ / "constant.vsieve");
  for (int level = 0; level < static_cast<int>(info.levels.size()); ++level) {
    constant.forEachGrid(level, [](const auto& /*brick*/, const auto& grid) {
      for (auto it = grid.cbeginValueOn(); it; ++it) {
        ASSERT_EQ(*it, 1234);  // rounded to the nearest integer
      }
    });
  }
}

TEST_F(DenoiseTest, RunsADenoisingModel) {
  // A model that gives back its input: one 1x1x1 convolution with weight 1.
  ModelSpec spec;
  spec.task = ModelTask::kDenoising;
  spec.name = "Identity";
  spec.input_offset = 1000.0F;
  spec.input_scale = 1.0F / 16384.0F;
  spec.layers = {{"conv", "out", {"input"}, 1, 1, 1, false, {1.0F}, {0.0F}}};
  writeModel(dir_ / "identity.vsm", spec);
  const Model model = Model::load(dir_ / "identity.vsm");
  EXPECT_EQ(model.spec().task, ModelTask::kDenoising);
  const std::vector<float> grey{1000.0F, 21000.0F, 3.0F, 65535.0F};
  EXPECT_EQ(model.denoise(grey, {2, 2, 1}, 0.0F), grey);
  EXPECT_THROW((void)model.classify(grey, {2, 2, 1}, 0.0F), std::invalid_argument);

  const SyntheticScan scan(boxMesh(kBoxSize), noisySpec());
  const Dataset noisy = sieve(scan, "noisy.vsieve");
  DenoiseOptions options;
  options.method = DenoiseMethod::kModel;
  options.model = &model;
  options.tile = 20;
  const DenoiseInfo info = denoiseDataset(noisy, dir_ / "denoised.vsieve", options);
  EXPECT_EQ(info.model, "Identity");
  EXPECT_EQ(level0(Dataset::open(dir_ / "denoised.vsieve")), level0(noisy));
  EXPECT_THROW((void)segmentMaterialsWithModel(noisy, model, dir_ / "materials"),
               std::invalid_argument);

  // A denoising model has one output channel and no materials.
  spec.materials = {Material{}};
  EXPECT_THROW(Model{spec}, std::invalid_argument);
  spec.materials.clear();
  spec.layers.front().out_channels = 2;
  spec.layers.front().weights = {1.0F, 1.0F};
  spec.layers.front().bias = {0.0F, 0.0F};
  EXPECT_THROW(Model{spec}, std::invalid_argument);
}

}  // namespace
}  // namespace voxelsieve
