#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "voxelsieve/phantom.hpp"

namespace voxelsieve {
namespace {

// Index of the voxel whose centre lies closest to `mm` along one axis.
std::int64_t voxelIndex(const PhantomSpec& spec, std::size_t axis, double mm) {
  return static_cast<std::int64_t>(
      std::floor(mm / spec.voxel_size[axis] + static_cast<double>(spec.dims[axis]) / 2.0));
}

TEST(Phantom, HasRequestedDimensions) {
  const PhantomSpec spec = defaultPhantomSpec();
  const Volume16 volume = generatePhantom(spec);
  EXPECT_EQ(volume.dims, spec.dims);
  EXPECT_EQ(volume.voxelCount(), 128U * 128U * 128U);
  EXPECT_EQ(volume.voxel_size, spec.voxel_size);
}

TEST(Phantom, SignedDistanceHasCorrectSign) {
  const PhantomSpec spec = defaultPhantomSpec();
  EXPECT_GT(phantomSignedDistanceMm(spec, {6.0, 0.0, 0.0}), 0.0);   // outside air
  EXPECT_GT(phantomSignedDistanceMm(spec, {0.0, 0.0, 0.0}), 0.0);   // inner cavity
  EXPECT_LT(phantomSignedDistanceMm(spec, {0.0, 3.25, 0.0}), 0.0);  // wall
  EXPECT_GT(phantomSignedDistanceMm(spec, spec.pores[0].center_mm), 0.0);
  EXPECT_NEAR(phantomSignedDistanceMm(spec, {0.0, 4.0, 0.0}), 0.0, 1e-12);  // outer surface
}

TEST(Phantom, NoiselessValuesMatchRegions) {
  const PhantomSpec spec = defaultPhantomSpec();
  const Volume16 volume = generatePhantom(spec);
  const auto at_mm = [&](double x, double y, double z) {
    return volume.at(voxelIndex(spec, 0, x), voxelIndex(spec, 1, y), voxelIndex(spec, 2, z));
  };
  EXPECT_EQ(volume.at(0, 0, 0), spec.air_value);
  EXPECT_EQ(at_mm(0.05, 0.05, 0.05), spec.air_value);       // inner cavity
  EXPECT_EQ(at_mm(0.05, 3.25, 0.05), spec.material_value);  // wall
  EXPECT_EQ(at_mm(3.25, 0.05, 0.05), spec.air_value);       // centre of the largest pore
}

TEST(Phantom, MaterialVolumeMatchesAnalyticValue) {
  const PhantomSpec spec = defaultPhantomSpec();
  const Volume16 volume = generatePhantom(spec);
  const auto contrast = static_cast<double>(spec.material_value - spec.air_value);
  double material_fraction_sum = 0.0;
  for (const std::uint16_t value : volume.data) {
    material_fraction_sum += (static_cast<double>(value) - spec.air_value) / contrast;
  }
  const double voxel_volume = spec.voxel_size.volumeMm3();
  EXPECT_NEAR(material_fraction_sum * voxel_volume, phantomMaterialVolumeMm3(spec),
              0.01 * phantomMaterialVolumeMm3(spec));
}

TEST(Phantom, SolidBoxHasNoCavity) {
  PhantomSpec spec = defaultPhantomSpec();
  spec.wall_thickness_mm = 0.0;
  spec.pores.clear();
  EXPECT_LT(phantomSignedDistanceMm(spec, {0.0, 0.0, 0.0}), 0.0);
  EXPECT_DOUBLE_EQ(phantomMaterialVolumeMm3(spec), 512.0);
}

TEST(Phantom, NoiseIsReproducibleAndCentred) {
  PhantomSpec spec = defaultPhantomSpec();
  spec.dims = {32, 32, 32};
  spec.noise_sigma = 200.0;
  const Volume16 first = generatePhantom(spec);
  const Volume16 second = generatePhantom(spec);
  EXPECT_EQ(first.data, second.data);

  // Corner region is pure outside air: mean close to the air value, spread close to sigma.
  double sum = 0.0;
  double sum_sq = 0.0;
  int count = 0;
  for (std::int64_t z = 0; z < 4; ++z) {
    for (std::int64_t y = 0; y < 32; ++y) {
      for (std::int64_t x = 0; x < 32; ++x) {
        const double value = first.at(x, y, z);
        sum += value;
        sum_sq += value * value;
        ++count;
      }
    }
  }
  const double mean = sum / count;
  const double sigma = std::sqrt(sum_sq / count - mean * mean);
  EXPECT_NEAR(mean, spec.air_value, 20.0);
  EXPECT_NEAR(sigma, spec.noise_sigma, 20.0);
}

}  // namespace
}  // namespace voxelsieve
