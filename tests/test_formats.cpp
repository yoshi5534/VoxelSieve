// More import formats (ADR 0044): raw files of signed, 32-bit and float samples, MetaImage and
// NRRD headers, PNG stacks, checked against the phantom whose grey values are known.

#include <gtest/gtest.h>
#include <hdf5.h>
#include <zip.h>
#include <zlib.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "damaged_files.hpp"
#include "detail/png.hpp"
#include "voxelsieve/first_look.hpp"
#include "voxelsieve/gui.hpp"
#include "voxelsieve/hdf5.hpp"
#include "voxelsieve/header_volume.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/png_stack.hpp"
#include "voxelsieve/source.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

constexpr std::array<std::int64_t, 3> kDims = {24, 20, 16};

/// The grey values of a small phantom, x fastest.
std::vector<std::uint16_t> phantomVoxels() {
  PhantomSpec spec = defaultPhantomSpec();
  spec.dims = kDims;
  spec.voxel_size = VoxelSize(0.1);
  spec.outer_size_mm = {1.6, 1.4, 1.1};
  spec.wall_thickness_mm = 0.3;
  spec.pores.clear();
  std::vector<std::uint16_t> voxels;
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    for (std::int64_t y = 0; y < kDims[1]; ++y) {
      for (std::int64_t x = 0; x < kDims[0]; ++x) {
        voxels.push_back(phantomValue(spec, x, y, z));
      }
    }
  }
  return voxels;
}

/// The bytes of `values` as samples of type T in `order`.
template <class T>
std::string samplesOf(const std::vector<T>& values, std::endian order = std::endian::little) {
  std::string bytes;
  for (const T value : values) {
    std::array<char, sizeof(T)> b{};
    std::memcpy(b.data(), &value, sizeof(T));
    if (order != std::endian::native) {
      std::reverse(b.begin(), b.end());
    }
    bytes.append(b.data(), b.size());
  }
  return bytes;
}

/// Floats of the phantom: an attenuation-like range with negative values.
std::vector<float> floatsOf(const std::vector<std::uint16_t>& voxels) {
  std::vector<float> values;
  values.reserve(voxels.size());
  for (const std::uint16_t v : voxels) {
    values.push_back(static_cast<float>(v) * 1e-5F - 0.05F);
  }
  return values;
}

std::vector<std::int16_t> signedOf(const std::vector<std::uint16_t>& voxels) {
  std::vector<std::int16_t> values;
  values.reserve(voxels.size());
  for (const std::uint16_t v : voxels) {
    values.push_back(static_cast<std::int16_t>(static_cast<int>(v) / 2 - 12000));
  }
  return values;
}

/// gzip (`gzip` true) or zlib data of `bytes`.
std::string compressed(const std::string& bytes, bool gzip) {
  z_stream z{};
  EXPECT_EQ(
      deflateInit2(&z, 6, Z_DEFLATED, gzip ? 16 + MAX_WBITS : MAX_WBITS, 8, Z_DEFAULT_STRATEGY),
      Z_OK);
  std::string out(deflateBound(&z, static_cast<uLong>(bytes.size())), '\0');
  z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));
  z.avail_in = static_cast<uInt>(bytes.size());
  z.next_out = reinterpret_cast<Bytef*>(out.data());
  z.avail_out = static_cast<uInt>(out.size());
  EXPECT_EQ(deflate(&z, Z_FINISH), Z_STREAM_END);
  out.resize(z.total_out);
  deflateEnd(&z);
  return out;
}

std::vector<std::uint16_t> readAll(const VolumeSource& source) {
  const auto dims = source.dims();
  std::vector<std::uint16_t> out(static_cast<std::size_t>(dims[0] * dims[1] * dims[2]));
  source.readRegion(Box{{0, 0, 0}, dims}, out);
  return out;
}

/// Whether the grey values of `source` give back `values` within half a step of the mapping.
template <class T>
void expectValues(const VolumeSource& source, const std::vector<T>& values) {
  const std::vector<std::uint16_t> grey = readAll(source);
  ASSERT_EQ(grey.size(), values.size());
  const ValueMapping mapping = source.valueMapping();
  const double tolerance = 0.5 * mapping.scale + 1e-9;
  for (std::size_t i = 0; i < grey.size(); ++i) {
    ASSERT_NEAR(mapping.toValue(grey[i]), static_cast<double>(values[i]), tolerance) << i;
  }
}

