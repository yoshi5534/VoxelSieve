#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "dicom_writer.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/dicom.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/studio.hpp"

namespace voxelsieve {
namespace {

using testing::DicomSliceSpec;
using testing::writeDicomSlice;

constexpr std::array<std::int64_t, 3> kDims{24, 20, 12};

/// The stored value of a voxel: a block of material (around +1000) in air (around -1000), each
/// voxel different so that a wrong order or offset shows.
std::int32_t storedValue(std::int64_t x, std::int64_t y, std::int64_t z) {
  const bool inside = x >= 6 && x < 18 && y >= 5 && y < 15 && z >= 3 && z < 9;
  return static_cast<std::int32_t>(inside ? 1000 + x + 2 * y + 3 * z
                                          : -1000 + (7 * x + 3 * y + z) % 50);
}

std::vector<std::int32_t> sliceValues(std::int64_t z,
                                      std::int32_t (*value)(std::int64_t, std::int64_t,
                                                            std::int64_t)) {
  std::vector<std::int32_t> values;
  for (std::int64_t y = 0; y < kDims[1]; ++y) {
    for (std::int64_t x = 0; x < kDims[0]; ++x) {
      values.push_back(value(x, y, z));
    }
  }
  return values;
}

std::vector<std::uint16_t> readAll(const VolumeSource& source) {
  const auto dims = source.dims();
  std::vector<std::uint16_t> out(static_cast<std::size_t>(dims[0] * dims[1] * dims[2]));
  source.readRegion({{0, 0, 0}, dims}, out);
  return out;
}

class DicomTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_dicom_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  /// The slice spec of a stack that lies along -x: rows along -z, columns along +y.
  static DicomSliceSpec tiltedSpec() {
    DicomSliceSpec spec;
    spec.rows = static_cast<std::uint16_t>(kDims[1]);
    spec.columns = static_cast<std::uint16_t>(kDims[0]);
    spec.orientation = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    spec.pixel_spacing = {0.5, 0.25};
    spec.thickness = 0.6;
    spec.slope = 2.0;
    spec.intercept = -1024.0;
    return spec;
  }

  static std::array<double, 3> slicePosition(std::int64_t z) {
    // The slice normal is row x column = (0,1,0) x (0,0,-1) = (-1,0,0); slices 0.8 mm apart.
    return {12.0 - 0.8 * static_cast<double>(z), -3.0, 40.0};
  }

  /// Writes the test volume with the files named and numbered in an order other than the slice
  /// order, so that only the positions give it.
  std::filesystem::path writeTiltedStack(const std::string& folder = "stack") {
    DicomSliceSpec spec = tiltedSpec();
    for (std::int64_t z = 0; z < kDims[2]; ++z) {
      const std::int64_t shuffled = (z * 5 + 3) % kDims[2];
      spec.instance = static_cast<int>(shuffled + 1);
      spec.position = slicePosition(z);
      writeDicomSlice(dir_ / folder / ("img" + std::to_string(shuffled) + ".dcm"), spec,
                      sliceValues(z, storedValue));
    }
    return dir_ / folder;
  }

  std::filesystem::path dir_;
};

TEST_F(DicomTest, ReadsSignedSlicesInPositionOrder) {
  const DicomStackSource source(writeTiltedStack());
  EXPECT_EQ(source.dims(), kDims);
  EXPECT_TRUE(source.isSigned());
  EXPECT_EQ(source.bitsStored(), 16);
  EXPECT_EQ(source.sampleType(), SampleType::kUInt16);
  const VoxelSize size = source.voxelSize();
  EXPECT_DOUBLE_EQ(size[0], 0.25);  // between columns
  EXPECT_DOUBLE_EQ(size[1], 0.5);   // between rows
  EXPECT_NEAR(size[2], 0.8, 1e-9);
  EXPECT_NEAR(size.slice_thickness_mm, 0.6, 1e-9);

  // value = intercept + slope * stored, and grey = stored + 32768.
  const ValueMapping mapping = source.valueMapping();
  EXPECT_DOUBLE_EQ(mapping.scale, 2.0);
  EXPECT_DOUBLE_EQ(mapping.offset, -1024.0 - 2.0 * 32768.0);
  const auto grey = readAll(source);
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    for (std::int64_t y = 0; y < kDims[1]; ++y) {
      for (std::int64_t x = 0; x < kDims[0]; ++x) {
        const auto g = grey[static_cast<std::size_t>((z * kDims[1] + y) * kDims[0] + x)];
        ASSERT_EQ(g, storedValue(x, y, z) + 32768) << x << " " << y << " " << z;
        ASSERT_DOUBLE_EQ(mapping.toValue(g), -1024.0 + 2.0 * storedValue(x, y, z));
      }
    }
  }
  // A region in the middle reads the same voxels.
  const Box box{{3, 4, 2}, {17, 9, 7}};
  std::vector<std::uint16_t> region(static_cast<std::size_t>(box.voxelCount()));
  source.readRegion(box, region);
  EXPECT_EQ(region.front(), storedValue(3, 4, 2) + 32768);
  EXPECT_EQ(region.back(), storedValue(16, 8, 6) + 32768);

