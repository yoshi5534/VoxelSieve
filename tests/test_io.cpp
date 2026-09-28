#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>

#include "voxelsieve/io.hpp"
#include "voxelsieve/phantom.hpp"

namespace voxelsieve {
namespace {

class IoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_io_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
            "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

TEST_F(IoTest, RawRoundTrip) {
  PhantomSpec spec = defaultPhantomSpec();
  spec.dims = {16, 24, 8};
  spec.noise_sigma = 100.0;
  const Volume16 original = generatePhantom(spec);

  const auto path = dir_ / "volume.raw";
  writeRaw(path, original);
  EXPECT_EQ(std::filesystem::file_size(path), 16U * 24U * 8U * 2U);

  const Volume16 loaded = readRaw(path, spec.dims, spec.voxel_size);
  EXPECT_EQ(loaded.dims, original.dims);
  EXPECT_EQ(loaded.data, original.data);
}

TEST_F(IoTest, ReadRawRejectsWrongDimensions) {
  const Volume16 volume({4, 4, 4}, 1.0);
  const auto path = dir_ / "small.raw";
  writeRaw(path, volume);
  EXPECT_THROW((void)readRaw(path, {4, 4, 5}, 1.0), std::runtime_error);
}

TEST(PhantomJson, ContainsGroundTruth) {
  const PhantomSpec spec = defaultPhantomSpec();
  const auto json = phantomToJson(spec);
  EXPECT_EQ(json["format"]["dtype"], "uint16");
  EXPECT_EQ(json["dims"][0], 128);
  EXPECT_EQ(json["phantom"]["pores"].size(), spec.pores.size());
  EXPECT_DOUBLE_EQ(json["phantom"]["material_volume_mm3"].get<double>(),
                   phantomMaterialVolumeMm3(spec));
}

}  // namespace
}  // namespace voxelsieve