class Formats : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("vs_formats_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  // Callers often only need the file, so the path may be ignored.
  // NOLINTNEXTLINE(modernize-use-nodiscard)
  std::filesystem::path write(const std::string& name, const std::string& bytes) const {
    const auto path = dir_ / name;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
  }

  std::filesystem::path dir_;
  std::vector<std::uint16_t> voxels_ = phantomVoxels();
};

// --- Raw files of other sample types -----------------------------------------------------------

TEST_F(Formats, ReadsFloatRawWithAnEstimatedRange) {
  const std::vector<float> values = floatsOf(voxels_);
  const auto path = write("scan.raw", std::string(100, 'H') + samplesOf(values, std::endian::big));
  RawLayout layout(kDims, VoxelSize(0.1), SampleType::kUInt16, std::endian::big, 100);
  layout.stored_type = RawSampleType::kFloat32;
  const MappedRawSource source(path, layout);
  EXPECT_EQ(source.sampleType(), SampleType::kUInt16);
  const auto range = source.valueRange();
  ASSERT_TRUE(range.has_value());
  const auto [low, high] = std::ranges::minmax(values);
  // Widened by 10 % of the values' span on each side.
  EXPECT_NEAR(range.value_or(std::array{0.0, 0.0})[0], low - 0.1 * (high - low), 1e-6);
  EXPECT_NEAR(range.value_or(std::array{0.0, 0.0})[1], high + 0.1 * (high - low), 1e-6);
  expectValues(source, values);
  EXPECT_EQ(source.clippedValues(), 0U);
}

TEST_F(Formats, ClipsFloatsOutsideAGivenRangeAndCountsThem) {
  const std::vector<float> values = floatsOf(voxels_);
  const auto path = write("scan.raw", samplesOf(values));
  RawLayout layout(kDims, VoxelSize(0.1));
  layout.stored_type = RawSampleType::kFloat32;
  layout.value_range = std::array{0.0, 0.2};
  const MappedRawSource source(path, layout);
  const auto negative = static_cast<std::uint64_t>(
      std::ranges::count_if(values, [](float v) { return std::round(v / 0.2 * 65535.0) < 0.0; }));
  ASSERT_GT(negative, 0U);
  (void)readAll(source);
  (void)readAll(source);  // the same region again is not counted twice
  EXPECT_EQ(source.clippedValues(), negative);
}

TEST_F(Formats, KeepsSignedSamplesExactly) {
  const std::vector<std::int16_t> values = signedOf(voxels_);
  const auto path = write("scan.raw", samplesOf(values));
  RawLayout layout(kDims, VoxelSize(0.1));
  layout.stored_type = RawSampleType::kInt16;
  const MappedRawSource source(path, layout);
  EXPECT_EQ(source.valueMapping(), (ValueMapping{-32768.0, 1.0}));
  EXPECT_FALSE(source.valueRange());
  const std::vector<std::uint16_t> grey = readAll(source);
  for (std::size_t i = 0; i < grey.size(); ++i) {
    ASSERT_EQ(static_cast<int>(grey[i]) - 32768, values[i]) << i;
  }
}

TEST_F(Formats, KeepsThirtyTwoBitIntegersThatFitAndMapsLargerOnes) {
  const std::vector<std::uint32_t> small(voxels_.begin(), voxels_.end());
  RawLayout layout(kDims, VoxelSize(0.1));
  layout.stored_type = RawSampleType::kUInt32;
  const MappedRawSource kept(write("small.raw", samplesOf(small)), layout);
  EXPECT_TRUE(kept.valueMapping().isIdentity());
  EXPECT_EQ(readAll(kept), voxels_);

  std::vector<std::uint32_t> large;
  large.reserve(voxels_.size());
  for (const std::uint16_t v : voxels_) {
    large.push_back(std::uint32_t{v} * 1000U);
  }
  const MappedRawSource mapped(write("large.raw", samplesOf(large)), layout);
  EXPECT_FALSE(mapped.valueMapping().isIdentity());
  expectValues(mapped, large);
}