  // The pose maps each voxel onto its place in the coordinates of the files.
  const RigidTransform pose = source.filePose();
  for (const std::int64_t z : {0, 5, 11}) {
    const auto first = pose.apply(size.toMm({0.0, 0.0, static_cast<double>(z)}));
    const auto expected = slicePosition(z);
    for (std::size_t axis = 0; axis < 3; ++axis) {
      EXPECT_NEAR(first[axis], expected[axis], 1e-6) << z;
    }
  }
  const auto corner = pose.apply(size.toMm({2.0, 3.0, 0.0}));
  EXPECT_NEAR(corner[0], 12.0, 1e-6);
  EXPECT_NEAR(corner[1], -3.0 + 2 * 0.25, 1e-6);
  EXPECT_NEAR(corner[2], 40.0 - 3 * 0.5, 1e-6);
  EXPECT_EQ(source.files().front().filename(), "img3.dcm");
}

std::int32_t twelveBitValue(std::int64_t x, std::int64_t y, std::int64_t z) {
  // 12 bits stored; the bits above them hold an overlay and must be masked off.
  return static_cast<std::int32_t>(((x * 131 + y * 17 + z * 7) % 4096) | 0xA000);
}

TEST_F(DicomTest, ReadsDataSetsWithoutMetaHeaderAndRleByInstanceNumber) {
  DicomSliceSpec spec;
  spec.rows = static_cast<std::uint16_t>(kDims[1]);
  spec.columns = static_cast<std::uint16_t>(kDims[0]);
  spec.is_signed = false;
  spec.bits_stored = 12;
  spec.pixel_spacing = {0.1, 0.1};
  spec.spacing_between_slices = 0.2;
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    spec.instance = static_cast<int>(z + 1);
    const bool rle = z % 2 == 1;
    spec.transfer = rle ? EXS_RLELossless : EXS_LittleEndianImplicit;
    spec.file_format = rle;  // the others without preamble and meta header
    writeDicomSlice(dir_ / "plain" / ("slice_" + std::to_string(kDims[2] - z) + ".dcm"), spec,
                    sliceValues(z, twelveBitValue));
  }
  const DicomStackSource source(dir_ / "plain");
  EXPECT_FALSE(source.isSigned());
  EXPECT_EQ(source.sampleType(), SampleType::kUInt16);  // 12 bits do not fit into 8
  EXPECT_TRUE(source.valueMapping().isIdentity());
  EXPECT_NEAR(source.voxelSize()[2], 0.2, 1e-9);
  EXPECT_EQ(source.filePose().matrix(), RigidTransform{}.matrix());
  const auto grey = readAll(source);
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    for (std::int64_t x = 0; x < kDims[0]; ++x) {
      ASSERT_EQ(grey[static_cast<std::size_t>((z * kDims[1] + 7) * kDims[0] + x)],
                twelveBitValue(x, 7, z) & 0x0FFF)
          << x << " " << z;
    }
  }
}

TEST_F(DicomTest, EightBitSlicesAreAnEightBitVolume) {
  DicomSliceSpec spec;
  spec.rows = static_cast<std::uint16_t>(kDims[1]);
  spec.columns = static_cast<std::uint16_t>(kDims[0]);
  spec.is_signed = false;
  spec.bits_stored = 8;
  const auto value = [](std::int64_t x, std::int64_t y, std::int64_t z) {
    return static_cast<std::int32_t>((x * 31 + y * 7 + z * 3) % 256);
  };
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    spec.instance = static_cast<int>(z + 1);
    writeDicomSlice(dir_ / "eight" / ("slice_" + std::to_string(z) + ".dcm"), spec,
                    sliceValues(z, value));
  }
  const DicomStackSource source(dir_ / "eight");
  EXPECT_EQ(source.bitsStored(), 8);
  EXPECT_EQ(source.sampleType(), SampleType::kUInt8);
  const auto grey = readAll(source);
  EXPECT_EQ(grey[static_cast<std::size_t>((2 * kDims[1] + 5) * kDims[0] + 3)], value(3, 5, 2));
}

