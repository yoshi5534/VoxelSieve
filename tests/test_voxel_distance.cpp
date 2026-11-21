#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "detail/voxel_distance.hpp"
#include "voxelsieve/vdb.hpp"

namespace voxelsieve {
namespace {

using openvdb::Coord;
using openvdb::CoordBBox;

/// The gap by comparing every voxel of `a` with every voxel of `b`.
double bruteForceGap(const openvdb::BoolTree& a, const openvdb::BoolTree& b, const VoxelSize& v) {
  double best = std::numeric_limits<double>::infinity();
  for (auto it = a.cbeginValueOn(); it; ++it) {
    for (auto jt = b.cbeginValueOn(); jt; ++jt) {
      best = std::min(best, detail::voxelGapMm(CoordBBox(it.getCoord(), it.getCoord()),
                                               CoordBBox(jt.getCoord(), jt.getCoord()), v));
    }
  }
  return best;
}

TEST(VoxelDistance, MaskGapIsTheSmallestGapOfAnyTwoVoxels) {
  initializeVdb();
  const VoxelSize v{0.05, 0.08, 0.11};
  std::mt19937 random(7);
  std::uniform_int_distribution<int> coord(-40, 60);
  for (int round = 0; round < 20; ++round) {
    openvdb::BoolTree a(false);
    openvdb::BoolTree b(false);
    // Scattered over several leaves, `b` mostly beyond `a` in x.
    for (int i = 0; i < 60; ++i) {
      a.setValueOn(Coord(coord(random), coord(random), coord(random)));
      b.setValueOn(Coord(coord(random) + 30, coord(random), coord(random)));
    }
    const double expected = bruteForceGap(a, b, v);
    EXPECT_EQ(detail::maskGapMm(a, b, v), expected) << "round " << round;
    // A bound below the gap is returned as it is; one above does not change the gap.
    EXPECT_EQ(detail::maskGapMm(a, b, v, expected * 0.5), expected * 0.5);
    EXPECT_EQ(detail::maskGapMm(a, b, v, expected + 1.0), expected);
  }
}

TEST(VoxelDistance, MaskGapOfTouchingOrEmptyMasks) {
  initializeVdb();
  const VoxelSize v{0.1, 0.1, 0.1};
  openvdb::BoolTree a(false);
  openvdb::BoolTree b(false);
  EXPECT_FALSE(std::isfinite(detail::maskGapMm(a, b, v)));
  a.setValueOn(Coord(0, 0, 0));
  EXPECT_FALSE(std::isfinite(detail::maskGapMm(a, b, v)));
  b.setValueOn(Coord(1, 1, 1));
  EXPECT_EQ(detail::maskGapMm(a, b, v), 0.0);
  b.setValueOff(Coord(1, 1, 1));
  b.setValueOn(Coord(4, 0, 0));
  EXPECT_DOUBLE_EQ(detail::maskGapMm(a, b, v), 0.3);
}

// Two concentric spherical shells of a million voxels and more: the material between them,
// found in well under a second where a search per voxel took minutes.
TEST(VoxelDistance, MaskGapOfLargeShells) {
  initializeVdb();
  const VoxelSize v{0.01, 0.01, 0.01};
  const double inner = 180.0;
  const double outer = 230.0;
  openvdb::BoolTree a(false);
  openvdb::BoolTree b(false);
  const int r = static_cast<int>(outer) + 1;
  for (int z = -r; z <= r; ++z) {
    for (int y = -r; y <= r; ++y) {
      for (int x = -r; x <= r; ++x) {
        const double d = std::sqrt(static_cast<double>(x * x + y * y + z * z));
        if (std::abs(d - inner) < 0.5) {
          a.setValueOn(Coord(x, y, z));
        } else if (std::abs(d - outer) < 0.5) {
          b.setValueOn(Coord(x, y, z));
        }
      }
    }
  }
  ASSERT_GT(a.activeVoxelCount() + b.activeVoxelCount(), 1'000'000U);
  // Along an axis, voxel 180 and voxel 230 have 49 voxels between them; along a diagonal the
  // gap of the cubes is a little smaller.
  const double gap = detail::maskGapMm(a, b, v);
  EXPECT_LE(gap, 49.0 * 0.01 + 1e-9);
  EXPECT_GT(gap, 47.0 * 0.01);
}

}  // namespace
}  // namespace voxelsieve