TEST_F(Formats, ReadsGzippedFloatsAndDoubles) {
  const std::vector<float> floats = floatsOf(voxels_);
  RawLayout layout(kDims, VoxelSize(0.1));
  layout.stored_type = RawSampleType::kFloat32;
  const GzipRawSource gz(write("scan.raw.gz", compressed(samplesOf(floats), true)), layout);
  expectValues(gz, floats);
  const std::vector<double> doubles(floats.begin(), floats.end());
  layout.stored_type = RawSampleType::kFloat64;
  const auto source =
      openRawVolume(write("doubles.raw.gz", compressed(samplesOf(doubles), true)), layout);
  expectValues(*source, doubles);
}

TEST_F(Formats, FindsFloatsFromTheFileNameAndTheSidecar) {
  const std::vector<float> values = floatsOf(voxels_);
  const auto named = write("engine_24x20x16_float32.raw", samplesOf(values));
  const RawInput input = resolveRawInput(named, Json::object());
  EXPECT_EQ(input.layout.stored_type, RawSampleType::kFloat32);
  EXPECT_EQ(input.from.at("dims"), "file name");
  const auto unnamed = write("part.raw", samplesOf(values));
  write("part.json", Json{{"dims", kDims},
                          {"voxel_size_mm", 0.1},
                          {"format", {{"dtype", "float32"}, {"value_range", {-0.1, 0.7}}}}}
                         .dump());
  const RawInput sidecar = resolveRawInput(unnamed, Json::object());
  EXPECT_EQ(sidecar.layout.stored_type, RawSampleType::kFloat32);
  EXPECT_EQ(sidecar.layout.value_range, (std::array{-0.1, 0.7}));
  EXPECT_THROW((void)resolveRawInput(unnamed, {{"sample_type", "complex64"}}),
               std::invalid_argument);
  const Json inspected = inspectScan(named);
  EXPECT_EQ(inspected.at("params").at("sample_type"), "float32");
}

// --- MetaImage ----------------------------------------------------------------------------------

TEST_F(Formats, ReadsMetaImageWithADataFile) {
  const std::vector<std::int16_t> values = signedOf(voxels_);
  write("part.raw", samplesOf(values, std::endian::big));
  const auto header = write("part.mhd",
                            "ObjectType = Image\nNDims = 3\nBinaryData = True\n"
                            "BinaryDataByteOrderMSB = True\nCompressedData = False\n"
                            "TransformMatrix = 0 1 0 -1 0 0 0 0 1\nOffset = 10 -20 5.5\n"
                            "ElementSpacing = 0.1 0.2 0.3\nDimSize = 24 20 16\n"
                            "ElementType = MET_SHORT\nElementDataFile = part.raw\n");
  ASSERT_TRUE(isHeaderVolumeFile(header));
  const HeaderVolume volume = readHeaderVolume(header);
  EXPECT_EQ(volume.format, "metaimage");
  EXPECT_EQ(volume.layout.dims, kDims);
  EXPECT_TRUE(volume.voxel_size_known);
  EXPECT_DOUBLE_EQ(volume.layout.voxel_size[1], 0.2);
  EXPECT_DOUBLE_EQ(volume.layout.voxel_size[2], 0.3);
  // The x axis points along +y, the y axis along -x.
  EXPECT_EQ(volume.pose.apply({1.0, 0.0, 0.0}), (std::array{10.0, -19.0, 5.5}));
  EXPECT_EQ(volume.pose.apply({0.0, 1.0, 0.0}), (std::array{9.0, -20.0, 5.5}));
  const auto source = openHeaderVolume(volume);
  const std::vector<std::uint16_t> grey = readAll(*source);
  for (std::size_t i = 0; i < grey.size(); ++i) {
    ASSERT_EQ(static_cast<int>(grey[i]) - 32768, values[i]) << i;
  }
  // Opened from the data file too, through its header.
  const Json inspected = inspectScan(dir_ / "part.raw");
  EXPECT_EQ(inspected.at("kind"), "metaimage");
  EXPECT_EQ(inspected.at("operation"), "import_header_volume");
  EXPECT_EQ(inspected.at("path"), header.string());
}

