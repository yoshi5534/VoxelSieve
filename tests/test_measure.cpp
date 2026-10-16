#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "material_scene.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/measure.hpp"
#include "voxelsieve/source.hpp"

namespace voxelsieve {
namespace {

constexpr std::array<std::int64_t, 3> kDims{64, 64, 64};
constexpr VoxelSize kPitch(0.1, 0.1, 0.2);

bool in(std::int64_t v, std::int64_t lo, std::int64_t hi) { return v >= lo && v < hi; }

/// A block of material 1 (32 x 16 x 12 voxels, 3.2 x 1.6 x 2.4 mm) with a closed cavity
/// (8 x 8 x 6), a small cube of material 1 and a bar of material 2.
std::uint8_t labelAt(std::int64_t x, std::int64_t y, std::int64_t z) {
  if (in(x, 8, 40) && in(y, 8, 24) && in(z, 8, 20)) {
    return in(x, 16, 24) && in(y, 12, 20) && in(z, 11, 17) ? 0 : 1;
  }
  if (in(x, 48, 56) && in(y, 40, 48) && in(z, 40, 48)) {
    return 1;
  }
  if (in(x, 10, 14) && in(y, 40, 56) && in(z, 40, 44)) {
    return 2;
  }
  return 0;
}

std::vector<std::uint8_t> labels() {
  std::vector<std::uint8_t> out;
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    for (std::int64_t y = 0; y < kDims[1]; ++y) {
      for (std::int64_t x = 0; x < kDims[0]; ++x) {
        out.push_back(labelAt(x, y, z));
      }
    }
  }
  return out;
}

std::vector<Material> materials() {
  Material light;
  light.id = 1;
  light.name = "Light";
  Material dense;
  dense.id = 2;
  dense.name = "Dense";
  return {light, dense};
}

TEST(MeasureObjects, MeasuresEveryObjectOfEachMaterial) {
  MeasureOptions options;
  options.axis = std::array<double, 3>{0.0, 0.0, 1.0};
  const auto all = labels();
  const MeasureInfo info = measureObjects(all, kDims, kPitch, materials(), options);
  EXPECT_EQ(info.stride, 1);
  ASSERT_EQ(info.objects.size(), 3U);

  // The block: its voxels without the cavity, centroid shifted away from the cavity.
  const MeasuredObject& block = info.objects[0];
  EXPECT_EQ(block.material, 1);
  EXPECT_EQ(block.material_name, "Light");
  constexpr std::int64_t kBlock = 32 * 16 * 12 - 8 * 8 * 6;
  EXPECT_EQ(block.voxels, kBlock);
  EXPECT_NEAR(block.volume_mm3, kBlock * 0.002, 1e-9);
  std::array<double, 3> sum{};
  for (std::int64_t z = 8; z < 20; ++z) {
    for (std::int64_t y = 8; y < 24; ++y) {
      for (std::int64_t x = 8; x < 40; ++x) {
        if (labelAt(x, y, z) == 1) {
          sum[0] += static_cast<double>(x) * 0.1;
          sum[1] += static_cast<double>(y) * 0.1;
          sum[2] += static_cast<double>(z) * 0.2;
        }
      }
    }
  }
  for (std::size_t a = 0; a < 3; ++a) {
    EXPECT_NEAR(block.centroid_mm[a], sum[a] / kBlock, 1e-9) << a;
  }
  EXPECT_NEAR(block.min_mm[0], 0.75, 1e-9);  // voxel 8's face
  EXPECT_NEAR(block.max_mm[2], 3.9, 1e-9);   // voxel 19's face
  // Longest along x (3.2 mm), then z (2.4 mm), then y (1.6 mm).
  EXPECT_NEAR(std::abs(block.principal_axes[0][0]), 1.0, 1e-6);
  EXPECT_NEAR(std::abs(block.principal_axes[1][2]), 1.0, 1e-6);
  EXPECT_NEAR(block.principal_lengths_mm[0], 3.2, 1e-6);
  EXPECT_NEAR(block.principal_lengths_mm[1], 2.4, 1e-6);
  EXPECT_NEAR(block.principal_lengths_mm[2], 1.6, 1e-6);
  EXPECT_NEAR(block.axis_length_mm.value_or(0.0), 2.4, 1e-6);
  // Outer and cavity faces; marching cubes rounds the edges a little.
  const double faces = 2 * (3.2 * 1.6 + 3.2 * 2.4 + 1.6 * 2.4) + 2 * (0.8 * 0.8 + 0.8 * 1.2 * 2);
  EXPECT_NEAR(block.surface_mm2, faces, 0.08 * faces);
  EXPECT_FALSE(block.touches_border);

  EXPECT_EQ(info.objects[1].material, 1);  // the cube, 8^3
  EXPECT_EQ(info.objects[1].voxels, 512);
  EXPECT_EQ(info.objects[2].material, 2);  // the bar, 4 x 16 x 4
  EXPECT_EQ(info.objects[2].voxels, 256);
  EXPECT_NEAR(info.objects[2].principal_lengths_mm[0], 1.6, 1e-6);
}

TEST(MeasureObjects, MeasuresEnclosedAirAndFilters) {
  const auto all = labels();
  MeasureOptions options;
  options.material_ids = {0};
  const MeasureInfo voids = measureObjects(all, kDims, kPitch, materials(), options);
  ASSERT_EQ(voids.objects.size(), 1U);  // the cavity, not the air around
  EXPECT_EQ(voids.objects[0].material_name, "Enclosed air");
  EXPECT_EQ(voids.objects[0].voxels, 8 * 8 * 6);
  EXPECT_NEAR(voids.objects[0].principal_lengths_mm[0], 1.2, 1e-6);

  options.material_ids = {1};
  options.min_voxels = 1000;
  EXPECT_EQ(measureObjects(all, kDims, kPitch, materials(), options).objects.size(), 1U);
  options.min_voxels = 1;
  options.max_objects = 1;
  const MeasureInfo largest = measureObjects(all, kDims, kPitch, materials(), options);
  EXPECT_EQ(largest.objects_found, 2);
  ASSERT_EQ(largest.objects.size(), 1U);
  EXPECT_GT(largest.objects[0].voxels, 512);

  options.material_ids = {5};
  EXPECT_THROW((void)measureObjects(all, kDims, kPitch, materials(), options),
               std::invalid_argument);
  options.material_ids = {};
  options.axis = std::array<double, 3>{0.0, 0.0, 0.0};
  EXPECT_THROW((void)measureObjects(all, kDims, kPitch, materials(), options),
               std::invalid_argument);

  const auto file = std::filesystem::temp_directory_path() / "voxelsieve_measure.csv";
  writeObjectsCsv(file, voids);
  std::string header;
  std::string row;
  {
    std::ifstream in(file);  // closed before the file is removed (Windows)
    std::getline(in, header);
    std::getline(in, row);
  }
  EXPECT_TRUE(header.starts_with("object,material,material_name,voxels,volume_mm3"));
  EXPECT_TRUE(row.starts_with("1,0,\"Enclosed air\",384,"));
  std::filesystem::remove(file);
}

TEST(MeasureObjects, MeasuresASegmentedScan) {
  const auto dir = std::filesystem::temp_directory_path() / "voxelsieve_measure_scan";
  std::filesystem::remove_all(dir);
  const Scene scene = makeScene();
  DatasetOptions sieve;
  sieve.threshold = kThreshold;
  (void)writeDataset(MemorySource(scene.grey), dir / "dataset", sieve);
  SegmentationOptions segmentation;
  segmentation.materials = 2;
  segmentation.material_thresholds = {14000.0F};
  (void)segmentMaterials(Dataset::open(dir / "dataset"), dir / "materials", segmentation);
  const MaterialVolume volume = MaterialVolume::open(dir / "materials");
  MeasureOptions options;
  options.material_ids = {2};
  const MeasureInfo dense = measureObjects(volume, options);
  ASSERT_EQ(dense.objects.size(), 1U);  // the dense cube, 16^3 voxels of 0.1 mm
  EXPECT_NEAR(dense.objects[0].volume_mm3, 4.096, 0.4);
  EXPECT_NEAR(dense.objects[0].centroid_mm[0], 5.15, 0.15);
  // Sampled when the volume is large: every 2nd voxel.
  options.max_samples = std::int64_t{48} * 48 * 48;
  const MeasureInfo coarse = measureObjects(volume, options);
  EXPECT_EQ(coarse.stride, 2);
  ASSERT_EQ(coarse.objects.size(), 1U);
  EXPECT_NEAR(coarse.objects[0].volume_mm3, 4.096, 0.6);
  std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace voxelsieve
