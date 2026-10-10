#include <gtest/gtest.h>
#include <openvdb/io/File.h>

#include <filesystem>
#include <stdexcept>
#include <string>

#include "voxelsieve/phantom.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/vdb.hpp"

namespace voxelsieve {
namespace {

TEST(Vdb, DenseGridKeepsEveryVoxelLossless) {
  PhantomSpec spec = defaultPhantomSpec();
  spec.dims = {40, 40, 40};
  spec.noise_sigma = 300.0;
  const Volume16 volume = generatePhantom(spec);

  const auto grid = toDenseFloatGrid(volume);
  EXPECT_EQ(grid->activeVoxelCount(), volume.voxelCount());
  EXPECT_DOUBLE_EQ(grid->voxelSize()[0], spec.voxel_size[0]);

  const auto accessor = grid->getConstAccessor();
  for (const auto& [x, y, z] : {std::array<int, 3>{0, 0, 0}, {5, 17, 33}, {39, 39, 39}}) {
    EXPECT_EQ(accessor.getValue(openvdb::Coord(x, y, z)), static_cast<float>(volume.at(x, y, z)));
  }
}

TEST(Vdb, ConvertsGreyValuesExactlyOrRefuses) {
  PhantomSpec spec = defaultPhantomSpec();
  spec.dims = {24, 24, 24};
  const Volume16 volume = generatePhantom(spec);
  const auto grid = toDenseFloatGrid(volume);

  const GreyGrid uint16 = convertGrid(*grid, ValueType::kUInt16);
  EXPECT_EQ(uint16.valueType(), ValueType::kUInt16);
  EXPECT_EQ(uint16.base().getName(), "density");
  EXPECT_EQ(uint16.base().activeVoxelCount(), grid->activeVoxelCount());
  const auto back = toFloatGrid(uint16);
  for (auto it = grid->cbeginValueOn(); it; ++it) {
    ASSERT_EQ(back->tree().getValue(it.getCoord()), *it);
  }
  float value = 0.0F;
  ASSERT_TRUE(uint16.probeValue(openvdb::Coord(3, 4, 5), value));
  EXPECT_EQ(value, static_cast<float>(volume.at(3, 4, 5)));
  EXPECT_EQ(toFloatGrid(GreyGrid(grid)), grid);  // float stays as it is

  // Never quantised: fractions, and values beyond the type.
  auto fraction = openvdb::FloatGrid::create(0.0F);
  fraction->tree().setValue(openvdb::Coord(1, 2, 3), 1.5F);
  EXPECT_THROW((void)convertGrid(*fraction, ValueType::kUInt16), std::runtime_error);
  auto wide = openvdb::FloatGrid::create(0.0F);
  wide->tree().setValue(openvdb::Coord(1, 2, 3), 300.0F);
  EXPECT_THROW((void)convertGrid(*wide, ValueType::kUInt8), std::runtime_error);
  EXPECT_EQ(convertGrid(*wide, ValueType::kUInt16).valueType(), ValueType::kUInt16);
  // Active tiles keep their value.
  auto tiles = openvdb::FloatGrid::create(0.0F);
  tiles->tree().fill(openvdb::CoordBBox({0, 0, 0}, {63, 63, 63}), 200.0F, /*active=*/true);
  const GreyGrid tiled = convertGrid(*tiles, ValueType::kUInt8);
  EXPECT_EQ(tiled.base().activeVoxelCount(), 64U * 64U * 64U);
  ASSERT_TRUE(tiled.probeValue(openvdb::Coord(40, 50, 60), value));
  EXPECT_EQ(value, 200.0F);
}

TEST(Vdb, IntegerGridsRoundTripThroughFiles) {
  initializeVdb();
  PhantomSpec spec = defaultPhantomSpec();
  spec.dims = {16, 16, 16};
  const auto grid = toDenseFloatGrid(generatePhantom(spec));
  const auto path = std::filesystem::temp_directory_path() /
                    ("voxelsieve_vdb_" +
                     std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".vdb");
  for (const ValueType type : {ValueType::kUInt16, ValueType::kFloat}) {
    writeVdb(path, {convertGrid(*grid, type).basePtr()});
    openvdb::io::File file(path.string());
    file.open();
    const GreyGrid read = GreyGrid::fromBase(file.readGrid("density"));
    file.close();
    ASSERT_TRUE(read);
    EXPECT_EQ(read.valueType(), type);
    EXPECT_EQ(read.base().activeVoxelCount(), grid->activeVoxelCount());
    float value = 0.0F;
    ASSERT_TRUE(read.probeValue(openvdb::Coord(7, 8, 9), value));
    EXPECT_EQ(value, grid->tree().getValue(openvdb::Coord(7, 8, 9)));
  }
  std::filesystem::remove(path);
  EXPECT_EQ(parseValueType("uint8"), ValueType::kUInt8);
  EXPECT_EQ(valueTypeName(ValueType::kUInt16), "uint16");
  EXPECT_THROW((void)parseValueType("int16"), std::invalid_argument);
}

}  // namespace
}  // namespace voxelsieve
