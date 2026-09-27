#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "voxelsieve/volume.hpp"

namespace voxelsieve {

/// Spherical void inside the material. Coordinates are relative to the volume centre.
struct Pore {
  std::array<double, 3> center_mm{0.0, 0.0, 0.0};
  double radius_mm = 0.0;
};

/// Synthetic CT phantom: a hollow box centred in the volume with spherical pores in its walls.
/// The closed inner cavity and the pores are internal air that a sieve must keep; everything
/// outside the box is outside air. All geometry is known analytically, so the phantom serves as
/// ground truth for tests and benchmarks.
struct PhantomSpec {
  std::array<std::int64_t, 3> dims{128, 128, 128};
  double voxel_size_mm = 0.1;
  std::uint16_t air_value = 1000;
  std::uint16_t material_value = 20000;
  /// Standard deviation of additive Gaussian noise in grey values; 0 disables noise.
  double noise_sigma = 0.0;
  std::uint64_t seed = 42;
  /// Outer edge lengths of the box.
  std::array<double, 3> outer_size_mm{8.0, 8.0, 8.0};
  /// Wall thickness of the box; values <= 0 produce a solid box.
  double wall_thickness_mm = 1.5;
  /// Pores are assumed to lie fully inside the material and not to overlap.
  std::vector<Pore> pores;
};

/// Default phantom with three pores of different sizes in the box walls.
[[nodiscard]] PhantomSpec defaultPhantomSpec();

/// Signed distance in mm from `point_mm` (relative to the volume centre) to the material
/// surface: negative inside material, positive in air.
[[nodiscard]] double phantomSignedDistanceMm(const PhantomSpec& spec,
                                             const std::array<double, 3>& point_mm);

/// Analytic material volume in mm^3.
[[nodiscard]] double phantomMaterialVolumeMm3(const PhantomSpec& spec);

/// Centre of voxel (x, y, z) in mm relative to the volume centre.
[[nodiscard]] std::array<double, 3> voxelCenterMm(const PhantomSpec& spec, std::int64_t x,
                                                  std::int64_t y, std::int64_t z);

/// Grey value of voxel (x, y, z). Edge voxels get a partial-volume value between air and
/// material. Noise depends only on the seed and the voxel index, so voxels can be generated in any
/// order and the result is identical on every platform.
[[nodiscard]] std::uint16_t phantomValue(const PhantomSpec& spec, std::int64_t x, std::int64_t y,
                                         std::int64_t z);

/// Renders the whole phantom into memory.
[[nodiscard]] Volume16 generatePhantom(const PhantomSpec& spec);

}  // namespace voxelsieve