TEST_F(Formats, ReadsCompressedMetaImageWithTheDataAttached) {
  const std::vector<float> values = floatsOf(voxels_);
  const auto header = write("part.mha",
                            "NDims = 3\nDimSize = 24 20 16\nElementType = MET_FLOAT\n"
                            "CompressedData = True\nElementSpacing = 0.1 0.1 0.1\n"
                            "ElementDataFile = LOCAL\n" +
                                compressed(samplesOf(values), false));
  const HeaderVolume volume = readHeaderVolume(header);
  EXPECT_TRUE(volume.compressed);
  const auto source = openHeaderVolume(volume);
  EXPECT_TRUE(source->sequentialAccess());
  expectValues(*source, values);
}

TEST_F(Formats, ReadsSlicesNamedByAPatternWithOneValueRange) {
  const std::vector<float> values = floatsOf(voxels_);
  const std::int64_t slice = kDims[0] * kDims[1];
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    const std::vector<float> part(values.begin() + static_cast<std::ptrdiff_t>(z * slice),
                                  values.begin() + static_cast<std::ptrdiff_t>((z + 1) * slice));
    write("slices/s" + std::string(z + 1 < 10 ? "0" : "") + std::to_string(z + 1) + ".raw",
          samplesOf(part));
  }
  const auto header = write("slices/stack.mhd",
                            "NDims = 3\nDimSize = 24 20 16\nElementType = MET_FLOAT\n"
                            "ElementDataFile = s%02d.raw 1 16 1\n");
  const HeaderVolume volume = readHeaderVolume(header);
  ASSERT_EQ(volume.files.size(), 16U);
  EXPECT_FALSE(volume.voxel_size_known);
  const auto source = openHeaderVolume(volume);
  expectValues(*source, values);
}

// --- NRRD ---------------------------------------------------------------------------------------

TEST_F(Formats, ReadsNrrdWithAttachedData) {
  const auto header = write("part.nrrd",
                            "NRRD0004\n# a comment\ntype: unsigned short\ndimension: 3\n"
                            "space: left-posterior-superior\nsizes: 24 20 16\n"
                            "space directions: (0.1,0,0) (0,0.1,0) (0,0,0.25)\n"
                            "kinds: domain domain domain\nendian: little\nencoding: raw\n"
                            "space origin: (1,2,3)\nmodality:=CT\n\n" +
                                samplesOf(voxels_));
  ASSERT_TRUE(isHeaderVolumeFile(header));
  const HeaderVolume volume = readHeaderVolume(header);
  EXPECT_EQ(volume.format, "nrrd");
  EXPECT_DOUBLE_EQ(volume.layout.voxel_size[2], 0.25);
  EXPECT_EQ(volume.pose.translation, (std::array{1.0, 2.0, 3.0}));
  const auto source = openHeaderVolume(volume);
  EXPECT_EQ(readAll(*source), voxels_);
}

TEST_F(Formats, ReadsDetachedGzipNrrdAndListsOfFiles) {
  write("part.raw.gz", compressed(samplesOf(voxels_, std::endian::big), true));
  const auto gz = write("part.nhdr",
                        "NRRD0005\ntype: uint16\ndimension: 3\nsizes: 24 20 16\n"
                        "spacings: 0.05 0.05 0.05\nendian: big\nencoding: gzip\n"
                        "data file: part.raw.gz\n");
  EXPECT_EQ(readAll(*openHeaderVolume(readHeaderVolume(gz))), voxels_);

  const std::size_t half = voxels_.size() / 2;
  write("a.raw", samplesOf(std::vector(voxels_.begin(),
                                       voxels_.begin() + static_cast<std::ptrdiff_t>(half))));
  write("b.raw",
        samplesOf(std::vector(voxels_.begin() + static_cast<std::ptrdiff_t>(half), voxels_.end())));
  const auto listed = write("listed.nhdr",
                            "NRRD0004\ntype: ushort\ndimension: 3\nsizes: 24 20 16\n"
                            "endian: little\nencoding: raw\ndata file: LIST\na.raw\nb.raw\n");
  const HeaderVolume volume = readHeaderVolume(listed);
  EXPECT_EQ(volume.files.size(), 2U);
  EXPECT_EQ(readAll(*openHeaderVolume(volume)), voxels_);
}

