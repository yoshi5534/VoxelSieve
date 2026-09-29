#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "material_scene.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/materials.hpp"
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

TEST_F(MaterialsTest, RejectsBadOptions) {
  const Scene scene = makeScene();
  DatasetOptions sieve;
  sieve.threshold = kThreshold;
  (void)writeDataset(MemorySource(scene.grey), dir_ / "dataset", sieve);
  const Dataset dataset = Dataset::open(dir_ / "dataset");
  SegmentationOptions options;
  options.materials = 0;
  EXPECT_THROW((void)segmentMaterials(dataset, dir_ / "a", options), std::invalid_argument);
  options.materials = 2;
  options.material_thresholds = {1.0F, 2.0F};
  EXPECT_THROW((void)segmentMaterials(dataset, dir_ / "b", options), std::invalid_argument);
  options.material_thresholds.clear();
  options.min_neighbours = 28;
  EXPECT_THROW((void)segmentMaterials(dataset, dir_ / "c", options), std::invalid_argument);
}

}  // namespace
}  // namespace voxelsieve
