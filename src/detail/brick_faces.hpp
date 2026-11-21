#pragma once

#include <openvdb/openvdb.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "voxelsieve/dataset.hpp"

namespace voxelsieve::detail {

/// Which voxels on the six faces of every brick of a level are active: one bit per face voxel,
/// about 48 KB per 256³ brick (200 MB at 4096³). A pass that asks whether the voxels just across
/// a brick face are active records the faces of each brick as it reads it and answers from them,
/// instead of reading the neighbouring bricks again for a layer one voxel thick.
///
/// `record` may run for different bricks at once; `active` is for after the bricks it asks
/// about are recorded. Voxels outside the level and in bricks that were not written are inactive.
class BrickFaces {
 public:
  BrickFaces(const Dataset& dataset, int level)
      : dataset_(dataset), level_(level), brick_size_(dataset.info().brick_size) {
    const auto& dims = dataset.level(level).dims;
    for (std::size_t a = 0; a < 3; ++a) {
      dims_[a] = dims[a];
      counts_[a] = (dims[a] + brick_size_ - 1) / brick_size_;
    }
    faces_.resize(static_cast<std::size_t>(counts_[0] * counts_[1] * counts_[2]));
  }

  /// Records the faces of `brick` from its grid.
  template <typename GridT>
  void record(const std::array<std::int64_t, 3>& brick, const GridT& grid) {
    const Box box = dataset_.brickBox(level_, brick);
    auto faces = std::make_unique<Faces>();
    const auto acc = grid.getConstAccessor();
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const std::size_t u = (axis + 1) % 3;
      const std::size_t v = (axis + 2) % 3;
      const std::int64_t width = box.max[u] - box.min[u];
      const std::int64_t height = box.max[v] - box.min[v];
      for (std::size_t side = 0; side < 2; ++side) {
        std::vector<std::uint64_t>& bits = faces->bits[axis * 2 + side];
        bits.assign(static_cast<std::size_t>((width * height + 63) / 64), 0);
        std::array<std::int64_t, 3> c{};
        c[axis] = side == 0 ? box.min[axis] : box.max[axis] - 1;
        for (std::int64_t j = 0; j < height; ++j) {
          c[v] = box.min[v] + j;
          for (std::int64_t i = 0; i < width; ++i) {
            c[u] = box.min[u] + i;
            if (acc.isValueOn(openvdb::Coord(static_cast<int>(c[0]), static_cast<int>(c[1]),
                                             static_cast<int>(c[2])))) {
              const auto bit = static_cast<std::size_t>(j * width + i);
              bits[bit / 64] |= std::uint64_t{1} << (bit % 64);
            }
          }
        }
      }
    }
    faces_[slot(brick)] = std::move(faces);
  }

  /// Whether the voxel `c` on a face of its brick is active. Only voxels on a face of their brick
  /// are known; asking about one inside a brick is a programming error.
  [[nodiscard]] bool active(const openvdb::Coord& c) const {
    const std::array<std::int64_t, 3> p{c.x(), c.y(), c.z()};
    std::array<std::int64_t, 3> brick{};
    for (std::size_t a = 0; a < 3; ++a) {
      if (p[a] < 0 || p[a] >= dims_[a]) {
        return false;
      }
      brick[a] = p[a] / brick_size_;
    }
    const Faces* faces = faces_[slot(brick)].get();
    if (faces == nullptr) {
      return false;  // not written: outside air only
    }
    const Box box = dataset_.brickBox(level_, brick);
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const bool low = p[axis] == box.min[axis];
      if (!low && p[axis] != box.max[axis] - 1) {
        continue;
      }
      const std::size_t u = (axis + 1) % 3;
      const std::size_t v = (axis + 2) % 3;
      const std::int64_t width = box.max[u] - box.min[u];
      const auto bit = static_cast<std::size_t>((p[v] - box.min[v]) * width + (p[u] - box.min[u]));
      const std::vector<std::uint64_t>& bits = faces->bits[axis * 2 + (low ? 0 : 1)];
      return ((bits[bit / 64] >> (bit % 64)) & 1U) != 0;
    }
    return false;  // not on a face
  }

 private:
  struct Faces {
    std::array<std::vector<std::uint64_t>, 6> bits;  // per axis: the low side, then the high side
  };

  [[nodiscard]] std::size_t slot(const std::array<std::int64_t, 3>& brick) const {
    return static_cast<std::size_t>((brick[2] * counts_[1] + brick[1]) * counts_[0] + brick[0]);
  }

  const Dataset& dataset_;
  int level_;
  std::int64_t brick_size_;
  std::array<std::int64_t, 3> dims_{};
  std::array<std::int64_t, 3> counts_{};
  std::vector<std::unique_ptr<Faces>> faces_;
};

}  // namespace voxelsieve::detail
