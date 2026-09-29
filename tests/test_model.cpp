#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "material_scene.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/model.hpp"
#include "voxelsieve/source.hpp"

namespace voxelsieve {
namespace {

using Dims = std::array<std::int64_t, 3>;

ModelLayer conv(const std::string& name, const std::string& input, int in, int out, int kernel,
                bool relu, std::mt19937& rng) {
  std::normal_distribution<float> weight(0.0F, 0.4F);
  ModelLayer layer{"conv", name, {input}, in, out, kernel, relu, {}, {}};
  layer.weights.resize(static_cast<std::size_t>(out) * static_cast<std::size_t>(in) *
                       static_cast<std::size_t>(kernel * kernel * kernel));
  layer.bias.resize(static_cast<std::size_t>(out));
  for (float& w : layer.weights) {
    w = weight(rng);
  }
  for (float& b : layer.bias) {
    b = weight(rng);
  }
  return layer;
}

Material material(int id, float lower) {
  Material m;
  m.id = id;
  m.name = "Material " + std::to_string(id);
  m.lower = lower;
  return m;
}

// A small U-Net with every layer type and random weights.
ModelSpec randomUNet() {
  std::mt19937 rng(3);
  ModelSpec spec;
  spec.name = "test";
  spec.input_offset = 100.0F;
  spec.input_scale = 0.01F;
  spec.divisor = 2;
  spec.halo = 4;
  spec.materials = {material(1, 150.0F), material(2, 250.0F)};
  spec.layers = {conv("a", "input", 1, 3, 3, true, rng),
                 {"maxpool", "p", {"a"}, 0, 0, 1, false, {}, {}},
                 conv("b", "p", 3, 2, 3, true, rng),
                 {"upsample", "u", {"b"}, 0, 0, 1, false, {}, {}},
                 {"concat", "c", {"u", "a"}, 0, 0, 1, false, {}, {}},
                 conv("scores", "c", 5, 3, 1, false, rng)};
  return spec;
}

// Straightforward reference implementations of the layers.
struct Map {
  int channels;
  Dims dims;
  std::vector<float> data;
  float& at(int c, std::int64_t x, std::int64_t y, std::int64_t z) {
    return data[static_cast<std::size_t>(((c * dims[2] + z) * dims[1] + y) * dims[0] + x)];
  }
};

Map referenceConv(Map& in, const ModelLayer& layer) {
  Map out{layer.out_channels, in.dims,
          std::vector<float>(
              static_cast<std::size_t>(layer.out_channels * in.dims[0] * in.dims[1] * in.dims[2]))};
  const int k = layer.kernel;
  const int r = k / 2;
  for (int o = 0; o < layer.out_channels; ++o) {
    for (std::int64_t z = 0; z < in.dims[2]; ++z) {
      for (std::int64_t y = 0; y < in.dims[1]; ++y) {
        for (std::int64_t x = 0; x < in.dims[0]; ++x) {
          double sum = layer.bias[static_cast<std::size_t>(o)];
          for (int i = 0; i < layer.in_channels; ++i) {
            for (int kz = 0; kz < k; ++kz) {
              for (int ky = 0; ky < k; ++ky) {
                for (int kx = 0; kx < k; ++kx) {
                  const std::int64_t sx = x + kx - r;
                  const std::int64_t sy = y + ky - r;
                  const std::int64_t sz = z + kz - r;
                  if (sx < 0 || sy < 0 || sz < 0 || sx >= in.dims[0] || sy >= in.dims[1] ||
                      sz >= in.dims[2]) {
                    continue;
                  }
                  const std::int64_t w =
                      (((std::int64_t{o} * layer.in_channels + i) * k + kz) * k + ky) * k + kx;
                  sum += layer.weights[static_cast<std::size_t>(w)] * in.at(i, sx, sy, sz);
                }
              }
            }
          }
          out.at(o, x, y, z) =
              layer.relu ? std::max(static_cast<float>(sum), 0.0F) : static_cast<float>(sum);
        }
      }
    }
  }
  return out;
}

Map referenceForward(const ModelSpec& spec, const std::vector<float>& grey, const Dims& dims) {
  Map input{1, dims, grey};
  for (float& v : input.data) {
    v = (v - spec.input_offset) * spec.input_scale;
  }
  Map a = referenceConv(input, spec.layers[0]);
  const Dims half{dims[0] / 2, dims[1] / 2, dims[2] / 2};
  Map p{3, half, std::vector<float>(static_cast<std::size_t>(3 * half[0] * half[1] * half[2]))};
  for (int c = 0; c < 3; ++c) {
    for (std::int64_t z = 0; z < half[2]; ++z) {
      for (std::int64_t y = 0; y < half[1]; ++y) {
        for (std::int64_t x = 0; x < half[0]; ++x) {
          float best = a.at(c, 2 * x, 2 * y, 2 * z);
          for (int d = 1; d < 8; ++d) {
            best = std::max(
                best, a.at(c, 2 * x + (d & 1), 2 * y + ((d >> 1) & 1), 2 * z + ((d >> 2) & 1)));
          }
          p.at(c, x, y, z) = best;
        }
      }
    }
  }
  Map b = referenceConv(p, spec.layers[2]);
  Map c{5, dims, std::vector<float>(static_cast<std::size_t>(5 * dims[0] * dims[1] * dims[2]))};
  for (std::int64_t z = 0; z < dims[2]; ++z) {
    for (std::int64_t y = 0; y < dims[1]; ++y) {
      for (std::int64_t x = 0; x < dims[0]; ++x) {
        for (int ch = 0; ch < 2; ++ch) {
          c.at(ch, x, y, z) = b.at(ch, x / 2, y / 2, z / 2);
        }
        for (int ch = 0; ch < 3; ++ch) {
          c.at(2 + ch, x, y, z) = a.at(ch, x, y, z);
        }
      }
    }
  }
  return referenceConv(c, spec.layers[5]);
}

// Scores of a threshold segmentation as a model: a 3^3 filter that weighs the voxel as much as its
// 26 neighbours together (a sheet one voxel thin stays above the threshold, a lone noise spike
// does not), then class scores linear in the filtered value m (in thousands): air 0, light
// m - 4.5 and dense 2 m - 18.5, so light wins from 4500 and dense from 14000 on.
ModelSpec thresholdModel(float light, float dense) {
  ModelSpec spec;
  spec.name = "threshold";
  spec.input_scale = 1e-3F;
  spec.halo = 1;
  spec.materials = {material(1, light), material(2, dense)};
  ModelLayer filter{"conv", "m", {"input"}, 1, 1, 3, false, std::vector<float>(27, 0.5F / 26.0F),
                    {0.0F}};
  filter.weights[13] = 0.5F;
  const float l = light * 1e-3F;
  const float d = dense * 1e-3F;
  spec.layers = {filter,
                 {"conv", "scores", {"m"}, 1, 3, 1, false, {0.0F, 1.0F, 2.0F}, {0.0F, -l, -l - d}}};
  return spec;
}

class ModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_model_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

TEST_F(ModelTest, LayersMatchReferenceImplementation) {
  const ModelSpec spec = randomUNet();
  const Model model(spec);
  const Dims dims{10, 6, 8};
  std::mt19937 rng(5);
  std::uniform_real_distribution<float> value(0.0F, 400.0F);
  std::vector<float> grey(static_cast<std::size_t>(dims[0] * dims[1] * dims[2]));
  for (float& v : grey) {
    v = value(rng);
  }
  const Tensor scores = model.forward(grey, dims);
  const Map reference = referenceForward(spec, grey, dims);
  ASSERT_EQ(scores.channels, 3);
  ASSERT_EQ(scores.dims, dims);
  ASSERT_EQ(scores.data.size(), reference.data.size());
  for (std::size_t i = 0; i < reference.data.size(); ++i) {
    ASSERT_NEAR(scores.data[i], reference.data[i], 1e-4F) << i;
  }

  // classify pads odd sizes and takes the best class per voxel.
  const std::vector<std::uint8_t> classes = model.classify(grey, dims, 0.0F);
  const std::int64_t n = dims[0] * dims[1] * dims[2];
  for (std::int64_t v = 0; v < n; ++v) {
    int best = 0;
    for (int c = 1; c < 3; ++c) {
      if (reference.data[static_cast<std::size_t>(c * n + v)] >
          reference.data[static_cast<std::size_t>(best * n + v)]) {
        best = c;
      }
    }
    ASSERT_EQ(classes[static_cast<std::size_t>(v)], best);
  }
  EXPECT_EQ(model.classify(std::vector<float>(105, 0.0F), {5, 3, 7}, 0.0F).size(), 105U);
  EXPECT_THROW((void)model.forward(std::vector<float>(240), {5, 6, 8}), std::invalid_argument);
}

TEST_F(ModelTest, RoundTripsThroughFile) {
  const ModelSpec spec = randomUNet();
  writeModel(dir_ / "m.vsm", spec);
  const Model loaded = Model::load(dir_ / "m.vsm");
  EXPECT_EQ(loaded.spec().name, "test");
  EXPECT_EQ(loaded.spec().halo, 4);
  EXPECT_EQ(loaded.spec().materials[1].lower, 250.0F);
  const Dims dims{4, 4, 4};
  const std::vector<float> grey(64, 180.0F);
  EXPECT_EQ(loaded.forward(grey, dims).data, Model(spec).forward(grey, dims).data);

  // A truncated file and a foreign file are refused.
  const auto size = std::filesystem::file_size(dir_ / "m.vsm");
  std::filesystem::copy_file(dir_ / "m.vsm", dir_ / "short.vsm");
  std::filesystem::resize_file(dir_ / "short.vsm", size - 4);
  EXPECT_THROW((void)Model::load(dir_ / "short.vsm"), std::runtime_error);
  std::ofstream(dir_ / "other.vsm") << "not a model";
  EXPECT_THROW((void)Model::load(dir_ / "other.vsm"), std::runtime_error);
}

TEST_F(ModelTest, RejectsInvalidGraphs) {
  ModelSpec spec = randomUNet();
  spec.layers[2].inputs = {"missing"};
  EXPECT_THROW(Model{spec}, std::invalid_argument);
  spec = randomUNet();
  spec.layers[2].in_channels = 4;  // gets 3
  EXPECT_THROW(Model{spec}, std::invalid_argument);
  spec = randomUNet();
  spec.materials.pop_back();  // 3 scores for 1 material
  EXPECT_THROW(Model{spec}, std::invalid_argument);
  spec = randomUNet();
  spec.divisor = 1;  // one pooling step needs even sizes
  EXPECT_THROW(Model{spec}, std::invalid_argument);
  spec = randomUNet();
  spec.layers[4].inputs = {"b", "a"};  // half and full resolution
  EXPECT_THROW(Model{spec}, std::invalid_argument);
}

TEST_F(ModelTest, SegmentsSceneTileByTile) {
  const Scene scene = makeScene();
  DatasetOptions sieve;
  sieve.threshold = kThreshold;
  sieve.brick_size = 32;
  sieve.min_material_voxels = 4;
  (void)writeDataset(MemorySource(scene.grey), dir_ / "dataset", sieve);
  const Dataset dataset = Dataset::open(dir_ / "dataset");
  const Model model(thresholdModel(kThreshold, 14000.0F));

  ModelSegmentationOptions options;
  options.tile = 20;  // tiles cross the bricks of 32
  const MaterialVolumeInfo info =
      segmentMaterialsWithModel(dataset, model, dir_ / "materials", options);
  EXPECT_EQ(info.model, "threshold");
  EXPECT_EQ(readMaterialVolumeInfo(dir_ / "materials").model, "threshold");
  ASSERT_EQ(info.materials.size(), 2U);

  // Tiles with a halo covering the receptive field give the same classes as one pass over the
  // whole volume.
  const Box whole{{0, 0, 0}, {kSize, kSize, kSize}};
  std::vector<float> grey(static_cast<std::size_t>(whole.voxelCount()));
  dataset.readRegion(0, whole, grey, dataset.info().air_level);
  const std::vector<std::uint8_t> expected =
      model.classify(grey, {kSize, kSize, kSize}, dataset.info().air_level);
  const MaterialVolume volume = MaterialVolume::open(dir_ / "materials");
  std::vector<std::uint8_t> ids(expected.size());
  volume.readRegion(whole, ids);
  EXPECT_EQ(ids, expected);
  EXPECT_EQ(info.materials[0].voxel_count + info.materials[1].voxel_count,
            std::count_if(ids.begin(), ids.end(), [](std::uint8_t id) { return id != 0; }));

  // Against the analytic labels.
  const MaterialScore score = scoreMaterials(volume, MemorySource(scene.labels), dataset);
  EXPECT_EQ(score.components_per_material, (std::vector<std::int64_t>{0, 2, 1}));
  EXPECT_GT(score.dice[0], 0.95);
  EXPECT_GT(score.dice[1], 0.9);
  EXPECT_GT(score.dice[2], 0.95);

  // Scoring a region: left of x = 40 there is no dense cube.
  const MaterialScore left = scoreMaterials(volume, MemorySource(scene.labels), dataset, {}, 2,
                                            Box{{0, 0, 0}, {40, kSize, kSize}});
  EXPECT_EQ(left.components_per_material, (std::vector<std::int64_t>{0, 2, 0}));
  EXPECT_THROW((void)scoreMaterials(volume, MemorySource(scene.labels), dataset, {}, 2,
                                    Box{{kSize, 0, 0}, {kSize + 5, kSize, kSize}}),
               std::invalid_argument);
}

}  // namespace
}  // namespace voxelsieve
