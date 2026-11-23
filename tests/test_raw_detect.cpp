// Raw files without a sidecar (ADR 0039): the layout from the name or from the grey values,
// checked against phantoms whose layout is known.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "voxelsieve/phantom.hpp"
#include "voxelsieve/source.hpp"

namespace voxelsieve {
namespace {

class RawDetect : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("vs_raw_detect_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  /// A noisy phantom of `dims` written as raw samples behind a header of `header` bytes.
  std::filesystem::path write(const std::string& name, const std::array<std::int64_t, 3>& dims,
                              SampleType type = SampleType::kUInt16,
                              std::endian order = std::endian::little, std::size_t header = 0) {
    PhantomSpec spec = defaultPhantomSpec();
    spec.dims = dims;
    spec.voxel_size = VoxelSize(0.1);
    spec.noise_sigma = 600.0;
    spec.outer_size_mm = {0.7 * static_cast<double>(dims[0]) * 0.1,
                          0.7 * static_cast<double>(dims[1]) * 0.1,
                          0.7 * static_cast<double>(dims[2]) * 0.1};
    spec.wall_thickness_mm = 1.2;
    spec.pores.clear();
    const auto path = dir_ / name;
    std::ofstream out(path, std::ios::binary);
    const std::vector<char> zeros(header, 'H');
    out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    for (std::int64_t z = 0; z < dims[2]; ++z) {
      for (std::int64_t y = 0; y < dims[1]; ++y) {
        for (std::int64_t x = 0; x < dims[0]; ++x) {
          const std::uint16_t value = phantomValue(spec, x, y, z);
          if (type == SampleType::kUInt8) {
            out.put(static_cast<char>(value >> 8U));
          } else if (order == std::endian::little) {
            out.put(static_cast<char>(value & 0xFFU));
            out.put(static_cast<char>(value >> 8U));
          } else {
            out.put(static_cast<char>(value >> 8U));
            out.put(static_cast<char>(value & 0xFFU));
          }
        }
      }
    }
    return path;
  }

  std::filesystem::path dir_;
};

TEST_F(RawDetect, FindsRowsAndSlicesInTheGreyValues) {
  const std::array<std::int64_t, 3> dims = {150, 124, 70};
  const auto guess_found = detectRawLayout(write("scan.raw", dims));
  ASSERT_TRUE(guess_found.has_value());
  const RawLayoutGuess guess = guess_found.value_or(RawLayoutGuess{});
  EXPECT_EQ(guess.layout.dims, dims);
  EXPECT_EQ(guess.layout.sample_type, SampleType::kUInt16);
  EXPECT_EQ(guess.layout.byte_order, std::endian::little);
  EXPECT_EQ(guess.layout.header_bytes, 0U);
  EXPECT_EQ(guess.dims_from, "grey values");
  EXPECT_GT(guess.confidence, 0.5);
  EXPECT_FALSE(guess.voxel_size_known);
}

TEST_F(RawDetect, SkipsAHeader) {
  const std::array<std::int64_t, 3> dims = {128, 128, 96};
  const auto guess_found =
      detectRawLayout(write("scan.vol", dims, SampleType::kUInt16, std::endian::little, 2048));
  ASSERT_TRUE(guess_found.has_value());
  const RawLayoutGuess guess = guess_found.value_or(RawLayoutGuess{});
  EXPECT_EQ(guess.layout.dims, dims);
  EXPECT_EQ(guess.layout.header_bytes, 2048U);
}

TEST_F(RawDetect, TellsTheByteOrderAndEightBits) {
  const std::array<std::int64_t, 3> dims = {112, 96, 64};
  const auto big_found =
      detectRawLayout(write("big.raw", dims, SampleType::kUInt16, std::endian::big));
  ASSERT_TRUE(big_found.has_value());
  const RawLayoutGuess big = big_found.value_or(RawLayoutGuess{});
  EXPECT_EQ(big.layout.dims, dims);
  EXPECT_EQ(big.layout.byte_order, std::endian::big);
  const auto bytes_found = detectRawLayout(write("bytes.raw", dims, SampleType::kUInt8));
  ASSERT_TRUE(bytes_found.has_value());
  const RawLayoutGuess bytes = bytes_found.value_or(RawLayoutGuess{});
  EXPECT_EQ(bytes.layout.dims, dims);
  EXPECT_EQ(bytes.layout.sample_type, SampleType::kUInt8);
}

TEST_F(RawDetect, ReadsTheName) {
  const std::array<std::int64_t, 3> dims = {100, 90, 40};
  const auto guess_found = detectRawLayout(write("part_100x90x40_16bit_0.05mm.raw", dims));
  ASSERT_TRUE(guess_found.has_value());
  const RawLayoutGuess guess = guess_found.value_or(RawLayoutGuess{});
  EXPECT_EQ(guess.layout.dims, dims);
  EXPECT_EQ(guess.dims_from, "file name");
  ASSERT_TRUE(guess.voxel_size_known);
  EXPECT_DOUBLE_EQ(guess.layout.voxel_size[0], 0.05);

  const auto micrometres_found =
      detectRawLayout(write("part 100 x 90 x 40 uint8 25um.raw", dims, SampleType::kUInt8));
  ASSERT_TRUE(micrometres_found.has_value());
  const RawLayoutGuess micrometres = micrometres_found.value_or(RawLayoutGuess{});
  EXPECT_EQ(micrometres.layout.sample_type, SampleType::kUInt8);
  EXPECT_DOUBLE_EQ(micrometres.layout.voxel_size[0], 0.025);
  EXPECT_EQ(micrometres.layout.header_bytes, 0U);

  // The size tells 8 from 16 bits; the grey values the byte order.
  const auto big_found = detectRawLayout(
      write("scan_100x90x40.raw", dims, SampleType::kUInt16, std::endian::big, 512));
  ASSERT_TRUE(big_found.has_value());
  const RawLayoutGuess big = big_found.value_or(RawLayoutGuess{});
  EXPECT_EQ(big.layout.sample_type, SampleType::kUInt16);
  EXPECT_EQ(big.layout.byte_order, std::endian::big);
  EXPECT_EQ(big.layout.header_bytes, 512U);
}

TEST_F(RawDetect, RefusesFloatsAndDimensionsTooLargeForTheFile) {
  const std::array<std::int64_t, 3> dims = {64, 64, 32};
  EXPECT_THROW((void)detectRawLayout(write("scan_float32.raw", dims)), std::invalid_argument);
  EXPECT_THROW((void)detectRawLayout(write("scan_640x640x320.raw", dims)), std::invalid_argument);
}

TEST_F(RawDetect, GivesNothingForNoise) {
  const auto path = dir_ / "noise.bin";
  std::ofstream out(path, std::ios::binary);
  std::uint32_t state = 12345;
  for (int i = 0; i < 2'000'000; ++i) {
    state = (state * 1664525U) + 1013904223U;
    out.put(static_cast<char>(state >> 24U));
  }
  out.close();
  EXPECT_FALSE(detectRawLayout(path));
}

}  // namespace
}  // namespace voxelsieve