TEST_F(Formats, RefusesHeadersThatAreNoGreyVolume) {
  const auto colour = write("colour.nrrd",
                            "NRRD0004\ntype: uchar\ndimension: 4\nsizes: 3 24 20 16\n"
                            "encoding: raw\n\n");
  EXPECT_THROW((void)readHeaderVolume(colour), std::invalid_argument);
  const auto text =
      write("text.nrrd", "NRRD0004\ntype: uchar\ndimension: 3\nsizes: 2 2 2\nencoding: ascii\n\n");
  EXPECT_THROW((void)readHeaderVolume(text), std::invalid_argument);
  const auto channels = write("rgb.mhd",
                              "NDims = 3\nDimSize = 2 2 2\nElementType = MET_UCHAR\n"
                              "ElementNumberOfChannels = 3\nElementDataFile = rgb.raw\n");
  EXPECT_THROW((void)readHeaderVolume(channels), std::invalid_argument);
  const auto missing = write("missing.mhd",
                             "NDims = 3\nDimSize = 2 2 2\nElementType = MET_UCHAR\n"
                             "ElementDataFile = nowhere.raw\n");
  EXPECT_THROW((void)readHeaderVolume(missing), std::invalid_argument);
}

TEST_F(Formats, SurvivesDamagedHeaders) {
  const auto read = [](const std::filesystem::path& file) {
    const HeaderVolume volume = readHeaderVolume(file);
    (void)readAll(*openHeaderVolume(volume));
  };
  write("part.raw", samplesOf(voxels_));
  write("part.mhd",
        "NDims = 3\nDimSize = 24 20 16\nElementType = MET_USHORT\nElementSpacing = 0.1 0.1 0.1\n"
        "ElementDataFile = part.raw\n");
  readDamagedCopies(dir_ / "part.mhd", 300, read);
  write("part.nrrd",
        "NRRD0004\ntype: float\ndimension: 3\nsizes: 24 20 16\nendian: little\n"
        "encoding: gzip\n\n" +
            compressed(samplesOf(floatsOf(voxels_)), true));
  readDamagedCopies(dir_ / "part.nrrd", 300, read);
}

// --- PNG stacks ---------------------------------------------------------------------------------

/// The phantom as 8-bit PNG slices: the upper byte of each grey value.
std::vector<std::vector<std::uint8_t>> pngSlices(const std::vector<std::uint16_t>& voxels) {
  std::vector<std::vector<std::uint8_t>> slices;
  const auto slice = static_cast<std::size_t>(kDims[0] * kDims[1]);
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    std::vector<std::uint8_t> pixels;
    for (std::size_t i = 0; i < slice; ++i) {
      pixels.push_back(
          static_cast<std::uint8_t>(voxels[static_cast<std::size_t>(z) * slice + i] >> 8U));
    }
    slices.push_back(detail::encodePng(static_cast<std::uint32_t>(kDims[0]),
                                       static_cast<std::uint32_t>(kDims[1]), 1, pixels));
  }
  return slices;
}

std::vector<std::uint16_t> upperBytes(const std::vector<std::uint16_t>& voxels) {
  std::vector<std::uint16_t> bytes;
  bytes.reserve(voxels.size());
  for (const std::uint16_t v : voxels) {
    bytes.push_back(static_cast<std::uint16_t>(v >> 8U));
  }
  return bytes;
}

