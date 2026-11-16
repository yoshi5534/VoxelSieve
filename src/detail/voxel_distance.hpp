#pragma once

// Distances between sets of voxels and the sphere that encloses a set of points, for the pore
// characteristics of BDG P 203 (ADR 0031).

#include <openvdb/openvdb.h>

#include <array>
#include <limits>
#include <span>

#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve::detail {

/// Shortest distance in mm between the cubes of two voxels (or of two boxes of voxels): the
/// material between them. Voxels that share a face, edge or corner are 0 apart.
[[nodiscard]] double voxelGapMm(const openvdb::CoordBBox& a, const openvdb::CoordBBox& b,
                                const VoxelSize& voxel_size);

/// Shortest gap (voxelGapMm) between an active voxel of `a` and one of `b`, both voxelized
/// masks. Searches both trees from their nearest nodes outward, so the time grows with the size
/// of `a` and the number of voxels of `b` near it, not with the size of `b`. Returns `bound` when
/// no pair is closer than that, and infinity when either mask is empty.
[[nodiscard]] double maskGapMm(const openvdb::BoolTree& a, const openvdb::BoolTree& b,
                               const VoxelSize& voxel_size,
                               double bound = std::numeric_limits<double>::infinity());

struct EnclosingSphere {
  std::array<double, 3> center{0.0, 0.0, 0.0};
  double radius = 0.0;
};

/// Smallest sphere that encloses all `points` (Welzl's algorithm, in expected linear time). The
/// order of `points` changes.
[[nodiscard]] EnclosingSphere enclosingSphere(std::span<std::array<double, 3>> points);

}  // namespace voxelsieve::detail
