#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
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

  const MappedRawSource mapped(path, phantom.dims, phantom.voxel_size);
  const Box box{{2, 1, 3}, {20, 15, 9}};
  std::vector<std::uint16_t> expected(static_cast<std::size_t>(box.voxelCount()));
  std::vector<std::uint16_t> actual(expected.size());
  MemorySource(volume).readRegion(box, expected);
  mapped.readRegion(box, actual);
  EXPECT_EQ(actual, expected);

  EXPECT_THROW(MappedRawSource(path, {24, 16, 13}, 1.0), std::runtime_error);  // too small
  EXPECT_THROW(mapped.readRegion({{0, 0, 0}, {25, 1, 1}}, std::span(actual).first(25)),
               std::out_of_range);
}

/// Writes `volume` with an arbitrary header and footer in the given sample type and byte order.
void writeRawWithHeader(const std::filesystem::path& path, const Volume16& volume,
                        std::size_t header, std::size_t footer, SampleType type,
                        std::endian order) {
  std::ofstream out(path, std::ios::binary);
  const std::string header_bytes(header, 'H');
  out.write(header_bytes.data(), static_cast<std::streamsize>(header));
  for (const std::uint16_t value : volume.data) {
    if (type == SampleType::kUInt8) {
      out.put(static_cast<char>(value & 0xFFU));
    } else {
      const auto lo = static_cast<char>(value & 0xFFU);
      const auto hi = static_cast<char>(value >> 8U);
      out.put(order == std::endian::little ? lo : hi);
      out.put(order == std::endian::little ? hi : lo);
    }
  }
  const std::string footer_bytes(footer, 'F');
  out.write(footer_bytes.data(), static_cast<std::streamsize>(footer));
}

