#include "voxelsieve/phantom.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>

#include "detail/noise.hpp"

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
                spec.voxel_size[i];
  }
  return center;
}

std::uint16_t phantomValue(const PhantomSpec& spec, std::int64_t x, std::int64_t y,
                           std::int64_t z) {
  const double air = spec.air_value;
  const double contrast = static_cast<double>(spec.material_value) - air;
  const std::array<double, 3> center = voxelCenterMm(spec, x, y, z);
  const double distance = phantomSignedDistanceMm(spec, center);
  // Linear partial-volume model: fraction of the voxel covered by material. The voxel's width
  // across the surface is its extent along the surface normal, which for box faces is exactly the
  // pitch of the axis they are perpendicular to.
  double width = spec.voxel_size[0];
  if (!spec.voxel_size.isotropic() && std::abs(distance) < spec.voxel_size.maxMm()) {
    std::array<double, 3> normal{};
    double length = 0.0;
    const double h = spec.voxel_size.minMm() * 0.25;
    for (std::size_t a = 0; a < 3; ++a) {
      std::array<double, 3> plus = center;
      std::array<double, 3> minus = center;
      plus[a] += h;
      minus[a] -= h;
      normal[a] = phantomSignedDistanceMm(spec, plus) - phantomSignedDistanceMm(spec, minus);
      length += normal[a] * normal[a];
    }
    if (length > 0.0) {
      width = 0.0;
      for (std::size_t a = 0; a < 3; ++a) {
        width += std::abs(normal[a]) / std::sqrt(length) * spec.voxel_size[a];
      }
    }
  }
  const double fraction = std::clamp(0.5 - distance / width, 0.0, 1.0);
  double value = air + fraction * contrast;
  if (spec.noise_sigma > 0.0) {
    const auto index = static_cast<std::uint64_t>(x + spec.dims[0] * (y + spec.dims[1] * z));
    value += spec.noise_sigma * detail::gaussianNoise(spec.seed, index);
  }
  return static_cast<std::uint16_t>(std::clamp(std::round(value), 0.0, 65535.0));
}

Volume16 generatePhantom(const PhantomSpec& spec) {
  Volume16 volume(spec.dims, spec.voxel_size);
  for (std::int64_t z = 0; z < spec.dims[2]; ++z) {
    for (std::int64_t y = 0; y < spec.dims[1]; ++y) {
      for (std::int64_t x = 0; x < spec.dims[0]; ++x) {
        volume.at(x, y, z) = phantomValue(spec, x, y, z);
      }
    }
  }
  return volume;
}

}  // namespace voxelsieve
