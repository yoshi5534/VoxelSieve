#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/source.hpp"

namespace voxelsieve {
namespace {

using Index3 = std::array<std::int64_t, 3>;

class DatasetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ =
        std::filesystem::temp_directory_path() /
        ("voxelsieve_dataset_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
         "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  static PhantomSpec spec() {
    PhantomSpec spec = defaultPhantomSpec();
    spec.noise_sigma = 500.0;
    return spec;
  }

  std::filesystem::path dir_;
};

TEST(Sources, PhantomSourceMatchesGeneratedVolume) {
  PhantomSpec spec = defaultPhantomSpec();
  spec.dims = {40, 36, 20};
  spec.noise_sigma = 300.0;
  const Volume16 volume = generatePhantom(spec);
  const Box box{{3, 5, 7}, {31, 30, 19}};
  std::vector<std::uint16_t> from_memory(static_cast<std::size_t>(box.voxelCount()));
  std::vector<std::uint16_t> from_phantom(from_memory.size());
  MemorySource(volume).readRegion(box, from_memory);
  PhantomSource(spec).readRegion(box, from_phantom);
  EXPECT_EQ(from_memory, from_phantom);
}

TEST_F(DatasetTest, MappedRawSourceReadsRegions) {
  PhantomSpec phantom = spec();
  phantom.dims = {24, 16, 12};
  const Volume16 volume = generatePhantom(phantom);
  std::filesystem::create_directories(dir_);
  const auto path = dir_ / "volume.raw";
  writeRaw(path, volume);

  const MappedRawSource mapped(path, phantom.dims, phantom.voxel_size_mm);
  const Box box{{2, 1, 3}, {20, 15, 9}};
  std::vector<std::uint16_t> expected(static_cast<std::size_t>(box.voxelCount()));
  std::vector<std::uint16_t> actual(expected.size());
  MemorySource(volume).readRegion(box, expected);
  mapped.readRegion(box, actual);
  EXPECT_EQ(actual, expected);

  EXPECT_THROW(MappedRawSource(path, {24, 16, 13}, 1.0), std::runtime_error);
  EXPECT_THROW(mapped.readRegion({{0, 0, 0}, {25, 1, 1}}, std::span(actual).first(25)),
               std::out_of_range);
}

TEST_F(DatasetTest, Level0MatchesInMemorySieve) {
  const PhantomSpec phantom = spec();
  const Volume16 volume = generatePhantom(phantom);
  constexpr float kThreshold = 10000.0F;

  SieveOptions sieve_options;
  sieve_options.threshold = kThreshold;
  sieve_options.margin_voxels = 3;
  const SieveResult reference = sieve(volume, sieve_options);

  DatasetOptions options;
  options.threshold = kThreshold;
  options.margin_voxels = 3;
  options.brick_size = 16;  // many bricks, so the margin crosses brick boundaries
  const DatasetInfo info = writeDataset(MemorySource(volume), dir_, options);

  EXPECT_EQ(info.active_voxel_count, reference.stats.active_voxel_count);
  const auto& level0 = info.levels.at(0);
  EXPECT_LT(level0.bricks.size(), std::size_t{512});  // outside-air bricks are skipped

  std::map<Index3, openvdb::FloatGrid::Ptr> bricks;
  for (const Index3& brick : level0.bricks) {
    bricks[brick] = readBrick(brickPath(dir_, 0, brick));
  }
  for (auto it = reference.grid->cbeginValueOn(); it; ++it) {
    const openvdb::Coord c = it.getCoord();
    const Index3 brick{c.x() / 16, c.y() / 16, c.z() / 16};
    ASSERT_TRUE(bricks.contains(brick)) << "missing brick for " << c;
    const auto accessor = bricks[brick]->getConstAccessor();
    ASSERT_TRUE(accessor.isValueOn(c)) << c;
    ASSERT_EQ(accessor.getValue(c), *it) << c;
  }
}

TEST_F(DatasetTest, WritesLevelsOverviewAndIndex) {
  const PhantomSpec phantom = spec();
  DatasetOptions options;
  options.brick_size = 32;
  const DatasetInfo written = writeDataset(PhantomSource(phantom), dir_, options);

  // 128 -> 64 -> 32: three levels, the last one a single brick.
  ASSERT_EQ(written.levels.size(), 3U);
  EXPECT_EQ(written.levels[1].dims, (Index3{64, 64, 64}));
  EXPECT_EQ(written.levels[2].dims, (Index3{32, 32, 32}));
  EXPECT_EQ(written.levels[2].bricks, (std::vector<Index3>{{0, 0, 0}}));
  EXPECT_DOUBLE_EQ(written.levels[2].voxel_size_mm, 4 * phantom.voxel_size_mm);

  const DatasetInfo read = readDatasetInfo(dir_);
  EXPECT_EQ(read.dims, written.dims);
  EXPECT_EQ(read.active_voxel_count, written.active_voxel_count);
  EXPECT_FLOAT_EQ(read.threshold, written.threshold);
  ASSERT_EQ(read.levels.size(), written.levels.size());
  EXPECT_EQ(read.levels[0].bricks, written.levels[0].bricks);

  // The overview maps world positions correctly: the wall is material, the far corner is gone.
  const auto overview = readBrick(dir_ / "overview.vdb", false);
  EXPECT_DOUBLE_EQ(overview->voxelSize()[0], 4 * phantom.voxel_size_mm);
  const auto accessor = overview->getConstAccessor();
  const auto at_world_mm = [&](double x, double y, double z) {
    // World space is level-0 index space scaled by the voxel size; the phantom is centred.
    const double offset = static_cast<double>(phantom.dims[0]) / 2.0 - 0.5;
    const openvdb::Vec3d world((x / phantom.voxel_size_mm + offset) * phantom.voxel_size_mm,
                               (y / phantom.voxel_size_mm + offset) * phantom.voxel_size_mm,
                               (z / phantom.voxel_size_mm + offset) * phantom.voxel_size_mm);
    return overview->transform().worldToIndexCellCentered(world);
  };
  const openvdb::Coord wall = at_world_mm(0.0, 3.25, 0.0);
  ASSERT_TRUE(accessor.isValueOn(wall));
  EXPECT_NEAR(accessor.getValue(wall), phantom.material_value, 0.05 * phantom.material_value);
  EXPECT_FALSE(accessor.isValueOn(at_world_mm(6.0, 6.0, 6.0)));
}

TEST_F(DatasetTest, SmallVolumeIsItsOwnOverview) {
  PhantomSpec phantom = spec();
  phantom.dims = {64, 64, 64};
  phantom.voxel_size_mm = 0.2;
  const DatasetInfo info = writeDataset(PhantomSource(phantom), dir_);  // brick size 256
  ASSERT_EQ(info.levels.size(), 1U);
  EXPECT_EQ(readBrick(dir_ / "overview.vdb", false)->activeVoxelCount(),
            static_cast<openvdb::Index64>(info.active_voxel_count));
}

TEST_F(DatasetTest, RejectsBadOptionsAndNonEmptyDirectory) {
  PhantomSpec phantom = spec();
  phantom.dims = {16, 16, 16};
  const PhantomSource source(phantom);
  DatasetOptions options;
  options.brick_size = 20;
  EXPECT_THROW(writeDataset(source, dir_, options), std::invalid_argument);
  options.brick_size = 16;
  (void)writeDataset(source, dir_, options);
  EXPECT_THROW(writeDataset(source, dir_, options), std::invalid_argument);
}

}  // namespace
}  // namespace voxelsieve
