#include <gtest/gtest.h>
#include <tbb/parallel_for.h>
#include <zlib.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define VOXELSIEVE_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define VOXELSIEVE_TEST_SANITIZED 1
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <latch>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "detail/mapped_read.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/resources.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/vdb.hpp"

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

TEST(Sources, ConcatSourceJoinsPartsAlongEachAxis) {
  PhantomSpec spec = defaultPhantomSpec();
  spec.dims = {40, 36, 20};
  spec.noise_sigma = 300.0;
  const Volume16 whole = generatePhantom(spec);
  const MemorySource reference(whole);
  for (int axis = 0; axis < 3; ++axis) {
    const auto a = static_cast<std::size_t>(axis);
    // Three parts of unequal length, cut out of the whole volume.
    const std::array<std::int64_t, 4> cuts{0, 7, 8, spec.dims[a]};
    std::vector<Volume16> pieces;
    std::vector<std::unique_ptr<VolumeSource>> parts;
    pieces.reserve(3);
    for (std::size_t p = 0; p < 3; ++p) {
      Box box{{0, 0, 0}, spec.dims};
      box.min[a] = cuts[p];
      box.max[a] = cuts[p + 1];
      Volume16& piece = pieces.emplace_back();
      piece.dims = {box.size(0), box.size(1), box.size(2)};
      piece.voxel_size = whole.voxel_size;
      piece.data.resize(static_cast<std::size_t>(box.voxelCount()));
      reference.readRegion(box, piece.data);
      parts.push_back(std::make_unique<MemorySource>(piece));
    }
    const ConcatSource joined(std::move(parts), axis);
    ASSERT_EQ(joined.dims(), spec.dims);
    EXPECT_EQ(joined.partStart(2), 8);
    const Box box{{3, 5, 2}, {31, 30, 19}};
    std::vector<std::uint16_t> expected(static_cast<std::size_t>(box.voxelCount()));
    std::vector<std::uint16_t> actual(expected.size());
    reference.readRegion(box, expected);
    joined.readRegion(box, actual);
    EXPECT_EQ(actual, expected) << "axis " << axis;
  }
}

