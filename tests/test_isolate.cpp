#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/isolate.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/volume.hpp"

namespace voxelsieve {
namespace {

constexpr std::int64_t kSize = 96;
constexpr std::uint16_t kAir = 1000;
constexpr std::uint16_t kPart = 20000;
constexpr std::uint16_t kHolder = 12000;
constexpr float kThreshold = 6000.0F;

bool in(std::int64_t v, std::int64_t lo, std::int64_t hi) { return v >= lo && v < hi; }

/// The part: a cube of 40 voxels with a spherical pore of radius 5 in its centre.
bool part(std::int64_t x, std::int64_t y, std::int64_t z) {
  const double dx = static_cast<double>(x) - 40.0;
  const double dy = static_cast<double>(y) - 40.0;
  const double dz = static_cast<double>(z) - 40.0;
  return in(x, 20, 60) && in(y, 20, 60) && in(z, 20, 60) && dx * dx + dy * dy + dz * dz > 25.0;
}

/// A part, a holder `gap` voxels beside it (x from 60 + gap, 10 voxels thick) and a speck of
/// debris, in noise; with `neck`, a bar of 2 x 2 voxels joins holder and part.
Volume16 makeScan(std::int64_t gap, bool neck = false) {
  Volume16 scan{{kSize, kSize, kSize}, VoxelSize{0.1}};
  std::mt19937 rng(3);
  std::normal_distribution<double> noise(0.0, 500.0);
  for (std::int64_t z = 0; z < kSize; ++z) {
    for (std::int64_t y = 0; y < kSize; ++y) {
      for (std::int64_t x = 0; x < kSize; ++x) {
        double value = kAir;
        const bool debris = in(x, 5, 8) && in(y, 80, 83) && in(z, 80, 83);
        const bool bar = neck && in(x, 60, 60 + gap) && in(y, 39, 41) && in(z, 39, 41);
        if (part(x, y, z) || debris || bar) {
          value = kPart;
        } else if (in(x, 60 + gap, 70 + gap) && in(y, 20, 60) && in(z, 0, 60)) {
          value = kHolder;
        }
        scan.data[scan.index(x, y, z)] =
            static_cast<std::uint16_t>(std::clamp(value + noise(rng), 0.0, 65535.0));
      }
    }
  }
  return scan;
}

class IsolateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_isolate_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  Dataset sieve(const Volume16& scan) {
    DatasetOptions options;
    options.threshold = kThreshold;
    options.brick_size = 32;  // objects and cells cross bricks
    options.min_material_voxels = 4;
    (void)writeDataset(MemorySource(scan), dir_ / "dataset", options);
    return Dataset::open(dir_ / "dataset");
  }

  /// Level-0 voxels above the threshold.
  static std::int64_t materialVoxels(const Dataset& dataset) {
    std::atomic<std::int64_t> count{0};  // bricks run in parallel
    dataset.forEachGrid(0, [&](const auto& /*brick*/, const auto& grid) {
      std::int64_t local = 0;
      for (auto it = grid.cbeginValueOn(); it; ++it) {
        local += static_cast<float>(*it) > kThreshold ? 1 : 0;
      }
      count += local;
    });
    return count;
  }