TEST_F(Formats, ReadsPngStacksFromDirectoriesAndArchives) {
  const auto slices = pngSlices(voxels_);
  for (std::size_t z = 0; z < slices.size(); ++z) {
    // Numbers by value: slice2 before slice10.
    write("scan_50um/rec/slice" + std::to_string(z) + ".png",
          std::string(slices[z].begin(), slices[z].end()));
  }
  write("scan_50um/labels/slice0.png", std::string(slices[0].begin(), slices[0].end()));
  const PngStackSource stack(dir_ / "scan_50um");
  EXPECT_EQ(stack.folder(), "rec");
  EXPECT_EQ(stack.otherFolders(), std::vector<std::string>{"labels"});
  EXPECT_EQ(stack.dims(), kDims);
  EXPECT_EQ(stack.sampleType(), SampleType::kUInt8);
  EXPECT_TRUE(stack.voxelSizeKnown());
  EXPECT_DOUBLE_EQ(stack.voxelSize()[0], 0.05);
  EXPECT_EQ(readAll(stack), upperBytes(voxels_));
  EXPECT_TRUE(containsPngSlices(dir_ / "scan_50um"));
  EXPECT_EQ(inspectScan(dir_ / "scan_50um").at("kind"), "png");

  const auto zip_path = dir_ / "stack.zip";
  int error = 0;
  zip_t* archive = zip_open(zip_path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
  ASSERT_NE(archive, nullptr);
  for (std::size_t z = 0; z < slices.size(); ++z) {
    zip_source_t* source = zip_source_buffer(archive, slices[z].data(), slices[z].size(), 0);
    ASSERT_GE(zip_file_add(archive, ("png/" + std::to_string(z) + ".png").c_str(), source, 0), 0);
  }
  ASSERT_EQ(zip_close(archive), 0);
  EXPECT_TRUE(containsPngSlices(zip_path));
  const PngStackSource zipped(zip_path);
  EXPECT_FALSE(zipped.voxelSizeKnown());
  EXPECT_EQ(readAll(zipped), upperBytes(voxels_));
  EXPECT_EQ(inspectScan(zip_path).at("operation"), "import_png");
}

// --- HDF5 ---------------------------------------------------------------------------------------

/// Writes `values` as dataset `name` of `file` (created when `create`), shape z, y, x with
/// `leading` extra dimensions of size 1 in front, chunked and deflated; with the attribute
/// element_size_um when `um` is given.
template <class T>
void writeHdf5(const std::filesystem::path& file, const std::string& name,
               const std::vector<T>& values, hid_t type, bool create, int leading = 0,
               std::optional<std::array<double, 3>> um = std::nullopt) {
  const hid_t f = create ? H5Fcreate(file.string().c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT)
                         : H5Fopen(file.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
  ASSERT_GE(f, 0);
  std::vector<hsize_t> dims(static_cast<std::size_t>(leading), 1);
  dims.insert(dims.end(), {static_cast<hsize_t>(kDims[2]), static_cast<hsize_t>(kDims[1]),
                           static_cast<hsize_t>(kDims[0])});
  std::vector<hsize_t> chunk = dims;
  chunk[dims.size() - 3] = 4;
  const hid_t space = H5Screate_simple(static_cast<int>(dims.size()), dims.data(), nullptr);
  const hid_t properties = H5Pcreate(H5P_DATASET_CREATE);
  H5Pset_chunk(properties, static_cast<int>(chunk.size()), chunk.data());
  H5Pset_deflate(properties, 4);
  const hid_t links = H5Pcreate(H5P_LINK_CREATE);
  H5Pset_create_intermediate_group(links, 1);
  const hid_t dataset = H5Dcreate2(f, name.c_str(), type, space, links, properties, H5P_DEFAULT);
  ASSERT_GE(dataset, 0);
  ASSERT_GE(H5Dwrite(dataset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, values.data()), 0);
  if (um) {
    const hsize_t three = 3;
    const hid_t attribute_space = H5Screate_simple(1, &three, nullptr);
    const hid_t attribute = H5Acreate2(dataset, "element_size_um", H5T_NATIVE_DOUBLE,
                                       attribute_space, H5P_DEFAULT, H5P_DEFAULT);
    H5Awrite(attribute, H5T_NATIVE_DOUBLE, um->data());
    H5Aclose(attribute);
    H5Sclose(attribute_space);
  }
  H5Dclose(dataset);
  H5Pclose(links);
  H5Pclose(properties);
  H5Sclose(space);
  H5Fclose(f);
}

TEST_F(Formats, ReadsTheLargestVolumeOfAnHdf5File) {
  const auto file = dir_ / "scan.h5";
  const std::vector<float> values = floatsOf(voxels_);
  writeHdf5(file, "/exchange/data", values, H5T_NATIVE_FLOAT, true, 0,
            std::array{200.0, 100.0, 50.0});
  const std::vector<std::uint8_t> labels(voxels_.size(), 1);
  writeHdf5(file, "/exchange/labels", labels, H5T_NATIVE_UINT8, false);
  ASSERT_TRUE(isHdf5File(file));
  EXPECT_FALSE(isHdf5File(write("not.h5", "plain text, no signature")));
  const auto volumes = listHdf5Volumes(file);
  ASSERT_EQ(volumes.size(), 2U);
  EXPECT_EQ(volumes.front().path, "/exchange/data");  // labels last
  const Hdf5Source source(file);
  EXPECT_EQ(source.dims(), kDims);
  EXPECT_EQ(source.voxelSizeFrom(), "file");
  // element_size_um is z, y, x.
  EXPECT_DOUBLE_EQ(source.voxelSize()[0], 0.05);
  EXPECT_DOUBLE_EQ(source.voxelSize()[2], 0.2);
  EXPECT_TRUE(source.valueRange().has_value());
  expectValues(source, values);
  EXPECT_THROW(Hdf5Source(file, {.dataset = "/nowhere", .voxel_size = {}, .value_range = {}}),
               std::invalid_argument);

  const Json inspected = inspectScan(file);
  EXPECT_EQ(inspected.at("kind"), "hdf5");
  EXPECT_EQ(inspected.at("params").at("dataset"), "/exchange/data");
  EXPECT_FALSE(inspected.at("check"));
}

TEST_F(Formats, ReadsSignedHdf5VolumesWithALeadingAxisOfOne) {
  const auto file = dir_ / "scan.nxs";
  const std::vector<std::int16_t> values = signedOf(voxels_);
  writeHdf5(file, "/entry/data/data", values, H5T_NATIVE_INT16, true, 1);
  const Hdf5Source source(file);
  EXPECT_EQ(source.dims(), kDims);
  EXPECT_EQ(source.voxelSizeFrom(), "assumed");
  EXPECT_EQ(source.valueMapping(), (ValueMapping{-32768.0, 1.0}));
  // A region away from the origin, as a brick is read.
  const Box box{{3, 2, 5}, {20, 17, 11}};
  std::vector<std::uint16_t> grey(static_cast<std::size_t>(box.voxelCount()));
  source.readRegion(box, grey);
  std::size_t i = 0;
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
      for (std::int64_t x = box.min[0]; x < box.max[0]; ++x) {
        ASSERT_EQ(static_cast<int>(grey[i++]) - 32768,
                  values[static_cast<std::size_t>(x + kDims[0] * (y + kDims[1] * z))]);
      }
    }
  }
}