TEST_F(DicomTest, ChoosesTheLargestSeriesAndRefusesGaps) {
  writeTiltedStack("mixed");
  DicomSliceSpec other = tiltedSpec();
  other.series = "1.2.826.0.1.3680043.2.1125.2";
  for (std::int64_t z = 0; z < 3; ++z) {
    other.position = slicePosition(z);
    writeDicomSlice(dir_ / "mixed" / ("other" + std::to_string(z) + ".dcm"), other,
                    sliceValues(z, storedValue));
  }
  std::ofstream(dir_ / "mixed" / "README") << "not an image";
  std::ofstream(dir_ / "mixed" / "notes.txt") << "not looked at";

  const DicomStackSource largest(dir_ / "mixed");
  EXPECT_EQ(largest.dims()[2], kDims[2]);
  ASSERT_EQ(largest.otherSeries().size(), 1U);
  EXPECT_EQ(largest.otherSeries().front().first, other.series);
  EXPECT_EQ(largest.otherSeries().front().second, 3U);
  EXPECT_EQ(largest.skippedFiles(), 1U);  // README; notes.txt is not a candidate

  DicomStackOptions options;
  options.series = other.series;
  EXPECT_EQ(DicomStackSource(dir_ / "mixed", options).dims()[2], 3);

  std::filesystem::remove(dir_ / "mixed" / "img8.dcm");  // a slice in the middle
  EXPECT_ANY_THROW(DicomStackSource(dir_ / "mixed"));
  EXPECT_ANY_THROW(DicomStackSource(dir_ / "nothing"));
}

TEST_F(DicomTest, RefusesSlicesThatDoNotFit) {
  DicomSliceSpec spec = tiltedSpec();
  spec.position = slicePosition(0);
  writeDicomSlice(dir_ / "a" / "0.dcm", spec, sliceValues(0, storedValue));
  spec.position = slicePosition(1);
  spec.intercept = 0.0;  // another value mapping
  writeDicomSlice(dir_ / "a" / "1.dcm", spec, sliceValues(1, storedValue));
  EXPECT_ANY_THROW(DicomStackSource(dir_ / "a"));

  // The files given one by one must all be images.
  std::ofstream(dir_ / "b.dcm") << "no DICOM";
  EXPECT_ANY_THROW(
      DicomStackSource(std::vector<std::filesystem::path>{dir_ / "a" / "0.dcm", dir_ / "b.dcm"}));
}

TEST_F(DicomTest, StudioImportsAndPlacesTheStack) {
  const auto folder = writeTiltedStack("input");
  Studio studio({});
  const auto listing = studio.call("browse", {{"path", dir_.string()}});
  std::map<std::string, std::string> kinds;
  for (const auto& entry : listing.at("entries")) {
    kinds[entry.at("name")] = entry.at("kind");
  }
  EXPECT_EQ(kinds["input"], "dicom");
  EXPECT_EQ(studio.call("browse", {{"path", folder.string()}}).at("entries").front().at("kind"),
            "dicom");

  studio.call("project_create", {{"path", (dir_ / "project").string()}});
  const auto imported =
      studio.call("run_import_dicom", {{"path", folder.string()}, {"brick_size", 16}});
  ASSERT_EQ(imported.at("status"), "done") << imported.dump();
  EXPECT_EQ(imported.at("summary").at("slices"), kDims[2]);
  EXPECT_EQ(imported.at("summary").at("signed"), true);
  EXPECT_DOUBLE_EQ(imported.at("summary").at("value_mapping").at("scale").get<double>(), 2.0);

  const auto objects = studio.call("objects", {}).at("objects");
  ASSERT_EQ(objects.size(), 1U);
  const auto pose = objects.front().at("pose").get<std::vector<double>>();
  const auto expected = DicomStackSource(folder).filePose().matrix();
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_NEAR(pose[i], expected[i], 1e-9) << i;
  }

  // The material keeps its grey values in the dataset.
  const Dataset dataset =
      Dataset::open(dir_ / "project" / "steps" / "1-import_dicom" / "dataset.vsieve");
  EXPECT_EQ(dataset.info().dims, kDims);
  EXPECT_EQ(dataset.sample(0, {10, 8, 5}), static_cast<float>(storedValue(10, 8, 5) + 32768));
}

}  // namespace
}  // namespace voxelsieve