  std::filesystem::path dir_;
};

std::int64_t partVoxels() {
  std::int64_t count = 0;
  for (std::int64_t z = 0; z < kSize; ++z) {
    for (std::int64_t y = 0; y < kSize; ++y) {
      for (std::int64_t x = 0; x < kSize; ++x) {
        count += part(x, y, z) ? 1 : 0;
      }
    }
  }
  return count;
}

TEST_F(IsolateTest, KeepsTheLargestObjectWithItsPore) {
  const Dataset input = sieve(makeScan(12));
  const IsolateInfo info = isolatePart(input, dir_ / "part");
  EXPECT_EQ(info.cell_size, 1);
  EXPECT_GE(info.objects, 3);  // part, holder, debris
  ASSERT_EQ(info.kept_material_voxels.size(), 1U);
  EXPECT_EQ(info.kept_material_voxels[0], partVoxels());
  EXPECT_GT(info.removed_voxels, 0);

  const Dataset isolated = Dataset::open(dir_ / "part");
  EXPECT_EQ(isolated.info().active_voxel_count, info.dataset.active_voxel_count);
  EXPECT_LT(isolated.info().levels[0].bricks.size(), input.info().levels[0].bricks.size());
  EXPECT_TRUE(isolated.sample(0, {30, 30, 30}).has_value());   // part
  EXPECT_TRUE(isolated.sample(0, {40, 40, 40}).has_value());   // its pore stays
  EXPECT_TRUE(isolated.sample(0, {61, 40, 40}).has_value());   // the margin of air
  EXPECT_FALSE(isolated.sample(0, {77, 40, 40}).has_value());  // holder
  EXPECT_FALSE(isolated.sample(0, {6, 81, 81}).has_value());   // debris
  EXPECT_EQ(materialVoxels(isolated), partVoxels());
  // The coarser levels follow.
  EXPECT_FALSE(isolated.sample(1, {38, 20, 20}).has_value());
}

TEST_F(IsolateTest, FindsObjectsOnCoarserCells) {
  const Dataset input = sieve(makeScan(12));
  IsolateOptions options;
  options.max_cells = 4096;
  const IsolateInfo info = isolatePart(input, dir_ / "part", options);
  EXPECT_EQ(info.cell_size, 8);
  ASSERT_EQ(info.kept_material_voxels.size(), 1U);
  const Dataset isolated = Dataset::open(dir_ / "part");
  EXPECT_EQ(materialVoxels(isolated), partVoxels());
}

TEST_F(IsolateTest, RegionSeparatesATouchingHolder) {
  const Dataset input = sieve(makeScan(0));
  // Touching, the holder and the part are one object.
  const IsolateInfo joined = isolatePart(input, dir_ / "joined");
  EXPECT_GT(materialVoxels(Dataset::open(dir_ / "joined")), partVoxels());
  EXPECT_GT(joined.kept_material_voxels.at(0), partVoxels());

  IsolateOptions options;
  options.region = Box{{0, 0, 0}, {60, kSize, kSize}};
  (void)isolatePart(input, dir_ / "cropped", options);
  const Dataset cropped = Dataset::open(dir_ / "cropped");
  EXPECT_EQ(materialVoxels(cropped), partVoxels());
  EXPECT_FALSE(cropped.sample(0, {60, 40, 40}).has_value());

  // Only the region, every object.
  options.keep_objects = 0;
  (void)isolatePart(input, dir_ / "region", options);
  EXPECT_TRUE(Dataset::open(dir_ / "region").sample(0, {6, 81, 81}).has_value());
}

TEST_F(IsolateTest, ErosionSeparatesObjectsJoinedByANeck) {
  const Dataset input = sieve(makeScan(12, true));
  // Joined by the bar, part and holder are one object.
  (void)isolatePart(input, dir_ / "joined");
  EXPECT_TRUE(Dataset::open(dir_ / "joined").sample(0, {77, 40, 40}).has_value());

  IsolateOptions options;
  options.separate_voxels = 2;
  const IsolateInfo info = isolatePart(input, dir_ / "separated", options);
  EXPECT_EQ(info.objects, 2);  // part and holder; the debris erodes away
  const Dataset separated = Dataset::open(dir_ / "separated");
  EXPECT_FALSE(separated.sample(0, {77, 40, 40}).has_value());  // holder
  EXPECT_TRUE(separated.sample(0, {21, 21, 21}).has_value());   // the part's corner grows back
  const std::int64_t material = materialVoxels(separated);
  EXPECT_GE(material, partVoxels());
  EXPECT_LE(material, partVoxels() + 4 * 6);  // at most a stub of the bar
}

TEST_F(IsolateTest, PlanesAndACylinderCutAway) {
  const Dataset input = sieve(makeScan(0));
  IsolateOptions options;
  options.cut_planes = {{{59.5, 0.0, 0.0}, {1.0, 0.0, 0.0}}};  // everything at x > 59.5 goes
  (void)isolatePart(input, dir_ / "plane", options);
  const Dataset cut = Dataset::open(dir_ / "plane");
  EXPECT_EQ(materialVoxels(cut), partVoxels());
  EXPECT_FALSE(cut.sample(0, {62, 40, 40}).has_value());

  // A cylinder along z around the part keeps neither the debris nor the far end of the holder.
  IsolateOptions tube;
  tube.keep_objects = 0;
  tube.cylinder = RegionCylinder{2, {40.0, 40.0}, 30.0};
  (void)isolatePart(input, dir_ / "cylinder", tube);
  const Dataset inside = Dataset::open(dir_ / "cylinder");
  EXPECT_TRUE(inside.sample(0, {21, 21, 30}).has_value());
  EXPECT_FALSE(inside.sample(0, {6, 81, 81}).has_value());
  EXPECT_FALSE(inside.sample(0, {69, 22, 30}).has_value());

  IsolateOptions bad;
  bad.cut_planes = {{{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}}};
  EXPECT_THROW((void)isolatePart(input, dir_ / "bad", bad), std::invalid_argument);
}

TEST_F(IsolateTest, RejectsBadOptions) {
  const Dataset input = sieve(makeScan(12));
  IsolateOptions options;
  options.region = Box{{0, 0, 0}, {kSize + 1, kSize, kSize}};
  EXPECT_THROW((void)isolatePart(input, dir_ / "a", options), std::invalid_argument);
  options.region = Box{{10, 0, 0}, {10, kSize, kSize}};
  EXPECT_THROW((void)isolatePart(input, dir_ / "b", options), std::invalid_argument);
  options.region.reset();
  options.keep_objects = -1;
  EXPECT_THROW((void)isolatePart(input, dir_ / "c", options), std::invalid_argument);
}

}  // namespace
}  // namespace voxelsieve
