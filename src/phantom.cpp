#include "voxelsieve/phantom.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <random>

namespace voxelsieve {
namespace {

double boxSignedDistance(const std::array<double, 3>& p, const std::array<double, 3>& half) {
  double outside_sq = 0.0;
  double inside = -std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < 3; ++i) {
    const double q = std::abs(p[i]) - half[i];
    outside_sq += std::max(q, 0.0) * std::max(q, 0.0);
    inside = std::max(inside, q);
  }
  return std::sqrt(outside_sq) + std::min(inside, 0.0);
}

double sphereSignedDistance(const std::array<double, 3>& p, const Pore& pore) {
  const double dx = p[0] - pore.center_mm[0];
  const double dy = p[1] - pore.center_mm[1];
  const double dz = p[2] - pore.center_mm[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz) - pore.radius_mm;
}

std::array<double, 3> halfExtents(const std::array<double, 3>& size, double shrink) {
  return {size[0] / 2.0 - shrink, size[1] / 2.0 - shrink, size[2] / 2.0 - shrink};
}

bool isHollow(const PhantomSpec& spec) { return spec.wall_thickness_mm > 0.0; }

}  // namespace

PhantomSpec defaultPhantomSpec() {
  PhantomSpec spec;
  // The walls span |coordinate| in [2.5, 4.0] mm, so the wall mid-plane is at 3.25 mm.
  spec.pores = {
      {{3.25, 0.0, 0.0}, 0.40},
      {{-3.25, 1.5, -1.0}, 0.30},
      {{0.5, -0.5, 3.25}, 0.20},
  };
  return spec;
}

double phantomSignedDistanceMm(const PhantomSpec& spec, const std::array<double, 3>& point_mm) {
  double distance = boxSignedDistance(point_mm, halfExtents(spec.outer_size_mm, 0.0));
  if (isHollow(spec)) {
    const auto inner_half = halfExtents(spec.outer_size_mm, spec.wall_thickness_mm);
    distance = std::max(distance, -boxSignedDistance(point_mm, inner_half));
  }
  for (const Pore& pore : spec.pores) {
    distance = std::max(distance, -sphereSignedDistance(point_mm, pore));
  }
  return distance;
}

double phantomMaterialVolumeMm3(const PhantomSpec& spec) {
  const auto& outer = spec.outer_size_mm;
  double volume = outer[0] * outer[1] * outer[2];
  if (isHollow(spec)) {
    const double t = 2.0 * spec.wall_thickness_mm;
    volume -= (outer[0] - t) * (outer[1] - t) * (outer[2] - t);
  }
  for (const Pore& pore : spec.pores) {
    volume -= 4.0 / 3.0 * std::numbers::pi * std::pow(pore.radius_mm, 3);
  }
  return volume;
}

std::array<double, 3> voxelCenterMm(const PhantomSpec& spec, std::int64_t x, std::int64_t y,
                                    std::int64_t z) {
  const std::array<std::int64_t, 3> index{x, y, z};
  std::array<double, 3> center{};
  for (std::size_t i = 0; i < 3; ++i) {
    center[i] = (static_cast<double>(index[i]) + 0.5 - static_cast<double>(spec.dims[i]) / 2.0) *
                spec.voxel_size_mm;
  }
  return center;
}

Volume16 generatePhantom(const PhantomSpec& spec) {
  Volume16 volume(spec.dims, spec.voxel_size_mm);
  std::mt19937_64 rng(spec.seed);
  std::normal_distribution<double> noise(0.0, spec.noise_sigma > 0.0 ? spec.noise_sigma : 1.0);
  const double air = spec.air_value;
  const double contrast = static_cast<double>(spec.material_value) - air;

  for (std::int64_t z = 0; z < spec.dims[2]; ++z) {
    for (std::int64_t y = 0; y < spec.dims[1]; ++y) {
      for (std::int64_t x = 0; x < spec.dims[0]; ++x) {
        const double distance = phantomSignedDistanceMm(spec, voxelCenterMm(spec, x, y, z));
        // Linear partial-volume model: fraction of the voxel covered by material.
        const double fraction = std::clamp(0.5 - distance / spec.voxel_size_mm, 0.0, 1.0);
        double value = air + fraction * contrast;
        if (spec.noise_sigma > 0.0) {
          value += noise(rng);
        }
        volume.at(x, y, z) =
            static_cast<std::uint16_t>(std::clamp(std::round(value), 0.0, 65535.0));
      }
    }
  }
  return volume;
}

}  // namespace voxelsieve