// --- Through the GUI ----------------------------------------------------------------------------

TEST_F(Formats, OpensNrrdAndPngScansInOneGo) {
  write("scans/part.nrrd",
        "NRRD0004\ntype: short\ndimension: 3\nsizes: 24 20 16\n"
        "spacings: 0.1 0.1 0.1\nendian: little\nencoding: raw\n"
        "space origin: (5,0,0)\n\n" +
            samplesOf(signedOf(voxels_)));
  Gui gui;
  const Json listing = gui.call("browse", {{"path", (dir_ / "scans").string()}});
  EXPECT_EQ(listing.at("entries").at(0).at("kind"), "nrrd");
  const Json inspected = gui.call("inspect_scan", {{"path", (dir_ / "scans/part.nrrd").string()}});
  EXPECT_FALSE(inspected.at("check"));
  const Json opened = gui.call("open_scan", {{"path", (dir_ / "scans/part.nrrd").string()},
                                             {"params", {{"brick_size", 8}}}});
  ASSERT_EQ(opened.at("run").at("status"), "done") << opened.dump();
  const Json& summary = opened.at("run").at("summary");
  EXPECT_EQ(summary.at("dims"), kDims);
  EXPECT_EQ(summary.at("format"), "nrrd");
  EXPECT_EQ(summary.at("value_mapping").at("offset"), -32768.0);

  writeHdf5(dir_ / "scans/beamline.h5", "/exchange/data", voxels_, H5T_NATIVE_UINT16, true);
  const Json h5 = gui.call("open_scan", {{"path", (dir_ / "scans/beamline.h5").string()},
                                         {"params", {{"brick_size", 8}, {"voxel_size_mm", 0.1}}}});
  ASSERT_EQ(h5.at("run").at("status"), "done") << h5.dump();
  EXPECT_EQ(h5.at("run").at("summary").at("dataset"), "/exchange/data");
  EXPECT_EQ(h5.at("run").at("summary").at("dims"), kDims);
}

}  // namespace
}  // namespace voxelsieve
