#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <random>
#include <vector>

#include "detail/components.hpp"
#include "voxelsieve/vdb.hpp"

namespace voxelsieve {
namespace {

using openvdb::Coord;

/// Components by a serial flood fill from each unvisited voxel in tree order: the numbering
/// VoxelComponents must reproduce.
std::map<Coord, std::int64_t> floodFill(const openvdb::BoolGrid& mask) {
  std::map<Coord, std::int64_t> labels;
  const auto acc = mask.getConstAccessor();
  std::int64_t next = 0;
  for (auto it = mask.cbeginValueOn(); it; ++it) {
    if (labels.contains(it.getCoord())) {
      continue;
    }
    std::deque<Coord> queue{it.getCoord()};
    labels[it.getCoord()] = next;
    while (!queue.empty()) {
      const Coord c = queue.front();
      queue.pop_front();
      for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
          for (int x = -1; x <= 1; ++x) {
            const Coord n = c.offsetBy(x, y, z);
            if (acc.isValueOn(n) && !labels.contains(n)) {
              labels[n] = next;
              queue.push_back(n);
            }
          }
        }
      }
    }
    ++next;
  }
  return labels;
}

void expectSameComponents(const openvdb::BoolGrid& mask) {
  const std::map<Coord, std::int64_t> expected = floodFill(mask);
  const detail::VoxelComponents components(mask.tree());
  std::int64_t count = 0;
  for (const auto& [voxel, label] : expected) {
    ASSERT_EQ(components.at(voxel), label) << voxel;
    count = std::max(count, label + 1);
  }
  EXPECT_EQ(static_cast<std::int64_t>(components.size()), count);
  EXPECT_EQ(components.at(Coord(1000, 1000, 1000)), -1);
}

TEST(ComponentsTest, MatchesASerialFloodFillOnRandomMasks) {
  initializeVdb();
  std::mt19937 random(7);
  // Sparse masks have many small components that touch across leaf faces, edges and corners;
  // dense ones a few large ones. The box straddles the origin, so leaves have negative origins.
  for (const double density : {0.05, 0.15, 0.3, 0.6}) {
    auto mask = openvdb::BoolGrid::create(false);
    auto acc = mask->getAccessor();
    std::bernoulli_distribution on(density);
    for (int z = -12; z < 20; ++z) {
      for (int y = -9; y < 17; ++y) {
        for (int x = -20; x < 12; ++x) {
          if (on(random)) {
            acc.setValueOn(Coord(x, y, z));
          }
        }
      }
    }
    expectSameComponents(*mask);
  }
}

TEST(ComponentsTest, JoinsPiecesOnlyThroughCornersAndTiles) {
  initializeVdb();
  auto mask = openvdb::BoolGrid::create(false);
  auto acc = mask->getAccessor();
  // A diagonal line of voxels through the corners of eight leaves is one component.
  for (int i = -16; i < 16; ++i) {
    acc.setValueOn(Coord(i, i, i));
  }
  // A fully active tile, and a voxel touching it at a corner.
  mask->tree().addTile(1, Coord(64, 0, 0), true, true);
  acc.setValueOn(Coord(72, 8, 8));
  // An isolated voxel.
  acc.setValueOn(Coord(-40, 0, 0));
  const detail::VoxelComponents components(mask->tree());
  EXPECT_EQ(components.size(), 3U);
  EXPECT_EQ(components.at(Coord(-16, -16, -16)), components.at(Coord(15, 15, 15)));
  EXPECT_EQ(components.at(Coord(64, 0, 0)), components.at(Coord(72, 8, 8)));
  EXPECT_NE(components.at(Coord(-40, 0, 0)), components.at(Coord(0, 0, 0)));
}

}  // namespace
}  // namespace voxelsieve
