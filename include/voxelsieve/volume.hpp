#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// Dense 16-bit grey-value volume as produced by CT reconstruction.
/// Voxels are stored with x varying fastest, then y, then z.
struct Volume16 {
  std::array<std::int64_t, 3> dims{0, 0, 0};
  VoxelSize voxel_size;
  std::vector<std::uint16_t> data;

  Volume16() = default;
  Volume16(std::array<std::int64_t, 3> volume_dims, const VoxelSize& size)
      : dims(volume_dims),
        voxel_size(size),
        data(static_cast<std::size_t>(volume_dims[0] * volume_dims[1] * volume_dims[2])) {}

  [[nodiscard]] std::size_t voxelCount() const { return data.size(); }

  [[nodiscard]] std::size_t index(std::int64_t x, std::int64_t y, std::int64_t z) const {
    return static_cast<std::size_t>(x + dims[0] * (y + dims[1] * z));
  }

  [[nodiscard]] std::uint16_t at(std::int64_t x, std::int64_t y, std::int64_t z) const {
    return data[index(x, y, z)];
  }
  std::uint16_t& at(std::int64_t x, std::int64_t y, std::int64_t z) { return data[index(x, y, z)]; }
};

}  // namespace voxelsieve