TEST_F(DatasetTest, MappedRawSourceSkipsHeadersAndDecodesSamples) {
  PhantomSpec phantom = spec();
  phantom.dims = {20, 12, 10};
  const Volume16 volume = generatePhantom(phantom);
  std::filesystem::create_directories(dir_);
  const Box box{{1, 2, 3}, {19, 11, 9}};
  std::vector<std::uint16_t> expected(static_cast<std::size_t>(box.voxelCount()));
  MemorySource(volume).readRegion(box, expected);
  std::vector<std::uint16_t> actual(expected.size());

  // Odd header size: samples are not 2-byte aligned. Detected automatically from the file size.
  const auto big_endian = dir_ / "big.raw";
  writeRawWithHeader(big_endian, volume, 1235, 0, SampleType::kUInt16, std::endian::big);
  const MappedRawSource big(big_endian, RawLayout{phantom.dims, 0.1, SampleType::kUInt16,
                                                  std::endian::big, std::nullopt});
  EXPECT_EQ(big.headerBytes(), 1235U);
  big.readRegion(box, actual);
  EXPECT_EQ(actual, expected);

  // Explicit header with a footer after the voxel data.
  const auto footer = dir_ / "footer.raw";
  writeRawWithHeader(footer, volume, 512, 100, SampleType::kUInt16, std::endian::little);
  const MappedRawSource with_footer(
      footer, RawLayout{phantom.dims, 0.1, SampleType::kUInt16, std::endian::little, 512});
  with_footer.readRegion(box, actual);
  EXPECT_EQ(actual, expected);

  // 8-bit samples come back unchanged.
  Volume16 small_values = volume;
  for (auto& value : small_values.data) {
    value = static_cast<std::uint16_t>(value % 256U);
  }
  const auto eight_bit = dir_ / "eight.raw";
  writeRawWithHeader(eight_bit, small_values, 64, 0, SampleType::kUInt8, std::endian::little);
  const MappedRawSource uint8(eight_bit, RawLayout{phantom.dims, 0.1, SampleType::kUInt8,
                                                   std::endian::little, std::nullopt});
  EXPECT_EQ(uint8.headerBytes(), 64U);
  uint8.readRegion(box, actual);
  MemorySource(small_values).readRegion(box, expected);
  EXPECT_EQ(actual, expected);

  // Too small for the dimensions, or a header that leaves no room for the data.
  EXPECT_THROW(MappedRawSource(eight_bit, RawLayout{phantom.dims, 0.1, SampleType::kUInt16,
                                                    std::endian::little, std::nullopt}),
               std::runtime_error);
  EXPECT_THROW(MappedRawSource(footer, RawLayout{phantom.dims, 0.1, SampleType::kUInt16,
                                                 std::endian::little, 700}),
               std::runtime_error);
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

TEST_F(DatasetTest, MinMaterialIgnoresNoiseSpikesInAir) {
  // Heavy noise: about one voxel in 1300 of the air lies above the threshold, so a third of the
  // air blocks hold a spike, but hardly any holds four. The walls start on a block boundary and
  // are 15 voxels thick, so every wall block holds far more than four material voxels.
  PhantomSpec noisy = defaultPhantomSpec();
  noisy.noise_sigma = 3000.0;
  PhantomSpec clean = noisy;
  clean.noise_sigma = 0.0;
  constexpr float kThreshold = 10500.0F;

  DatasetOptions options;
  options.threshold = kThreshold;
  options.brick_size = 32;
  const DatasetInfo reference = writeDataset(PhantomSource(clean), dir_ / "clean", options);
  const DatasetInfo spiky = writeDataset(PhantomSource(noisy), dir_ / "k1", options);
  options.min_material_voxels = 4;
  const DatasetInfo robust = writeDataset(PhantomSource(noisy), dir_ / "k4", options);

  EXPECT_GT(spiky.active_voxel_count, reference.active_voxel_count * 3 / 2);
  EXPECT_EQ(robust.active_voxel_count, reference.active_voxel_count);
  EXPECT_EQ(robust.min_material_voxels, 4);
  EXPECT_EQ(readDatasetInfo(dir_ / "k4").min_material_voxels, 4);
  EXPECT_EQ(readDatasetInfo(dir_ / "k1").min_material_voxels, 1);

  // Voxel-identical to the in-memory sieve with the same block criterion.
  SieveOptions sieve_options;
  sieve_options.threshold = kThreshold;
  sieve_options.min_material_voxels = 4;
  const SieveResult in_memory = sieve(generatePhantom(noisy), sieve_options);
  EXPECT_EQ(robust.active_voxel_count, in_memory.stats.active_voxel_count);
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
  EXPECT_EQ(written.levels[2].voxel_size, phantom.voxel_size.scaled(4.0));

  const DatasetInfo read = readDatasetInfo(dir_);
  EXPECT_EQ(read.dims, written.dims);
  EXPECT_EQ(read.active_voxel_count, written.active_voxel_count);
  EXPECT_FLOAT_EQ(read.threshold, written.threshold);
  ASSERT_EQ(read.levels.size(), written.levels.size());
  EXPECT_EQ(read.levels[0].bricks, written.levels[0].bricks);

  // The overview maps world positions correctly: the wall is material, the far corner is gone.
  const auto overview = readBrick(dir_ / "overview.vdb", false);
  EXPECT_DOUBLE_EQ(overview->voxelSize()[0], 4 * phantom.voxel_size[0]);
  const auto accessor = overview->getConstAccessor();
  const auto at_world_mm = [&](double x, double y, double z) {
    // World space is level-0 index space scaled by the voxel size; the phantom is centred.
    const double offset = static_cast<double>(phantom.dims[0]) / 2.0 - 0.5;
    const VoxelSize& v = phantom.voxel_size;
    const openvdb::Vec3d world((x / v[0] + offset) * v[0], (y / v[1] + offset) * v[1],
                               (z / v[2] + offset) * v[2]);
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
  phantom.voxel_size = 0.2;
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
  options.min_material_voxels = 0;
  EXPECT_THROW(writeDataset(source, dir_, options), std::invalid_argument);
  options.min_material_voxels = 513;
  EXPECT_THROW(writeDataset(source, dir_, options), std::invalid_argument);
  options.min_material_voxels = 1;
  (void)writeDataset(source, dir_, options);
  EXPECT_THROW(writeDataset(source, dir_, options), std::invalid_argument);
}

class DatasetReaderTest : public DatasetTest {
 protected:
  static constexpr float kThreshold = 10000.0F;
  static constexpr std::int64_t kBrickSize = 16;

  /// Writes the phantom with many small bricks and returns the in-memory sieve as reference.
  SieveResult writeReference() {
    const Volume16 volume = generatePhantom(spec());
    SieveOptions sieve_options;
    sieve_options.threshold = kThreshold;
    DatasetOptions options;
    options.threshold = kThreshold;
    options.brick_size = kBrickSize;
    (void)writeDataset(MemorySource(volume), dir_, options);
    return sieve(volume, sieve_options);
  }
};

TEST_F(DatasetReaderTest, SamplesMatchInMemorySieve) {
  const SieveResult reference = writeReference();
  const Dataset dataset = Dataset::open(dir_);
  EXPECT_EQ(dataset.info().dims, (Index3{128, 128, 128}));

  std::int64_t checked = 0;
  for (auto it = reference.grid->cbeginValueOn(); it; ++it) {
    const openvdb::Coord c = it.getCoord();
    const auto value = dataset.sample(0, {c.x(), c.y(), c.z()});
    ASSERT_EQ(value, std::optional<float>(*it)) << c;
    ++checked;
  }
  EXPECT_EQ(checked, dataset.info().active_voxel_count);

  // Removed outside air and positions outside the volume have no value.
  EXPECT_FALSE(dataset.sample(0, {0, 0, 0}).has_value());
  EXPECT_FALSE(dataset.sample(0, {-1, 5, 5}).has_value());
  EXPECT_FALSE(dataset.sample(0, {5, 5, 128}).has_value());
  EXPECT_FALSE(dataset.hasBrick(0, {0, 0, 0}));
  EXPECT_EQ(dataset.brick(0, {0, 0, 0}), nullptr);
  EXPECT_THROW((void)dataset.sample(7, {0, 0, 0}), std::out_of_range);
}

TEST_F(DatasetReaderTest, RegionAcrossBricksMatchesInMemorySieve) {
  const SieveResult reference = writeReference();
  const Dataset dataset = Dataset::open(dir_);
  // Spans several bricks on every axis, with partial bricks at both ends.
  const Box box{{5, 20, 37}, {70, 61, 100}};
  constexpr float kFill = -1.0F;
  std::vector<float> region(static_cast<std::size_t>(box.voxelCount()));
  dataset.readRegion(0, box, region, kFill);

  const auto accessor = reference.grid->getConstAccessor();
  std::size_t index = 0;
  std::size_t active = 0;
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
      for (std::int64_t x = box.min[0]; x < box.max[0]; ++x, ++index) {
        const openvdb::Coord c(static_cast<int>(x), static_cast<int>(y), static_cast<int>(z));
        float expected = kFill;
        if (accessor.probeValue(c, expected)) {
          ++active;
        } else {
          expected = kFill;
        }
        ASSERT_EQ(region[index], expected) << c;
      }
    }
  }
  EXPECT_GT(active, 0U);
  EXPECT_LT(active, region.size());  // the box also contains removed air

  EXPECT_THROW(dataset.readRegion(0, {{0, 0, 0}, {129, 1, 1}}, region), std::out_of_range);
  EXPECT_THROW(dataset.readRegion(0, {{0, 0, 0}, {2, 2, 2}}, region), std::invalid_argument);
}