TEST(Sources, ConcatSourceRejectsMismatchedParts) {
  PhantomSpec small = defaultPhantomSpec();
  small.dims = {8, 8, 8};
  PhantomSpec wide = small;
  wide.dims = {16, 8, 8};
  std::vector<std::unique_ptr<VolumeSource>> parts;
  parts.push_back(std::make_unique<PhantomSource>(small));
  parts.push_back(std::make_unique<PhantomSource>(wide));
  EXPECT_THROW(ConcatSource(std::move(parts), 1), std::invalid_argument);
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
  // Letting the pages go changes nothing that is read afterwards.
  mapped.releaseMemory();
  std::ranges::fill(actual, 0);
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

void expectSameDataset(const std::filesystem::path& a, const std::filesystem::path& b);

/// Compresses `source` with zlib's gzip writer into `target`, as `members` gzip members one after
/// another (as pigz and concatenated files have them).
void gzipFile(const std::filesystem::path& source, const std::filesystem::path& target,
              int members = 1) {
  std::ifstream in(source, std::ios::binary);
  const std::vector<char> bytes{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
  std::filesystem::remove(target);
  const std::size_t part =
      (bytes.size() + static_cast<std::size_t>(members) - 1) / static_cast<std::size_t>(members);
  for (std::size_t begin = 0; begin < bytes.size(); begin += part) {
    gzFile file = gzopen(target.string().c_str(), "ab");
    ASSERT_NE(file, nullptr);
    const auto count = static_cast<unsigned>(std::min(part, bytes.size() - begin));
    ASSERT_EQ(gzwrite(file, bytes.data() + begin, count), static_cast<int>(count));
    gzclose(file);
  }
}

TEST_F(DatasetTest, GzipRawSourceReadsLikeTheUncompressedFile) {
  PhantomSpec phantom = spec();
  phantom.dims = {20, 12, 10};
  const Volume16 volume = generatePhantom(phantom);
  std::filesystem::create_directories(dir_);
  writeRawWithHeader(dir_ / "scan.raw", volume, 77, 0, SampleType::kUInt16, std::endian::big);
  gzipFile(dir_ / "scan.raw", dir_ / "scan.raw.gz", 3);
  EXPECT_TRUE(isGzipFile(dir_ / "scan.raw.gz"));
  EXPECT_FALSE(isGzipFile(dir_ / "scan.raw"));
  EXPECT_EQ(rawSidecarPath(dir_ / "scan.raw.gz"), dir_ / "scan.json");
  EXPECT_EQ(rawSidecarPath(dir_ / "scan.raw"), dir_ / "scan.json");

  const RawLayout layout{phantom.dims, 0.1, SampleType::kUInt16, std::endian::big, 77};
  const GzipRawSource gzip(dir_ / "scan.raw.gz", layout);
  EXPECT_TRUE(gzip.sequentialAccess());
  EXPECT_TRUE(gzip.slowRandomAccess());
  // Forward, backward (the stream starts again) and whole slices.
  for (const Box& box : {Box{{1, 2, 3}, {19, 11, 9}}, Box{{0, 0, 0}, {20, 12, 2}},
                         Box{{0, 0, 7}, {20, 12, 10}}, Box{{5, 5, 1}, {6, 6, 2}}}) {
    std::vector<std::uint16_t> expected(static_cast<std::size_t>(box.voxelCount()));
    MemorySource(volume).readRegion(box, expected);
    std::vector<std::uint16_t> actual(expected.size());
    gzip.readRegion(box, actual);
    EXPECT_EQ(actual, expected);
  }
  std::uint64_t header = 0;
  EXPECT_TRUE(openRawVolume(dir_ / "scan.raw.gz", layout, &header)->sequentialAccess());
  EXPECT_EQ(header, 77U);
  EXPECT_FALSE(openRawVolume(dir_ / "scan.raw", layout, &header)->sequentialAccess());

  // Dimensions larger than the data: reading past the end fails.
  RawLayout larger = layout;
  larger.dims[2] = 11;
  const GzipRawSource short_file(dir_ / "scan.raw.gz", larger);
  std::vector<std::uint16_t> slice(std::size_t{20} * 12);
  EXPECT_THROW(short_file.readRegion(Box{{0, 0, 10}, {20, 12, 11}}, slice), std::runtime_error);
}

TEST_F(DatasetTest, GzipRawImportsInOnePassWithItsPreview) {
  const PhantomSpec phantom = spec();
  std::filesystem::create_directories(dir_);
  writeRaw(dir_ / "scan.raw", generatePhantom(phantom));
  gzipFile(dir_ / "scan.raw", dir_ / "scan.raw.gz");
  DatasetOptions options;
  options.brick_size = 32;
  const DatasetInfo reference =
      writeDataset(MappedRawSource(dir_ / "scan.raw", phantom.dims, phantom.voxel_size),
                   dir_ / "plain", options);

  // Always staged, even with staging off; the preview comes once the copy has passed its slices.
  options.stage_slow_sources = false;
  options.preview_options.slices = 8;
  std::vector<std::string> events;
  ImportPreview preview;
  options.preview = [&](const ImportPreview& p) {
    events.emplace_back("preview");
    preview = p;
  };
  options.progress = [&events](std::string_view stage, double) {
    if (events.empty() || events.back() != stage) {
      events.emplace_back(stage);
    }
  };
  const GzipRawSource gzip(dir_ / "scan.raw.gz",
                           RawLayout{phantom.dims, phantom.voxel_size, SampleType::kUInt16,
                                     std::endian::little, std::nullopt});
  const DatasetInfo info = writeDataset(gzip, dir_ / "gzip", options);
  EXPECT_EQ(events,
            (std::vector<std::string>{"staging", "preview", "histogram", "bricks", "levels"}));
  expectSameDataset(dir_ / "plain", dir_ / "gzip");
  EXPECT_EQ(info.active_voxel_count, reference.active_voxel_count);
  // Every slice passed: each preview slice shows its own, and the histogram is the volume's.
  EXPECT_TRUE(preview.complete());
  EXPECT_EQ(preview.slices, previewSlices(phantom.dims[2], preview.volume.dims[2]));
  EXPECT_EQ(preview.threshold, info.threshold);
  // Read directly, the first look reads the stream once, in order.
  const ImportPreview direct = readImportPreview(PhantomSource(phantom), options.preview_options);
  EXPECT_EQ(readImportPreview(gzip, options.preview_options).volume.data, direct.volume.data);
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
    bricks[brick] = toFloatGrid(readBrick(brickPath(dir_, 0, brick)));
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
  const auto overview = toFloatGrid(readBrick(dir_ / "overview.vdb", false));
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

TEST_F(DatasetTest, ReportsProgressPerStage) {
  DatasetOptions options;
  options.brick_size = 32;
  std::vector<std::pair<std::string, double>> calls;
  options.progress = [&calls](std::string_view stage, double fraction) {
    calls.emplace_back(stage, fraction);
  };
  writeDataset(PhantomSource(spec()), dir_, options);
  // Each stage in order, from 0 to 1, rising, at most once per percent.
  const std::vector<std::string> stages{"histogram", "bricks", "levels"};
  std::size_t stage = 0;
  double previous = -1.0;
  std::map<std::string, std::pair<double, double>> range;
  for (const auto& [name, fraction] : calls) {
    if (name != stages[stage]) {
      ++stage;
      ASSERT_LT(stage, stages.size());
      ASSERT_EQ(name, stages[stage]);
      previous = -1.0;
    }
    EXPECT_GT(fraction, previous);
    previous = fraction;
    range.try_emplace(name, fraction, fraction).first->second.second = fraction;
  }
  EXPECT_EQ(stage, 2U);
  for (const auto& name : stages) {
    EXPECT_DOUBLE_EQ(range.at(name).first, 0.0) << name;
    EXPECT_DOUBLE_EQ(range.at(name).second, 1.0) << name;
  }
  EXPECT_LE(calls.size(), 3U * 101U);
}

TEST_F(DatasetTest, OutsideAirAxesMatchTheInMemorySieveAndAreRecorded) {
  // A pipe along z that the first and last slice cut through.
  Volume16 volume({48, 48, 40}, 0.1);
  for (std::int64_t z = 0; z < 40; ++z) {
    for (std::int64_t y = 0; y < 48; ++y) {
      for (std::int64_t x = 0; x < 48; ++x) {
        const bool wall =
            x >= 8 && x < 40 && y >= 8 && y < 40 && !(x >= 12 && x < 36 && y >= 12 && y < 36);
        volume.at(x, y, z) = wall ? 20000 : 1000;
      }
    }
  }
  DatasetOptions options;
  options.brick_size = 16;
  options.outside_air_axes = parseAirAxes("xy");
  const DatasetInfo info = writeDataset(MemorySource(volume), dir_, options);
  SieveOptions sieve_options;
  sieve_options.outside_air_axes = options.outside_air_axes;
  EXPECT_EQ(info.active_voxel_count,
            static_cast<std::int64_t>(sieve(volume, sieve_options).grid->activeVoxelCount()));
  const Dataset dataset = Dataset::open(dir_);
  EXPECT_EQ(dataset.info().outside_air_axes, (AirAxes{true, true, false}));
  EXPECT_TRUE(dataset.sample(0, {24, 24, 20}).has_value());  // the inside of the pipe
  EXPECT_FALSE(dataset.sample(0, {2, 2, 20}).has_value());   // outside
}

TEST_F(DatasetTest, SmallVolumeIsItsOwnOverview) {
  PhantomSpec phantom = spec();
  phantom.dims = {64, 64, 64};
  phantom.voxel_size = 0.2;
  DatasetOptions options;  // brick size 256
  double levels_done = 0.0;
  options.progress = [&levels_done](std::string_view stage, double fraction) {
    if (stage == "levels") {
      levels_done = fraction;
    }
  };
  const DatasetInfo info = writeDataset(PhantomSource(phantom), dir_, options);
  EXPECT_DOUBLE_EQ(levels_done, 1.0);  // no coarser level: done at once
  ASSERT_EQ(info.levels.size(), 1U);
  EXPECT_EQ(readBrick(dir_ / "overview.vdb", false).base().activeVoxelCount(),
            static_cast<openvdb::Index64>(info.active_voxel_count));
}

/// A phantom that claims slow random access, like a TIFF stack, and counts the voxels read.
class SlowPhantomSource final : public VolumeSource {
 public:
  explicit SlowPhantomSource(const PhantomSpec& spec, bool slow = true,
                             SampleType type = SampleType::kUInt16)
      : phantom_(spec), slow_(slow), type_(type) {}
  [[nodiscard]] SampleType sampleType() const override { return type_; }
  [[nodiscard]] std::array<std::int64_t, 3> dims() const override { return phantom_.dims(); }
  [[nodiscard]] VoxelSize voxelSize() const override { return phantom_.voxelSize(); }
  [[nodiscard]] bool slowRandomAccess() const override { return slow_; }
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override {
    voxels_read_ += box.voxelCount();
    phantom_.readRegion(box, out);
  }
  [[nodiscard]] std::int64_t voxelsRead() const { return voxels_read_; }

 private:
  PhantomSource phantom_;
  bool slow_ = true;
  SampleType type_ = SampleType::kUInt16;
  mutable std::atomic<std::int64_t> voxels_read_ = 0;
};

/// Every brick of every level and the overview of two datasets hold the same voxels.
void expectSameDataset(const std::filesystem::path& a, const std::filesystem::path& b) {
  const DatasetInfo info_a = readDatasetInfo(a);
  const DatasetInfo info_b = readDatasetInfo(b);
  EXPECT_EQ(info_a.active_voxel_count, info_b.active_voxel_count);
  EXPECT_EQ(info_a.threshold, info_b.threshold);
  ASSERT_EQ(info_a.levels.size(), info_b.levels.size());
  for (std::size_t level = 0; level < info_a.levels.size(); ++level) {
    ASSERT_EQ(info_a.levels[level].bricks, info_b.levels[level].bricks);
    for (const Index3& brick : info_a.levels[level].bricks) {
      const auto grid_a =
          toFloatGrid(readBrick(brickPath(a, static_cast<int>(level), brick), false));
      const auto grid_b =
          toFloatGrid(readBrick(brickPath(b, static_cast<int>(level), brick), false));
      ASSERT_EQ(grid_a->activeVoxelCount(), grid_b->activeVoxelCount());
      const auto accessor = grid_b->getConstAccessor();
      for (auto it = grid_a->cbeginValueOn(); it; ++it) {
        ASSERT_TRUE(accessor.isValueOn(it.getCoord())) << it.getCoord();
        ASSERT_EQ(accessor.getValue(it.getCoord()), *it) << it.getCoord();
      }
    }
  }
}

TEST_F(DatasetTest, StagesSlowSourcesOnceAndWritesTheSameDataset) {
  const PhantomSpec phantom = spec();
  DatasetOptions options;
  options.brick_size = 32;
  options.margin_voxels = 3;
  std::vector<std::string> stages;
  options.progress = [&stages](std::string_view stage, double) {
    if (stages.empty() || stages.back() != stage) {
      stages.emplace_back(stage);
    }
  };
  const DatasetInfo reference = writeDataset(PhantomSource(phantom), dir_ / "direct", options);
  EXPECT_EQ(stages, (std::vector<std::string>{"histogram", "bricks", "levels"}));

  // Staged in the output directory: every voxel is read exactly once, and the copy is gone.
  const SlowPhantomSource slow(phantom);
  stages.clear();
  const DatasetInfo staged = writeDataset(slow, dir_ / "staged", options);
  EXPECT_EQ(stages, (std::vector<std::string>{"staging", "histogram", "bricks", "levels"}));
  EXPECT_EQ(slow.voxelsRead(), phantom.dims[0] * phantom.dims[1] * phantom.dims[2]);
  EXPECT_EQ(staged.active_voxel_count, reference.active_voxel_count);
  expectSameDataset(dir_ / "direct", dir_ / "staged");
  for (const auto& entry : std::filesystem::directory_iterator(dir_ / "staged")) {
    EXPECT_NE(entry.path().extension(), ".raw") << entry.path();
  }

  // Staged elsewhere: that directory is left empty, but for copies of processes still running.
  // A copy that a process left when it ended is removed; process 999999999 never runs.
  options.staging_dir = dir_ / "scratch";
  std::filesystem::create_directories(options.staging_dir);
  const auto left = options.staging_dir / ".voxelsieve-staging-999999999-1.raw";
  const auto in_use =
      options.staging_dir / (".voxelsieve-staging-" + std::to_string(processId()) + "-1.raw");
  std::ofstream(left) << "left";
  std::ofstream(in_use) << "in use";
  (void)writeDataset(SlowPhantomSource(phantom), dir_ / "elsewhere", options);
  expectSameDataset(dir_ / "direct", dir_ / "elsewhere");
  EXPECT_FALSE(std::filesystem::exists(left));
  EXPECT_TRUE(std::filesystem::exists(in_use));
  std::filesystem::remove(in_use);
  EXPECT_TRUE(std::filesystem::is_empty(dir_ / "scratch"));
  EXPECT_FALSE(processRunning(999999999));
  EXPECT_TRUE(processRunning(processId()));

  // Read directly when staging is off.
  options.stage_slow_sources = false;
  const SlowPhantomSource unstaged(phantom);
  (void)writeDataset(unstaged, dir_ / "unstaged", options);
  EXPECT_GT(unstaged.voxelsRead(), slow.voxelsRead());
  expectSameDataset(dir_ / "direct", dir_ / "unstaged");

  // Joined parts stage when any part is slow.
  PhantomSpec half = phantom;
  half.dims[2] /= 2;
  std::vector<std::unique_ptr<VolumeSource>> parts;
  parts.push_back(std::make_unique<PhantomSource>(half));
  parts.push_back(std::make_unique<SlowPhantomSource>(half));
  EXPECT_TRUE(ConcatSource(std::move(parts), 2).slowRandomAccess());
  EXPECT_FALSE(PhantomSource(half).slowRandomAccess());
}

TEST_F(DatasetTest, StagesEightBitInputInEightBit) {
  // Grey values of at most 255: an 8-bit scan, staged in 8 bit and stored as UInt8 grids.
  PhantomSpec phantom = spec();
  phantom.air_value = 20;
  phantom.material_value = 200;
  phantom.noise_sigma = 0.0;
  DatasetOptions options;
  options.brick_size = 32;
  options.stage_slow_sources = false;
  (void)writeDataset(SlowPhantomSource(phantom, true, SampleType::kUInt8), dir_ / "direct",
                     options);
  options.stage_slow_sources = true;
  std::vector<std::string> stages;
  options.progress = [&stages](std::string_view stage, double) {
    if (stages.empty() || stages.back() != stage) {
      stages.emplace_back(stage);
    }
  };
  const SlowPhantomSource source(phantom, true, SampleType::kUInt8);
  const DatasetInfo info = writeDataset(source, dir_ / "staged", options);
  EXPECT_EQ(stages.front(), "staging");
  EXPECT_EQ(info.value_type, ValueType::kUInt8);
  EXPECT_EQ(source.voxelsRead(), phantom.dims[0] * phantom.dims[1] * phantom.dims[2]);
  expectSameDataset(dir_ / "direct", dir_ / "staged");
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

TEST_F(DatasetTest, InterruptedImportResumesWithTheFinishedBricks) {
  const PhantomSpec phantom = spec();
  DatasetOptions options;
  options.brick_size = 16;
  const SlowPhantomSource fresh(phantom, /*slow=*/false);
  (void)writeDataset(fresh, dir_ / "whole", options);

  // Interrupted half way through the bricks: the journal stays, the dataset is not there yet.
  DatasetOptions interrupted = options;
  interrupted.progress = [](std::string_view stage, double fraction) {
    if (stage == "bricks" && fraction >= 0.5) {
      throw std::runtime_error("interrupted");
    }
  };
  const auto out = dir_ / "resumed";
  EXPECT_THROW((void)writeDataset(PhantomSource(phantom), out, interrupted), std::runtime_error);
  EXPECT_TRUE(std::filesystem::exists(out / "import.json"));
  EXPECT_FALSE(std::filesystem::exists(out / "index.json"));
  // A partial file that the interrupted process left is no content.
  std::ofstream(out / "level0" / "0_0_0.vdb.partial-999999999-0") << "cut off";

  // Nor for another scan of the same size, as the next part of a series: the journal names the
  // file it was written for.
  {
    const Volume16 volume = generatePhantom(phantom);
    writeRaw(dir_ / "a.raw", volume);
    writeRaw(dir_ / "b.raw", volume);
    const MappedRawSource a(dir_ / "a.raw", phantom.dims, phantom.voxel_size);
    const MappedRawSource b(dir_ / "b.raw", phantom.dims, phantom.voxel_size);
    EXPECT_NE(a.identity(), b.identity());
    EXPECT_EQ(a.identity(),
              MappedRawSource(dir_ / "a.raw", phantom.dims, phantom.voxel_size).identity());
    const auto series = dir_ / "series";
    EXPECT_THROW((void)writeDataset(a, series, interrupted), std::runtime_error);
    EXPECT_THROW((void)writeDataset(b, series, options), std::invalid_argument);
    // Who owns the directory, as the step of a project does, has it started again instead.
    DatasetOptions replace = options;
    replace.replace_other_import = true;
    (void)writeDataset(b, series, replace);
    expectSameDataset(dir_ / "whole", series);
  }

  // With other options the directory is not taken for this import.
  DatasetOptions other = options;
  other.margin_voxels = 5;
  EXPECT_THROW((void)writeDataset(PhantomSource(phantom), out, other), std::invalid_argument);
  DatasetOptions no_resume = options;
  no_resume.resume = false;
  EXPECT_THROW((void)writeDataset(PhantomSource(phantom), out, no_resume), std::invalid_argument);

  // The same options resume: no pass 1, and only the bricks not done yet are read.
  std::vector<std::string> stages;
  options.progress = [&stages](std::string_view stage, double) {
    if (stages.empty() || stages.back() != stage) {
      stages.emplace_back(stage);
    }
  };
  const SlowPhantomSource resumed(phantom, /*slow=*/false);
  (void)writeDataset(resumed, out, options);
  EXPECT_EQ(stages, (std::vector<std::string>{"bricks", "levels"}));
  EXPECT_LT(resumed.voxelsRead(), fresh.voxelsRead() * 3 / 4);
  expectSameDataset(dir_ / "whole", out);
  for (const char* name : {"import.json", "pass1.bin", "bricks.log"}) {
    EXPECT_FALSE(std::filesystem::exists(out / name)) << name;
  }
  EXPECT_FALSE(std::filesystem::exists(out / "level0" / "0_0_0.vdb.partial-999999999-0"));
}

// Value types (ADR 0021).

/// Voxels of the stored level-0 grids whose value differs from `volume`, and checks that every
/// brick holds `type`.
std::int64_t level0Mismatches(const std::filesystem::path& dir, const Volume16& volume,
                              ValueType type) {
  std::int64_t mismatches = 0;
  const DatasetInfo info = readDatasetInfo(dir);
  for (const Index3& brick : info.levels.at(0).bricks) {
    const GreyGrid grid = readBrick(brickPath(dir, 0, brick), false);
    EXPECT_EQ(grid.valueType(), type);
    grid.visit([&](const auto& typed) {
      for (auto it = typed.cbeginValueOn(); it; ++it) {
        const openvdb::Coord c = it.getCoord();
        mismatches +=
            static_cast<float>(*it) == static_cast<float>(volume.at(c.x(), c.y(), c.z())) ? 0 : 1;
      }
    });
  }
  return mismatches;
}

nlohmann::json readIndex(const std::filesystem::path& dir) {
  std::ifstream in(dir / "index.json");
  return nlohmann::json::parse(in);
}

// Dimensions whose product does not fit into 64 bits used to pass the size check by overflow and
// end in an allocation of the wrong size.
TEST_F(DatasetTest, DimensionsOutOfRangeAreRejected) {
  std::filesystem::create_directories(dir_);
  const std::filesystem::path path = dir_ / "tiny.raw";
  std::ofstream(path, std::ios::binary) << std::string(64, '\0');
  const std::int64_t huge = std::int64_t{1} << 32;
  EXPECT_THROW(MappedRawSource(path, {huge, huge, 1}, 1.0), std::invalid_argument);
  EXPECT_THROW(MappedRawSource(path, {huge, huge, huge}, 1.0), std::invalid_argument);
  EXPECT_THROW(MappedRawSource(path, {4, 0, 8}, 1.0), std::invalid_argument);
  EXPECT_THROW(MappedRawSource(path, {4, -2, 8}, 1.0), std::invalid_argument);
  EXPECT_NO_THROW(MappedRawSource(path, {4, 4, 2}, 1.0));
}

TEST_F(DatasetTest, ADamagedIndexIsAnErrorThatNamesTheFile) {
  PhantomSpec phantom = spec();
  phantom.dims = {40, 36, 20};
  DatasetOptions options;
  options.brick_size = 16;
  (void)writeDataset(PhantomSource(phantom), dir_, options);
  const std::filesystem::path index = dir_ / "index.json";
  std::ifstream in(index);
  const nlohmann::json good = nlohmann::json::parse(in);
  in.close();
  const auto damaged = [&](const std::function<void(nlohmann::json&)>& change) {
    nlohmann::json json = good;
    change(json);
    std::ofstream(index) << json.dump();
    try {
      (void)readDatasetInfo(dir_);
    } catch (const std::runtime_error& error) {
      return std::string(error.what());
    }
    return std::string();
  };
  EXPECT_EQ(damaged([](nlohmann::json&) {}), "");
  const std::int64_t huge = std::int64_t{1} << 40;
  for (const std::string& message :
       {damaged([&](nlohmann::json& j) { j["dims"] = {huge, huge, huge}; }),
        damaged([](nlohmann::json& j) { j["dims"] = {40, 0, 20}; }),
        damaged([](nlohmann::json& j) { j["brick_size"] = 0; }),
        damaged([](nlohmann::json& j) { j.erase("levels"); }),
        damaged([](nlohmann::json& j) { j["dims"] = "large"; })}) {
    EXPECT_NE(message.find("index.json"), std::string::npos) << message;
  }
}

TEST_F(DatasetTest, StoresSixteenBitInputExactlyAsUInt16) {
  const Volume16 volume = generatePhantom(spec());
  DatasetOptions options;
  options.brick_size = 32;
  const DatasetInfo info = writeDataset(MemorySource(volume), dir_, options);
  EXPECT_EQ(info.value_type, ValueType::kUInt16);
  EXPECT_EQ(readDatasetInfo(dir_).value_type, ValueType::kUInt16);
  EXPECT_EQ(readIndex(dir_).at("value_type"), "uint16");
  EXPECT_EQ(readIndex(dir_).at("coarse_levels"), "rounded mean");
  EXPECT_EQ(level0Mismatches(dir_, volume, ValueType::kUInt16), 0);
  EXPECT_EQ(readBrick(dir_ / "overview.vdb").valueType(), ValueType::kUInt16);
}

TEST_F(DatasetTest, CoarseLevelsOfIntegerDatasetsHoldTheRoundedMean) {
  DatasetOptions options;
  options.brick_size = 32;
  (void)writeDataset(PhantomSource(spec()), dir_, options);
  const Dataset dataset = Dataset::open(dir_);
  std::int64_t checked = 0;
  for (const Index3& brick : dataset.level(1).bricks) {
    dataset.brick(1, brick)->visit([&](const auto& grid) {
      std::int64_t n = 0;
      for (auto it = grid.cbeginValueOn(); it; ++it) {
        if (n++ % 17 != 0) {
          continue;  // a sample of the voxels keeps the test fast
        }
        const openvdb::Coord c = it.getCoord();
        double sum = 0.0;
        int count = 0;
        for (int child = 0; child < 8; ++child) {
          const auto value =
              dataset.sample(0, {2 * c.x() + (child & 1), 2 * c.y() + ((child >> 1) & 1),
                                 2 * c.z() + ((child >> 2) & 1)});
          if (value) {
            sum += *value;
            ++count;
          }
        }
        ASSERT_GT(count, 0);
        // The exact mean is a multiple of 1/8; rounding moves it by at most half a grey value.
        const double mean = sum / count;
        ASSERT_LE(std::abs(static_cast<double>(*it) - mean), 0.5) << c;
        ++checked;
      }
    });
  }
  EXPECT_GT(checked, 1000);
}

TEST_F(DatasetTest, FloatOnRequestHoldsTheSameValues) {
  const Volume16 volume = generatePhantom(spec());
  DatasetOptions options;
  options.brick_size = 32;
  options.value_type = ValueType::kFloat;
  const DatasetInfo info = writeDataset(MemorySource(volume), dir_, options);
  EXPECT_EQ(info.value_type, ValueType::kFloat);
  EXPECT_EQ(readIndex(dir_).at("value_type"), "float");
  EXPECT_FALSE(readIndex(dir_).contains("coarse_levels"));
  EXPECT_EQ(level0Mismatches(dir_, volume, ValueType::kFloat), 0);

  const auto native = dir_.string() + "_native";
  options.value_type.reset();
  const DatasetInfo native_info = writeDataset(MemorySource(volume), native, options);
  EXPECT_EQ(native_info.active_voxel_count, info.active_voxel_count);
  EXPECT_EQ(native_info.levels.at(0).bricks, info.levels.at(0).bricks);
  std::filesystem::remove_all(native);
}

TEST_F(DatasetTest, StoresEightBitInputExactlyAsUInt8) {
  PhantomSpec phantom = spec();
  phantom.dims = {64, 64, 64};
  phantom.voxel_size = 0.2;  // the same part at half the resolution
  Volume16 volume = generatePhantom(phantom);
  for (auto& value : volume.data) {
    value = static_cast<std::uint16_t>(value >> 8U);  // the phantom with 8-bit grey values
  }
  std::filesystem::create_directories(dir_);
  const auto raw = dir_ / "eight.raw";
  writeRawWithHeader(raw, volume, 0, 0, SampleType::kUInt8, std::endian::little);
  const MappedRawSource source(
      raw, RawLayout{phantom.dims, phantom.voxel_size, SampleType::kUInt8, std::endian::little, 0});
  EXPECT_EQ(source.sampleType(), SampleType::kUInt8);

  DatasetOptions options;
  options.brick_size = 32;
  const DatasetInfo info = writeDataset(source, dir_ / "eight.vsieve", options);
  EXPECT_EQ(info.value_type, ValueType::kUInt8);
  EXPECT_GT(info.active_voxel_count, 0);
  EXPECT_LT(info.active_voxel_count, volume.voxelCount());  // the outside air is gone
  EXPECT_EQ(level0Mismatches(dir_ / "eight.vsieve", volume, ValueType::kUInt8), 0);
  const Dataset dataset = Dataset::open(dir_ / "eight.vsieve");
  const Box box{{8, 24, 24}, {56, 40, 40}};
  std::vector<float> region(static_cast<std::size_t>(box.voxelCount()));
  dataset.readRegion(0, box, region, -1.0F);
  std::int64_t active = 0;
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
      for (std::int64_t x = box.min[0]; x < box.max[0]; ++x) {
        const float read = region[static_cast<std::size_t>(
            (x - box.min[0]) + box.size(0) * ((y - box.min[1]) + box.size(1) * (z - box.min[2])))];
        if (read >= 0.0F) {
          ASSERT_EQ(read, static_cast<float>(volume.at(x, y, z)));
          ++active;
        }
      }
    }
  }
  EXPECT_GT(active, 0);

  // Wider on request, never narrower.
  options.value_type = ValueType::kUInt16;
  EXPECT_EQ(writeDataset(source, dir_ / "sixteen.vsieve", options).value_type, ValueType::kUInt16);
  EXPECT_EQ(level0Mismatches(dir_ / "sixteen.vsieve", volume, ValueType::kUInt16), 0);
  options.value_type = ValueType::kUInt8;
  EXPECT_THROW(writeDataset(PhantomSource(phantom), dir_ / "narrow.vsieve", options),
               std::invalid_argument);
}

TEST_F(DatasetTest, ReadsFloatDatasetsOfFormatVersionOne) {
  const Volume16 volume = generatePhantom(spec());
  DatasetOptions options;
  options.brick_size = 32;
  options.value_type = ValueType::kFloat;
  (void)writeDataset(MemorySource(volume), dir_, options);
  // As written before ADR 0021: version 1, float grids.
  nlohmann::json index = readIndex(dir_);
  index["version"] = 1;
  std::ofstream(dir_ / "index.json") << index.dump(2);

  const Dataset dataset = Dataset::open(dir_);
  EXPECT_EQ(dataset.info().value_type, ValueType::kFloat);
  EXPECT_EQ(dataset.sample(0, {64, 64, 64}), static_cast<float>(volume.at(64, 64, 64)));

  index["version"] = 3;
  std::ofstream(dir_ / "index.json") << index.dump(2);
  EXPECT_THROW((void)readDatasetInfo(dir_), std::runtime_error);
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
  EXPECT_EQ(dataset.brick(0, {0, 0, 0}).get(), nullptr);
  EXPECT_THROW((void)dataset.sample(7, {0, 0, 0}), std::out_of_range);
}

TEST_F(DatasetReaderTest, SamplerMatchesSample) {
  (void)writeReference();
  const Dataset dataset = Dataset::open(dir_);
  for (const int level : {0, 1}) {
    const Index3 dims = dataset.level(level).dims;
    Dataset::Sampler sampler(dataset, level);
    std::int64_t active = 0;
    // One voxel beyond the volume on each side, x fastest, so the sampler crosses every brick.
    for (std::int64_t z = -1; z <= dims[2]; ++z) {
      for (std::int64_t y = -1; y <= dims[1]; ++y) {
        for (std::int64_t x = -1; x <= dims[0]; ++x) {
          const auto value = sampler({x, y, z});
          ASSERT_EQ(value, dataset.sample(level, {x, y, z})) << x << " " << y << " " << z;
          active += value ? 1 : 0;
        }
      }
    }
    if (level == 0) {
      EXPECT_EQ(active, dataset.info().active_voxel_count);
    }
  }
  // Jumping back and forth between bricks with a cache of one brick: the sampler's brick is
  // evicted while it still reads from it. The full scan above stays on the default cache, which
  // would otherwise reload a brick at every crossing and outlast the ASan timeout.
  const Dataset small = Dataset::open(dir_, 1);
  Dataset::Sampler sampler(small, 0);
  for (const Index3& voxel : {Index3{64, 64, 0}, Index3{0, 0, 0}, Index3{64, 64, 0},
                              Index3{127, 64, 64}, Index3{64, 64, 0}}) {
    EXPECT_EQ(sampler(voxel), small.sample(0, voxel));
  }
  EXPECT_THROW(Dataset::Sampler(small, 7), std::out_of_range);
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
          .base()
          .memUsage());
  const std::size_t budget = 3 * one_brick;
  const Dataset dataset = Dataset::open(dir_, budget);

  const auto& bricks = dataset.level(0).bricks;
  ASSERT_GT(bricks.size(), 10U);
  for (const Index3& brick : bricks) {
    ASSERT_NE(dataset.brick(0, brick).get(), nullptr);
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
  EXPECT_GT(held->base().activeVoxelCount(), 0U);
}

TEST_F(DatasetReaderTest, DatasetsShareOneCacheBudget) {
  (void)writeReference();
  const auto one_brick = static_cast<std::size_t>(
      readBrick(brickPath(dir_, 0, readDatasetInfo(dir_).levels[0].bricks.front()), false)
          .base()
          .memUsage());
  const auto cache = std::make_shared<BrickCache>(4 * one_brick);
  const Dataset first = Dataset::open(dir_, cache);
  auto second = std::make_optional(Dataset::open(dir_, cache));
  EXPECT_EQ(first.cache(), cache);

  const auto& bricks = first.level(0).bricks;
  ASSERT_GT(bricks.size(), 10U);
  for (const Index3& brick : bricks) {
    ASSERT_NE(first.brick(0, brick).get(), nullptr);
    ASSERT_NE(second->brick(0, brick).get(), nullptr);
    const CacheStats stats = cache->stats();
    EXPECT_TRUE(stats.bytes <= cache->budget() || stats.bricks == 1) << stats.bytes;
  }
  // Each dataset counts its own misses; the same brick of two datasets is cached twice.
  EXPECT_EQ(first.cacheStats().misses, bricks.size());
  EXPECT_EQ(second->cacheStats().misses, bricks.size());
  const std::size_t both = cache->stats().bricks;
  EXPECT_GE(both, 2U);

  // A closed dataset takes its bricks out of the cache.
  second.reset();
  EXPECT_LT(cache->stats().bricks, both);
  EXPECT_EQ(first.cacheStats().bricks, cache->stats().bricks);
  (void)first.brick(0, bricks.back());
  EXPECT_EQ(first.cacheStats().hits, 1U);
}

TEST_F(DatasetReaderTest, DatasetsWithoutACacheShareOne) {
  (void)writeReference();
  {
    const Dataset first = Dataset::open(dir_);
    const Dataset second = Dataset::open(dir_, BrickLoading::kOnAccess);
    const Dataset own = Dataset::open(dir_, std::size_t{1} << 20U);
    EXPECT_EQ(first.cache(), second.cache());
    EXPECT_EQ(first.cache(), Dataset::sharedCache());
    EXPECT_NE(own.cache(), first.cache());
    EXPECT_EQ(first.cache()->budget(), Dataset::defaultCacheBytes());

    // A smaller budget evicts at once.
    for (const Index3& brick : first.level(0).bricks) {
      (void)first.brick(0, brick);
    }
    ASSERT_GT(first.cache()->stats().bricks, 1U);
    Dataset::setSharedCacheBytes(1);
    EXPECT_EQ(first.cache()->budget(), 1U);
    EXPECT_EQ(first.cache()->stats().bricks, 1U);
  }
  // The budget set holds for the next shared cache too, until the default is restored.
  EXPECT_EQ(Dataset::sharedCache()->budget(), 1U);
  Dataset::setSharedCacheBytes(0);
  EXPECT_EQ(Dataset::sharedCache()->budget(), Dataset::defaultCacheBytes());
}

TEST_F(DatasetReaderTest, ConcurrentRequestsLoadABrickOnce) {
  (void)writeReference();
  const Dataset dataset = Dataset::open(dir_, std::size_t{1} << 30U);
  for (const Index3& brick : dataset.level(0).bricks) {
    constexpr int kThreads = 8;
    std::latch start(kThreads);
    std::vector<const GreyGrid*> seen(kThreads, nullptr);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
      threads.emplace_back([&, i] {
        start.arrive_and_wait();
        seen[static_cast<std::size_t>(i)] = dataset.brick(0, brick).get();
      });
    }
    for (std::thread& thread : threads) {
      thread.join();
    }
    EXPECT_NE(seen.front(), nullptr);
    EXPECT_EQ(std::count(seen.begin(), seen.end(), seen.front()), kThreads);
  }
  EXPECT_EQ(dataset.cacheStats().misses, dataset.level(0).bricks.size());
}

// Bricks evicted from a small cache while other threads load new ones: the freed memory goes back
// to the system now and then (releaseFreedMemory), and every brick still reads the same.
TEST_F(DatasetReaderTest, BricksReadTheSameThroughACacheThatKeepsEvicting) {
  (void)writeReference();
  const Dataset reference = Dataset::open(dir_, std::size_t{1} << 30U);
  const auto& bricks = reference.level(0).bricks;
  std::vector<openvdb::Index64> active(bricks.size());
  for (std::size_t i = 0; i < bricks.size(); ++i) {
    active[i] = reference.brick(0, bricks[i])->base().activeVoxelCount();
  }
  const auto one_brick =
      static_cast<std::size_t>(reference.brick(0, bricks.front())->base().memUsage());
  const Dataset small = Dataset::open(dir_, 2 * one_brick);
  // Enough rounds to free several times the 64 MB after which the memory goes back.
  const std::size_t rounds =
      std::max<std::size_t>(2, (std::size_t{256} << 20U) / (one_brick * bricks.size()) + 1);
  std::atomic<int> mismatches{0};
  for (std::size_t round = 0; round < rounds; ++round) {
    tbb::parallel_for(std::size_t{0}, bricks.size(), [&](std::size_t i) {
      if (small.brick(0, bricks[i])->base().activeVoxelCount() != active[i]) {
        ++mismatches;
      }
    });
  }
  EXPECT_EQ(mismatches.load(), 0);
  EXPECT_GE(small.cacheStats().misses, rounds * bricks.size() / 2);
  releaseFreedMemory();  // also safe to call at any time
}

#ifndef _WIN32
// OpenVDB copies files it loads on access to a temp file unless told not to (readBrick).
TEST_F(DatasetReaderTest, LoadingOnAccessWritesNoTempCopies) {
  (void)writeReference();
  const std::filesystem::path temp = dir_.string() + "_openvdb_temp";
  std::filesystem::create_directories(temp);
  setenv("OPENVDB_TEMP_DIR", temp.c_str(), 1);  // NOLINT(concurrency-mt-unsafe): no threads yet
  {
    const Dataset dataset = Dataset::open(dir_, BrickLoading::kOnAccess);
    std::vector<Dataset::BrickPtr> held;
    for (const Index3& brick : dataset.level(0).bricks) {
      held.push_back(dataset.brick(0, brick));
      const Box box = dataset.brickBox(0, brick);
      (void)dataset.sample(0, box.min);
    }
    ASSERT_FALSE(held.empty());
    EXPECT_TRUE(std::filesystem::is_empty(temp));
  }
  unsetenv("OPENVDB_TEMP_DIR");  // NOLINT(concurrency-mt-unsafe)
  std::filesystem::remove_all(temp);
}
#endif

TEST(CacheBudget, DefaultFollowsTheMemory) {
  constexpr std::size_t kGiB = std::size_t{1} << 30U;
  const std::uint64_t memory = usableMemoryBytes();
  ASSERT_GT(memory, 0U);
  EXPECT_EQ(memoryBudgetBytes(), memory / 4 * 3);
  const std::size_t budget = Dataset::defaultCacheBytes();
  EXPECT_GE(budget, memory / 4);
  EXPECT_LE(budget, std::max<std::uint64_t>(memory / 4, memory / 2));
  EXPECT_GE(budget, std::min<std::uint64_t>(2 * kGiB, memory / 2));

  // The shared cache and the work memory follow a budget that is set.
  const auto cache = Dataset::sharedCache();
  setMemoryBudget(std::uint64_t{6} * kGiB);
  EXPECT_EQ(memoryBudgetBytes(), 6 * kGiB);
  EXPECT_EQ(cache->budget(), 2 * kGiB);
  EXPECT_EQ(workMemoryBytes(), 4 * kGiB);
  EXPECT_EQ(tasksInMemory(kGiB), std::min<std::size_t>(4, static_cast<std::size_t>(threadLimit())));
  EXPECT_EQ(tasksInMemory(64 * kGiB), 1U);
  {
    // Every cache counts against the budget.
    const BrickCache own(kGiB);
    EXPECT_EQ(workMemoryBytes(), 3 * kGiB);
  }
  setMemoryBudget(0);
  EXPECT_EQ(cache->budget(), Dataset::defaultCacheBytes());
}

TEST(CacheBudget, AdmissionWaitsUntilTheOperationsFit) {
  constexpr std::uint64_t kGiB = std::uint64_t{1} << 30U;
  setMemoryBudget(6 * kGiB);  // 4 GiB of work memory beside the shared cache
  {
    Admission first = admitOperation(3 * kGiB);
    EXPECT_EQ(admittedOperations(), 1);
    EXPECT_EQ(workMemoryBytes(), 4 * kGiB);
    // Fits beside the first: admitted at once, and both get half of the work memory.
    {
      const Admission small = admitOperation(kGiB);
      EXPECT_EQ(workMemoryBytes(), 2 * kGiB);
    }
    // Does not fit: waits until the first is done.
    std::atomic<bool> admitted{false};
    std::thread second([&] {
      const Admission waiting = admitOperation(2 * kGiB);
      admitted = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(admitted.load());
    first = Admission();
    second.join();
    EXPECT_TRUE(admitted.load());
    EXPECT_EQ(admittedOperations(), 0);

    // A waiting operation stops when it is cancelled.
    const Admission big = admitOperation(4 * kGiB);
    const std::atomic<bool> cancel{true};
    EXPECT_THROW((void)admitOperation(kGiB, &cancel), std::runtime_error);
    EXPECT_EQ(admittedOperations(), 1);
  }
  // More than the work memory still runs when it is alone.
  {
    const Admission alone = admitOperation(64 * kGiB);
    EXPECT_EQ(admittedOperations(), 1);
  }
  setMemoryBudget(0);
}

TEST(ThreadLimit, LimitsAndRestoresParallelism) {
  const int all = threadLimit();
  setThreadLimit(2);
  EXPECT_EQ(threadLimit(), 2);
  std::atomic<int> busy{0};
  std::atomic<int> most{0};
  tbb::parallel_for(0, 64, [&](int) {
    const int now = ++busy;
    int seen = most.load();
    while (now > seen && !most.compare_exchange_weak(seen, now)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    --busy;
  });
  EXPECT_LE(most.load(), 2);
  setThreadLimit(0);
  EXPECT_EQ(threadLimit(), all);
  EXPECT_THROW(setThreadLimit(-1), std::invalid_argument);
}

TEST_F(DatasetReaderTest, ParallelBrickIterationSeesEveryVoxel) {
  (void)writeReference();
  // A tiny budget forces eviction while several threads load bricks.
  const Dataset dataset = Dataset::open(dir_, 1);
  for (int level = 0; level < static_cast<int>(dataset.info().levels.size()); ++level) {
    std::atomic<std::int64_t> active{0};
    std::atomic<std::size_t> visited{0};
    dataset.forEachGrid(level, [&](const Index3& brick, const auto& grid) {
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

TEST_F(DatasetReaderTest, InterruptedDerivedDatasetResumes) {
  (void)writeReference();
  const Dataset input = Dataset::open(dir_);
  const auto doubled = [&](const Index3& /*brick*/, const Box& box, std::vector<float>& values) {
    input.readRegion(0, box, values);
    for (float& value : values) {
      value = value > 30000.0F ? std::numeric_limits<float>::quiet_NaN() : value / 2.0F;
    }
  };
  const std::filesystem::path fresh_dir = dir_.string() + "_fresh";
  const std::filesystem::path resumed_dir = dir_.string() + "_resumed";
  std::filesystem::remove_all(fresh_dir);
  std::filesystem::remove_all(resumed_dir);
  const DatasetInfo fresh = writeDerivedDataset(input, fresh_dir, doubled);

  DerivedDatasetOptions options;
  options.resume_key = "halved";
  std::size_t calls = 0;
  const std::size_t stop = input.level(0).bricks.size() / 2;
  EXPECT_THROW((void)writeDerivedDataset(
                   input, resumed_dir,
                   [&](const Index3& brick, const Box& box, std::vector<float>& values) {
                     if (++calls == stop) {
                       throw std::runtime_error("interrupted");
                     }
                     doubled(brick, box, values);
                   },
                   options),
               std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(resumed_dir / "index.json"));

  // Other processing into the same directory is refused; the same resumes.
  DerivedDatasetOptions other = options;
  other.resume_key = "other";
  EXPECT_THROW((void)writeDerivedDataset(input, resumed_dir, doubled, other),
               std::invalid_argument);
  EXPECT_THROW((void)writeDerivedDataset(input, resumed_dir, doubled), std::invalid_argument);
  std::size_t again = 0;
  std::size_t kept = 0;
  options.finished = [&](const Index3&, const Box&) { ++kept; };
  const DatasetInfo resumed = writeDerivedDataset(
      input, resumed_dir,
      [&](const Index3& brick, const Box& box, std::vector<float>& values) {
        ++again;
        doubled(brick, box, values);
      },
      options);
  EXPECT_GT(kept, 0U);
  EXPECT_EQ(kept + again, input.level(0).bricks.size());
  EXPECT_EQ(resumed.active_voxel_count, fresh.active_voxel_count);
  EXPECT_EQ(resumed.levels.front().bricks, fresh.levels.front().bricks);
  EXPECT_FALSE(std::filesystem::exists(resumed_dir / "derived.json"));
  const Dataset a = Dataset::open(fresh_dir);
  const Dataset b = Dataset::open(resumed_dir);
  const Box all{{0, 0, 0}, a.info().dims};
  std::vector<float> va(static_cast<std::size_t>(all.voxelCount()));
  std::vector<float> vb(va.size());
  a.readRegion(0, all, va, -1.0F);
  b.readRegion(0, all, vb, -1.0F);
  EXPECT_EQ(va, vb);
  std::filesystem::remove_all(fresh_dir);
  std::filesystem::remove_all(resumed_dir);
}

TEST_F(DatasetReaderTest, TypedRegionsMatchTheFloatRegion) {
  (void)writeReference();
  const Dataset dataset = Dataset::open(dir_);
  ASSERT_EQ(dataset.info().value_type, ValueType::kUInt16);
  const Box box{{3, 17, 30}, {101, 70, 99}};
  const auto voxels = static_cast<std::size_t>(box.voxelCount());
  std::vector<float> grey(voxels);
  std::vector<std::uint16_t> narrow(voxels);
  dataset.readRegion(0, box, grey, 7.0F);
  dataset.readRegion(0, box, narrow, std::uint16_t{7});
  for (std::size_t i = 0; i < voxels; ++i) {
    ASSERT_EQ(static_cast<float>(narrow[i]), grey[i]) << i;
  }
  std::vector<std::uint8_t> bytes(voxels);
  EXPECT_THROW(dataset.readRegion(0, box, std::span<std::uint8_t>(bytes)), std::invalid_argument);
}

TEST_F(DatasetReaderTest, RegionsReadTilesAndLeavesAlike) {
  // A brick with a constant tile and a partly active leaf, read through the dataset.
  (void)writeReference();
  const Dataset dataset = Dataset::open(dir_);
  const Index3 index = dataset.level(0).bricks.front();
  const Box box = dataset.brickBox(0, index);
  auto grid = UInt16Grid::create(0);
  const openvdb::Coord tile(static_cast<int>(box.min[0]), static_cast<int>(box.min[1]),
                            static_cast<int>(box.min[2]));
  grid->tree().addTile(1, tile, 1234, true);
  grid->tree().setValueOn(tile + openvdb::Coord(9, 1, 2), 77);
  grid->setName("density");
  openvdb::io::File(brickPath(dir_, 0, index).string()).write({grid});
  const Dataset fresh = Dataset::open(dir_, std::size_t{1} << 30U);
  std::vector<std::uint16_t> values(static_cast<std::size_t>(box.voxelCount()));
  fresh.readRegion(0, box, values, std::uint16_t{1});
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto x = static_cast<std::int64_t>(i) % box.size(0);
    const auto y = static_cast<std::int64_t>(i) / box.size(0) % box.size(1);
    const auto z = static_cast<std::int64_t>(i) / box.size(0) / box.size(1);
    const auto value = fresh.sample(0, {box.min[0] + x, box.min[1] + y, box.min[2] + z});
    ASSERT_EQ(values[i], value ? static_cast<std::uint16_t>(*value) : 1) << i;
  }
  EXPECT_EQ(values[0], 1234);
  EXPECT_EQ(values[static_cast<std::size_t>(9 + box.size(0) * (1 + box.size(1) * 2))], 77);
}

TEST_F(DatasetReaderTest, HeldBricksArePinnedAndOnAccessBricksChargedFully) {
  (void)writeReference();
  const Dataset dataset = Dataset::open(dir_, 1);
  const auto& bricks = dataset.level(0).bricks;
  ASSERT_GT(bricks.size(), 3U);
  {
    const Dataset::BrickPtr held = dataset.brick(0, bricks.front());
    for (const Index3& brick : bricks) {
      (void)dataset.brick(0, brick);
    }
    // The held brick stayed in the cache: asking again is a hit, not a second copy.
    const std::uint64_t misses = dataset.cacheStats().misses;
    EXPECT_EQ(dataset.brick(0, bricks.front()), held);
    EXPECT_EQ(dataset.cacheStats().misses, misses);
    const CacheStats stats = dataset.cacheStats();
    EXPECT_EQ(stats.pinned, 1U);
    EXPECT_GT(stats.pinned_bytes, 0U);
    EXPECT_EQ(stats.bricks, 2U);  // the held brick and the most recent one
  }
  EXPECT_EQ(dataset.cacheStats().pinned, 0U);

  // A brick loaded on access counts with all its leaves before any is read.
  const Dataset on_access = Dataset::open(dir_, std::size_t{1} << 30U, BrickLoading::kOnAccess);
  const Dataset::BrickPtr brick = on_access.brick(0, bricks.front());
  const auto leaves = brick->visit([](const auto& grid) { return grid.tree().leafCount(); });
  EXPECT_GE(on_access.cacheStats().bytes, leaves * 512 * sizeof(std::uint16_t));
}

TEST_F(DatasetReaderTest, BrickSweepKeepsItsTasksInMemoryAndPinsTheHalo) {
  (void)writeReference();
  const Dataset dataset = Dataset::open(dir_, std::size_t{1} << 30U);
  std::atomic<int> busy{0};
  std::atomic<int> most{0};
  std::atomic<std::size_t> visited{0};
  BrickSweep sweep;
  sweep.task_bytes = std::uint64_t{1} << 62U;  // one task at a time
  sweep.halo = 1;
  dataset.forEachBrick(
      0,
      [&](const Index3& brick, const GreyGrid&) {
        const int now = ++busy;
        int seen = most.load();
        while (now > seen && !most.compare_exchange_weak(seen, now)) {
        }
        // The brick and its stored neighbours are pinned.
        std::size_t stored = 0;
        for (std::int64_t dz = -1; dz <= 1; ++dz) {
          for (std::int64_t dy = -1; dy <= 1; ++dy) {
            for (std::int64_t dx = -1; dx <= 1; ++dx) {
              stored += dataset.hasBrick(0, {brick[0] + dx, brick[1] + dy, brick[2] + dz}) ? 1 : 0;
            }
          }
        }
        EXPECT_EQ(dataset.cacheStats().pinned, stored);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        --busy;
        ++visited;
      },
      sweep);
  EXPECT_EQ(most.load(), 1);
  EXPECT_EQ(visited.load(), dataset.level(0).bricks.size());
}

/// What a failed read of a file mapping raises: an I/O error of the device.
void failMappedRead() {
#if defined(_WIN32)
  RaiseException(EXCEPTION_IN_PAGE_ERROR, 0, 0, nullptr);
#else
  (void)std::raise(SIGBUS);
#endif
}

TEST(MappedRead, ReadErrorsOfAMappingBecomeExceptions) {
#if !defined(_WIN32) && defined(VOXELSIEVE_TEST_SANITIZED)
  GTEST_SKIP() << "the sanitizers handle SIGBUS themselves";
#endif
  int value = 0;
  detail::readMapped("scan.raw", [&] { value = 1; });
  EXPECT_EQ(value, 1);
  EXPECT_THROW(detail::readMapped("scan.raw", [] { failMappedRead(); }), std::runtime_error);
  // Other exceptions pass unchanged, and the next read is guarded again.
  EXPECT_THROW(detail::readMapped("scan.raw", [] { throw std::invalid_argument("other"); }),
               std::invalid_argument);
  try {
    detail::readMapped("scan.raw", [] { failMappedRead(); });
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("scan.raw"), std::string::npos);
  }
}

}  // namespace
}  // namespace voxelsieve