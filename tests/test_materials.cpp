#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "material_scene.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/slice.hpp"
#include "voxelsieve/source.hpp"

namespace voxelsieve {
namespace {

class MaterialsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_materials_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

TEST(MultiOtsu, SplitsSeparatedPeaks) {
  std::vector<std::uint64_t> histogram(65536, 0);
  for (const int peak : {3000, 9000, 21000}) {
    for (int v = peak - 500; v <= peak + 500; ++v) {
      histogram[static_cast<std::size_t>(v)] = 10;
    }
  }
  const std::vector<float> two = multiOtsu(histogram, 3);
  ASSERT_EQ(two.size(), 2U);
  EXPECT_GT(two[0], 3500.0F);
  EXPECT_LT(two[0], 8500.0F);
  EXPECT_GT(two[1], 9500.0F);
  EXPECT_LT(two[1], 20500.0F);
  EXPECT_TRUE(multiOtsu(histogram, 1).empty());
  EXPECT_THROW((void)multiOtsu(std::vector<std::uint64_t>(65536, 0), 2), std::invalid_argument);
}

// Adds `count` voxels in a Gaussian peak around `mean`.
void addPeak(std::vector<std::uint64_t>& histogram, double mean, double sigma, double count) {
  for (std::size_t v = 0; v < histogram.size(); ++v) {
    const double d = (static_cast<double>(v) - mean) / sigma;
    histogram[v] += static_cast<std::uint64_t>(
        std::llround(count * std::exp(-0.5 * d * d) / (sigma * 2.5066282746310002)));
  }
}

TEST(SuggestMaterials, FindsOnePeakPerMaterial) {
  std::vector<std::uint64_t> histogram(65536, 0);
  addPeak(histogram, 1000.0, 1200.0, 5e7);  // air, whose tail reaches past the threshold
  addPeak(histogram, 9000.0, 1200.0, 7e6);
  addPeak(histogram, 20000.0, 1500.0, 2.5e6);
  addPeak(histogram, 35000.0, 1500.0, 5e5);  // a small dense class
  const MaterialSuggestion three = suggestMaterials(histogram, 4500.0F);
  ASSERT_EQ(three.peaks.size(), 3U);
  EXPECT_NEAR(three.peaks[0], 9000.0F, 500.0F);
  EXPECT_NEAR(three.peaks[1], 20000.0F, 500.0F);
  EXPECT_NEAR(three.peaks[2], 35000.0F, 500.0F);
  ASSERT_EQ(three.thresholds.size(), 2U);
  EXPECT_GT(three.thresholds[0], 12000.0F);
  EXPECT_LT(three.thresholds[0], 17000.0F);
  EXPECT_GT(three.thresholds[1], 25000.0F);
  EXPECT_LT(three.thresholds[1], 31000.0F);

  // The two most prominent peaks when fewer are allowed.
  const MaterialSuggestion two = suggestMaterials(histogram, 4500.0F, 2);
  ASSERT_EQ(two.peaks.size(), 2U);
  EXPECT_NEAR(two.peaks[0], 9000.0F, 500.0F);
  EXPECT_NEAR(two.peaks[1], 20000.0F, 500.0F);

  // Two classes that blend into one peak with a shoulder are one material.
  std::vector<std::uint64_t> blended(65536, 0);
  addPeak(blended, 10000.0, 1500.0, 1e6);
  addPeak(blended, 12000.0, 1500.0, 3e5);
  EXPECT_EQ(suggestMaterials(blended, 4000.0F).peaks.size(), 1U);
  EXPECT_TRUE(suggestMaterials(std::vector<std::uint64_t>(65536, 0), 4000.0F).peaks.empty());
}

TEST_F(MaterialsTest, SuggestsTheMaterialsFromTheHistogram) {
  const Scene scene = makeScene();
  DatasetOptions sieve;
  sieve.threshold = kThreshold;
  (void)writeDataset(MemorySource(scene.grey), dir_ / "dataset", sieve);
  const Dataset dataset = Dataset::open(dir_ / "dataset");

  const MaterialSuggestion suggestion = suggestMaterials(datasetHistogram(dataset), kThreshold);
  ASSERT_EQ(suggestion.peaks.size(), 2U);
  EXPECT_NEAR(suggestion.peaks[0], static_cast<float>(kLight), 600.0F);
  EXPECT_NEAR(suggestion.peaks[1], static_cast<float>(kDense), 600.0F);

  SegmentationOptions options;
  options.materials = 0;
  const MaterialVolumeInfo info = segmentMaterials(dataset, dir_ / "materials", options);
  ASSERT_EQ(info.materials.size(), 2U);
  EXPECT_FLOAT_EQ(info.materials[1].lower, suggestion.thresholds[0]);
  const MaterialScore score =
      scoreMaterials(MaterialVolume::open(dir_ / "materials"), MemorySource(scene.labels), dataset);
  EXPECT_EQ(score.components_per_material, (std::vector<std::int64_t>{0, 2, 1}));
  EXPECT_GT(score.dice[2], 0.97);
}

/// Volume enclosed by a mesh in voxels, positive when its triangles face outwards.
double enclosedVolume(const IndexedMesh& mesh) {
  double volume = 0.0;
  for (const auto& t : mesh.triangles) {
    const auto& a = mesh.points[t[0]];
    const auto& b = mesh.points[t[1]];
    const auto& c = mesh.points[t[2]];
    volume += (static_cast<double>(a[0]) * (b[1] * c[2] - b[2] * c[1]) -
               static_cast<double>(a[1]) * (b[0] * c[2] - b[2] * c[0]) +
               static_cast<double>(a[2]) * (b[0] * c[1] - b[1] * c[0])) /
              6.0;
  }
  return volume;
}

TEST_F(MaterialsTest, MeshesEachMaterial) {
  const Scene scene = makeScene();
  DatasetOptions sieve;
  sieve.threshold = kThreshold;
  sieve.brick_size = 32;
  (void)writeDataset(MemorySource(scene.grey), dir_ / "dataset", sieve);
  (void)segmentMaterials(Dataset::open(dir_ / "dataset"), dir_ / "materials");
  const MaterialVolume volume = MaterialVolume::open(dir_ / "materials");

  // The dense cube of 16 voxels, facing outwards, where it lies.
  const IndexedMesh dense = materialMesh(volume, 2);
  ASSERT_FALSE(dense.triangles.empty());
  EXPECT_NEAR(enclosedVolume(dense), 16.0 * 16.0 * 16.0, 0.08 * 16.0 * 16.0 * 16.0);
  for (const auto& p : dense.points) {
    EXPECT_GT(p[0], 42.0F);
    EXPECT_LT(p[0], 61.0F);
  }
  // Coarser when it must fit fewer samples or triangles.
  const IndexedMesh sampled = materialMesh(volume, 2, 0, 96 * 96 * 96 / 8);
  EXPECT_LT(sampled.triangles.size(), dense.triangles.size());
  EXPECT_NEAR(enclosedVolume(sampled), 16.0 * 16.0 * 16.0, 0.3 * 16.0 * 16.0 * 16.0);
  EXPECT_LE(materialMesh(volume, 2, 500).triangles.size(), 500U);
  // The hollow box: its outer and inner walls enclose the shell.
  const IndexedMesh light = materialMesh(volume, 1);
  EXPECT_NEAR(enclosedVolume(light), 64.0 * 64.0 * 64.0 - 52.0 * 52.0 * 52.0,
              0.1 * (64.0 * 64.0 * 64.0 - 52.0 * 52.0 * 52.0));
  EXPECT_TRUE(materialMesh(volume, 3).triangles.empty());
}

TEST_F(MaterialsTest, SegmentsTwoMaterialsAndKeepsThinSheet) {
  const Scene scene = makeScene();
  DatasetOptions sieve;
  sieve.threshold = kThreshold;
  sieve.brick_size = 32;  // many bricks, so the halo crosses brick borders
  sieve.min_material_voxels = 4;
  (void)writeDataset(MemorySource(scene.grey), dir_ / "dataset", sieve);
  const Dataset dataset = Dataset::open(dir_ / "dataset");

  const MaterialVolumeInfo info = segmentMaterials(dataset, dir_ / "materials");
  ASSERT_EQ(info.materials.size(), 2U);
  EXPECT_FLOAT_EQ(info.materials[0].lower, kThreshold);
  EXPECT_GT(info.materials[1].lower, 9000.0F);
  EXPECT_LT(info.materials[1].lower, 19000.0F);
  EXPECT_EQ(readMaterialVolumeInfo(dir_ / "materials").materials[1].voxel_count,
            info.materials[1].voxel_count);

  const MaterialVolume volume = MaterialVolume::open(dir_ / "materials");
  const MaterialScore score = scoreMaterials(volume, MemorySource(scene.labels), dataset);
  EXPECT_EQ(score.components, 3);
  EXPECT_EQ(score.components_per_material, (std::vector<std::int64_t>{0, 2, 1}));
  EXPECT_GT(score.dice[0], 0.97);
  EXPECT_GT(score.dice[1], 0.95);
  EXPECT_GT(score.dice[2], 0.97);

  // The sheet one voxel thin is material 1 almost everywhere; noise spikes in the air are not.
  const Box sheet{{32, 22, 22}, {33, 74, 74}};
  std::vector<std::uint8_t> ids(static_cast<std::size_t>(sheet.voxelCount()));
  volume.readRegion(sheet, ids);
  const auto light = std::count(ids.begin(), ids.end(), std::uint8_t{1});
  EXPECT_GT(static_cast<double>(light), 0.95 * static_cast<double>(ids.size()));
  const Box air{{0, 0, 0}, {kSize, kSize, 12}};
  std::vector<std::uint8_t> outside(static_cast<std::size_t>(air.voxelCount()));
  volume.readRegion(air, outside);
  EXPECT_EQ(std::count(outside.begin(), outside.end(), std::uint8_t{0}),
            static_cast<std::ptrdiff_t>(outside.size()));

  // Strided reads sample every n-th voxel.
  const Box whole{{0, 0, 0}, {kSize, kSize, kSize}};
  std::vector<std::uint8_t> coarse(static_cast<std::size_t>(24 * 24 * 24));
  volume.readRegion(whole, coarse, 4);
  EXPECT_EQ(coarse[static_cast<std::size_t>(12 + 24 * (12 + 24 * 12))], 2);  // (48, 48, 48)
}

TEST_F(MaterialsTest, ReadsRegionsOfMoreBricksThanTheCacheHolds) {
  const Scene scene = makeScene();
  DatasetOptions sieve;
  sieve.threshold = kThreshold;
  sieve.brick_size = 16;  // 216 bricks, more than the material volume keeps cached
  sieve.min_material_voxels = 4;
  (void)writeDataset(MemorySource(scene.grey), dir_ / "dataset", sieve);
  const Dataset dataset = Dataset::open(dir_ / "dataset");
  (void)segmentMaterials(dataset, dir_ / "materials");
  const MaterialVolume volume = MaterialVolume::open(dir_ / "materials");

  // The whole volume agrees with the ground truth: the light box and sheet are material 1, the
  // dense cube material 2.
  const Box whole{{0, 0, 0}, {kSize, kSize, kSize}};
  std::vector<std::uint8_t> ids(static_cast<std::size_t>(whole.voxelCount()));
  volume.readRegion(whole, ids);
  const auto at = [&ids](std::int64_t x, std::int64_t y, std::int64_t z) {
    return ids[static_cast<std::size_t>(x + kSize * (y + kSize * z))];
  };
  std::int64_t agree = 0;
  for (std::int64_t z = 0; z < kSize; ++z) {
    for (std::int64_t y = 0; y < kSize; ++y) {
      for (std::int64_t x = 0; x < kSize; ++x) {
        const std::uint16_t label = labelAt(x, y, z);
        agree += at(x, y, z) == (label == 0 ? 0 : label == 2 ? 2 : 1) ? 1 : 0;
      }
    }
  }
  EXPECT_GT(static_cast<double>(agree), 0.99 * static_cast<double>(whole.voxelCount()));

  // A strided region that starts and ends inside bricks samples the same voxels.
  const Box region{{5, 3, 7}, {91, 90, 95}};
  constexpr std::int64_t kStride = 3;
  const std::array<std::int64_t, 3> count{29, 29, 30};
  std::vector<std::uint8_t> coarse(static_cast<std::size_t>(count[0] * count[1] * count[2]));
  volume.readRegion(region, coarse, kStride);
  std::size_t offset = 0;
  for (std::int64_t k = 0; k < count[2]; ++k) {
    for (std::int64_t j = 0; j < count[1]; ++j) {
      for (std::int64_t i = 0; i < count[0]; ++i) {
        ASSERT_EQ(coarse[offset++], at(5 + i * kStride, 3 + j * kStride, 7 + k * kStride))
            << i << ' ' << j << ' ' << k;
      }
    }
  }

  // The 3D view's overview reads all bricks at once and shows the materials of every 4th voxel
  // (level 2), like the slices of that level.
  const VolumePreview preview = readVolumePreview(dataset, 24, nullptr, &volume);
  ASSERT_EQ(preview.level, 2);
  ASSERT_EQ(preview.dims, (std::array<std::int64_t, 3>{24, 24, 24}));
  for (std::int64_t z = 0; z < 24; ++z) {
    SliceRequest request;
    request.axis = 2;
    request.level = 2;
    request.index = z * 4;
    request.size = {24, 24};
    const SliceImage slice = readSlice(dataset, request, nullptr, &volume);
    for (std::int64_t y = 0; y < 24; ++y) {
      for (std::int64_t x = 0; x < 24; ++x) {
        const std::uint8_t expected =
            at(x * 4, y * 4, z * 4) == 0
                ? 0
                : static_cast<std::uint8_t>(static_cast<int>(SliceOverlay::kMaterial) +
                                            at(x * 4, y * 4, z * 4));
        const auto pixel = static_cast<std::size_t>(x + 24 * y);
        ASSERT_EQ(slice.overlay[pixel], expected) << x << ' ' << y << ' ' << z;
        ASSERT_EQ(preview.overlay[pixel + static_cast<std::size_t>(z * 24 * 24)], expected)
            << x << ' ' << y << ' ' << z;
      }
    }
  }
}

TEST_F(MaterialsTest, RejectsBadOptions) {
  const Scene scene = makeScene();
  DatasetOptions sieve;
  sieve.threshold = kThreshold;
  (void)writeDataset(MemorySource(scene.grey), dir_ / "dataset", sieve);
  const Dataset dataset = Dataset::open(dir_ / "dataset");
  SegmentationOptions options;
  options.materials = -1;
  EXPECT_THROW((void)segmentMaterials(dataset, dir_ / "a", options), std::invalid_argument);
  options.materials = 2;
  options.material_thresholds = {1.0F, 2.0F};
  EXPECT_THROW((void)segmentMaterials(dataset, dir_ / "b", options), std::invalid_argument);
  options.material_thresholds.clear();
  options.min_neighbours = 28;
  EXPECT_THROW((void)segmentMaterials(dataset, dir_ / "c", options), std::invalid_argument);
  options.min_neighbours = 6;
  options.names = {"a", "b", "c"};
  EXPECT_THROW((void)segmentMaterials(dataset, dir_ / "d", options), std::invalid_argument);
}

TEST_F(MaterialsTest, KeepsNamesAndColoursOfTheClasses) {
  const Scene scene = makeScene();
  DatasetOptions sieve;
  sieve.threshold = kThreshold;
  (void)writeDataset(MemorySource(scene.grey), dir_ / "dataset", sieve);
  const Dataset dataset = Dataset::open(dir_ / "dataset");
  SegmentationOptions options;
  options.names = {"Polymer"};  // the second class keeps its default name
  options.colors = {{10, 20, 30}, {200, 100, 50}};
  (void)segmentMaterials(dataset, dir_ / "materials", options);
  const MaterialVolumeInfo info = readMaterialVolumeInfo(dir_ / "materials");
  ASSERT_EQ(info.materials.size(), 2U);
  EXPECT_EQ(info.materials[0].name, "Polymer");
  EXPECT_EQ(info.materials[1].name, "Material 2");
  EXPECT_EQ(info.materials[0].color, (std::array<std::uint8_t, 3>{10, 20, 30}));
  EXPECT_EQ(info.materials[1].color, (std::array<std::uint8_t, 3>{200, 100, 50}));
}

}  // namespace
}  // namespace voxelsieve