TEST_F(DatasetReaderTest, CacheStaysWithinBudget) {
  (void)writeReference();
  const auto one_brick = static_cast<std::size_t>(
      readBrick(brickPath(dir_, 0, readDatasetInfo(dir_).levels[0].bricks.front()), false)
          ->memUsage());
  const std::size_t budget = 3 * one_brick;
  const Dataset dataset = Dataset::open(dir_, budget);

  const auto& bricks = dataset.level(0).bricks;
  ASSERT_GT(bricks.size(), 10U);
  for (const Index3& brick : bricks) {
    ASSERT_NE(dataset.brick(0, brick), nullptr);
    const CacheStats stats = dataset.cacheStats();
    EXPECT_TRUE(stats.bytes <= budget || stats.bricks == 1) << stats.bytes;
  }
  const CacheStats after_first_pass = dataset.cacheStats();
  EXPECT_EQ(after_first_pass.misses, bricks.size());
  EXPECT_LT(after_first_pass.bricks, bricks.size());  // older bricks were evicted

  // The most recent brick is cached; a brick held by the caller survives eviction.
  const Dataset::BrickPtr held = dataset.brick(0, bricks.back());
  EXPECT_EQ(dataset.cacheStats().hits, 1U);
  for (const Index3& brick : bricks) {
    (void)dataset.brick(0, brick);
  }
  EXPECT_GT(held->activeVoxelCount(), 0U);
}

TEST_F(DatasetReaderTest, ParallelBrickIterationSeesEveryVoxel) {
  (void)writeReference();
  // A tiny budget forces eviction while several threads load bricks.
  const Dataset dataset = Dataset::open(dir_, 1);
  for (int level = 0; level < static_cast<int>(dataset.info().levels.size()); ++level) {
    std::atomic<std::int64_t> active{0};
    std::atomic<std::size_t> visited{0};
    dataset.forEachBrick(level, [&](const Index3& brick, const openvdb::FloatGrid& grid) {
      const Box box = dataset.brickBox(level, brick);
      const openvdb::CoordBBox bounds = grid.evalActiveVoxelBoundingBox();
      EXPECT_GE(bounds.min().x(), box.min[0]);
      EXPECT_LT(bounds.max().z(), box.max[2]);
      active += static_cast<std::int64_t>(grid.activeVoxelCount());
      ++visited;
    });
    EXPECT_EQ(visited, dataset.level(level).bricks.size());
    if (level == 0) {
      EXPECT_EQ(active, dataset.info().active_voxel_count);
    } else {
      EXPECT_GT(active, 0);
    }
  }
}

}  // namespace
}  // namespace voxelsieve
