#include <gtest/gtest.h>
#include <tiffio.h>
#include <unistd.h>
#include <zip.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "detail/tiff.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/studio.hpp"
#include "voxelsieve/tiff.hpp"

namespace voxelsieve {
namespace {

// --- Writing TIFF files and ZIP archives for the tests, with libtiff and libzip --------------

struct TiffSpec {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  int bits = 16;
  int sample_format = SAMPLEFORMAT_UINT;
  int samples_per_pixel = 1;
  int compression = COMPRESSION_NONE;
  int predictor = PREDICTOR_NONE;
  bool big_endian = false;
  bool bigtiff = false;
  std::uint32_t rows_per_strip = 0;  // 0: one strip
  std::uint32_t tile = 0;            // > 0: square tiles instead of strips
  int resolution_unit = RESUNIT_INCH;
  double resolution = 0.0;  // pixels per unit, 0: no resolution tags
  std::string description;
};

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Samples of `pixels` (width x height, x fastest) inside the chunk at x0, y0 of size w x h, in
/// native byte order; float images hold the pixel values as floats.
std::vector<std::uint8_t> chunkSamples(const TiffSpec& spec,
                                       const std::vector<std::uint32_t>& pixels, std::uint32_t x0,
                                       std::uint32_t y0, std::uint32_t w, std::uint32_t h) {
  const std::size_t sample_bytes = static_cast<std::size_t>(spec.bits) / 8;
  const auto samples = static_cast<std::size_t>(spec.samples_per_pixel);
  std::vector<std::uint8_t> out;
  out.reserve(std::size_t{w} * h * samples * sample_bytes);
  for (std::uint32_t y = y0; y < y0 + h; ++y) {
    for (std::uint32_t x = x0; x < x0 + w; ++x) {
      const std::uint32_t value =
          x < spec.width && y < spec.height ? pixels[std::size_t{y} * spec.width + x] : 0;
      std::array<std::uint8_t, 8> bytes{};
      if (spec.sample_format == SAMPLEFORMAT_IEEEFP && sample_bytes == 4) {
        const auto f = static_cast<float>(value);
        std::memcpy(bytes.data(), &f, 4);
      } else if (spec.sample_format == SAMPLEFORMAT_IEEEFP) {
        const auto d = static_cast<double>(value);
        std::memcpy(bytes.data(), &d, 8);
      } else if (sample_bytes == 1) {
        bytes[0] = static_cast<std::uint8_t>(value);
      } else if (sample_bytes == 2) {
        const auto v = static_cast<std::uint16_t>(value);
        std::memcpy(bytes.data(), &v, 2);
      } else {
        std::memcpy(bytes.data(), &value, 4);
      }
      for (std::size_t s = 0; s < samples; ++s) {
        out.insert(out.end(), bytes.begin(),
                   bytes.begin() + static_cast<std::ptrdiff_t>(sample_bytes));
      }
    }
  }
  return out;
}

/// A TIFF file with one page per entry of `pages`, written by libtiff.
std::vector<std::uint8_t> writeTiff(const TiffSpec& spec,
                                    const std::vector<std::vector<std::uint32_t>>& pages) {
  const auto file = std::filesystem::temp_directory_path() /
                    ("voxelsieve_tiff_writer_" + std::to_string(::getpid()) + ".tif");
  std::string mode = "w";
  mode += spec.big_endian ? "b" : "l";
  if (spec.bigtiff) {
    mode += "8";
  }
  TIFF* tiff = TIFFOpen(file.c_str(), mode.c_str());
  EXPECT_NE(tiff, nullptr);
  if (tiff == nullptr) {
    return {};
  }
  for (const auto& pixels : pages) {
    TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, spec.width);
    TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, spec.height);
    TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, spec.bits);
    TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, spec.samples_per_pixel);
    TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, spec.sample_format);
    TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC,
                 spec.samples_per_pixel == 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK);
    TIFFSetField(tiff, TIFFTAG_COMPRESSION, spec.compression);
    if (spec.predictor != PREDICTOR_NONE) {
      TIFFSetField(tiff, TIFFTAG_PREDICTOR, spec.predictor);
    }
    if (!spec.description.empty()) {
      TIFFSetField(tiff, TIFFTAG_IMAGEDESCRIPTION, spec.description.c_str());
    }
    if (spec.resolution > 0.0) {
      TIFFSetField(tiff, TIFFTAG_XRESOLUTION, static_cast<float>(spec.resolution));
      TIFFSetField(tiff, TIFFTAG_YRESOLUTION, static_cast<float>(spec.resolution));
      TIFFSetField(tiff, TIFFTAG_RESOLUTIONUNIT, spec.resolution_unit);
    }
    if (spec.tile > 0) {
      TIFFSetField(tiff, TIFFTAG_TILEWIDTH, spec.tile);
      TIFFSetField(tiff, TIFFTAG_TILELENGTH, spec.tile);
      std::uint32_t index = 0;
      for (std::uint32_t y = 0; y < spec.height; y += spec.tile) {
        for (std::uint32_t x = 0; x < spec.width; x += spec.tile) {
          auto chunk = chunkSamples(spec, pixels, x, y, spec.tile, spec.tile);
          EXPECT_GE(TIFFWriteEncodedTile(tiff, index++, chunk.data(),
                                         static_cast<tmsize_t>(chunk.size())),
                    0);
        }
      }
    } else {
      const std::uint32_t rows = spec.rows_per_strip > 0 ? spec.rows_per_strip : spec.height;
      TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, rows);
      std::uint32_t index = 0;
      for (std::uint32_t y = 0; y < spec.height; y += rows) {
        auto chunk = chunkSamples(spec, pixels, 0, y, spec.width, std::min(rows, spec.height - y));
        EXPECT_GE(
            TIFFWriteEncodedStrip(tiff, index++, chunk.data(), static_cast<tmsize_t>(chunk.size())),
            0);
      }
    }
    EXPECT_NE(TIFFWriteDirectory(tiff), 0);
  }
  TIFFClose(tiff);
  auto bytes = readFile(file);
  std::filesystem::remove(file);
  return bytes;
}

void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary)
      .write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
}

/// A ZIP archive written by libzip; entries are deflated unless `stored`.
std::vector<std::uint8_t> writeZip(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries, bool stored) {
  const auto file = std::filesystem::temp_directory_path() /
                    ("voxelsieve_zip_writer_" + std::to_string(::getpid()) + ".zip");
  std::filesystem::remove(file);
  int error = 0;
  zip_t* archive = zip_open(file.c_str(), ZIP_CREATE | ZIP_EXCL, &error);
  EXPECT_NE(archive, nullptr);
  if (archive == nullptr) {
    return {};
  }
  for (const auto& [name, data] : entries) {
    zip_int64_t index = 0;
    if (name.ends_with('/')) {
      index = zip_dir_add(archive, name.c_str(), ZIP_FL_ENC_UTF_8);
    } else {
      // libzip reads the buffer when the archive is closed, so it must outlive this loop.
      zip_source_t* source = zip_source_buffer(archive, data.data(), data.size(), 0);
      index = zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8);
      zip_set_file_compression(archive, static_cast<zip_uint64_t>(index),
                               stored ? ZIP_CM_STORE : ZIP_CM_DEFLATE, 0);
    }
    EXPECT_GE(index, 0) << zip_strerror(archive);
  }
  EXPECT_EQ(zip_close(archive), 0);
  auto bytes = readFile(file);
  std::filesystem::remove(file);
  return bytes;
}

// --- Test data ----------------------------------------------------------------------------------

constexpr std::array<std::int64_t, 3> kDims{40, 24, 6};

/// Grey values with structure (runs for the encoders) and a spread over the whole 16-bit range.
std::uint32_t greyValue(std::int64_t x, std::int64_t y, std::int64_t z, int bits) {
  const std::uint64_t hash =
      static_cast<std::uint64_t>((x * 73856093) ^ (y * 19349663) ^ (z * 83492791)) % 997;
  const std::uint64_t value =
      x < 10 ? 1000 : static_cast<std::uint64_t>(x * 1531 + y * 211 + z * 4099) + hash;
  return static_cast<std::uint32_t>(value % (bits == 8 ? 256 : 65536));
}

std::vector<std::uint32_t> slicePixels(std::int64_t z, int bits) {
  std::vector<std::uint32_t> pixels;
  for (std::int64_t y = 0; y < kDims[1]; ++y) {
    for (std::int64_t x = 0; x < kDims[0]; ++x) {
      pixels.push_back(greyValue(x, y, z, bits));
    }
  }
  return pixels;
}

std::vector<std::uint16_t> readAll(const VolumeSource& source) {
  const auto dims = source.dims();
  std::vector<std::uint16_t> values(static_cast<std::size_t>(dims[0] * dims[1] * dims[2]));
  source.readRegion(Box{{0, 0, 0}, dims}, values);
  return values;
}

void expectGreyValues(const VolumeSource& source, int bits) {
  ASSERT_EQ(source.dims(), kDims);
  const auto values = readAll(source);
  std::size_t mismatches = 0;
  std::size_t i = 0;
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    for (std::int64_t y = 0; y < kDims[1]; ++y) {
      for (std::int64_t x = 0; x < kDims[0]; ++x, ++i) {
        mismatches += values[i] != greyValue(x, y, z, bits) ? 1 : 0;
      }
    }
  }
  EXPECT_EQ(mismatches, 0U);
}

class TiffTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_tiff_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  /// Writes the test volume as one file per slice (slice1.tif ... slice6.tif) below `folder`.
  std::filesystem::path writeStack(const TiffSpec& base, const std::string& folder = "slices") {
    TiffSpec spec = base;
    spec.width = static_cast<std::uint32_t>(kDims[0]);
    spec.height = static_cast<std::uint32_t>(kDims[1]);
    for (std::int64_t z = 0; z < kDims[2]; ++z) {
      writeFile(dir_ / folder / ("slice" + std::to_string(z + 1) + ".tif"),
                writeTiff(spec, {slicePixels(z, spec.bits)}));
    }
    return dir_ / folder;
  }

  std::filesystem::path dir_;
};

// --- Tests ----------------------------------------------------------------------------------

TEST(Tiff, NaturalOrderComparesNumbersByValue) {
  EXPECT_TRUE(naturalLess("slice2.tif", "slice10.tif"));
  EXPECT_FALSE(naturalLess("slice10.tif", "slice2.tif"));
  EXPECT_TRUE(naturalLess("slice009.tif", "slice10.tif"));
  EXPECT_TRUE(naturalLess("a.tif", "b.tif"));
  EXPECT_FALSE(naturalLess("slice1.tif", "slice1.tif"));
  std::vector<std::string> names = {"s10", "s9", "s100", "s1"};
  std::ranges::sort(names, naturalLess);
  EXPECT_EQ(names, (std::vector<std::string>{"s1", "s9", "s10", "s100"}));
}

TEST_F(TiffTest, ReadsAllEncodings) {
  struct Case {
    std::string name;
    TiffSpec spec;
  };
  std::vector<Case> cases;
  const auto add = [&](std::string name, auto change) {
    TiffSpec spec;
    change(spec);
    cases.push_back({std::move(name), spec});
  };
  add("plain", [](TiffSpec&) {});
  add("big_endian", [](TiffSpec& s) { s.big_endian = true; });
  add("eight_bit", [](TiffSpec& s) { s.bits = 8; });
  add("thirty_two_bit", [](TiffSpec& s) { s.bits = 32; });
  add("strips", [](TiffSpec& s) { s.rows_per_strip = 5; });
  add("tiles", [](TiffSpec& s) { s.tile = 16; });
  add("lzw", [](TiffSpec& s) { s.compression = 5; });
  add("lzw_predictor", [](TiffSpec& s) {
    s.compression = 5;
    s.predictor = 2;
    s.rows_per_strip = 7;
  });
  add("deflate_predictor_big_endian", [](TiffSpec& s) {
    s.compression = 8;
    s.predictor = 2;
    s.big_endian = true;
  });
  add("deflate_tiles", [](TiffSpec& s) {
    s.compression = 8;
    s.tile = 16;
  });
  add("packbits", [](TiffSpec& s) { s.compression = 32773; });
  add("packbits_eight_bit", [](TiffSpec& s) {
    s.compression = 32773;
    s.bits = 8;
  });
  add("bigtiff", [](TiffSpec& s) {
    s.bigtiff = true;
    s.tile = 16;
    s.compression = 5;
  });
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    const auto folder = writeStack(c.spec, c.name);
    const TiffStackSource source(folder);
    EXPECT_EQ(source.bitsPerSample(), c.spec.bits);
    expectGreyValues(source, c.spec.bits);
  }
}

TEST(Tiff, DecodesFloatSlices) {
  // Float input (ADR 0015): 32 and 64 bit, with the floating-point predictor, in strips and tiles.
  struct Case {
    std::string name;
    int bits;
    int compression;
    int predictor;
    std::uint32_t tile;
  };
  const std::vector<Case> cases = {
      {"float32", 32, COMPRESSION_NONE, PREDICTOR_NONE, 0},
      {"float32_deflate_predictor", 32, COMPRESSION_ADOBE_DEFLATE, PREDICTOR_FLOATINGPOINT, 0},
      {"float64_lzw_predictor_tiles", 64, COMPRESSION_LZW, PREDICTOR_FLOATINGPOINT, 16},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    TiffSpec spec;
    spec.width = static_cast<std::uint32_t>(kDims[0]);
    spec.height = static_cast<std::uint32_t>(kDims[1]);
    spec.bits = c.bits;
    spec.sample_format = SAMPLEFORMAT_IEEEFP;
    spec.compression = c.compression;
    spec.predictor = c.predictor;
    spec.tile = c.tile;
    // Big endian only without the predictor: libtiff 4.5 writes that combination wrongly.
    spec.big_endian = c.predictor == PREDICTOR_NONE;
    const detail::MemoryBytes bytes(writeTiff(spec, {slicePixels(0, 16)}));
    const detail::TiffPage page = detail::readTiffPages(bytes).front();
    EXPECT_EQ(page.sample_format, SAMPLEFORMAT_IEEEFP);
    EXPECT_ANY_THROW(detail::checkSupported(page));  // the uint16 pipeline does not take floats
    EXPECT_ANY_THROW((void)detail::decodeTiffChunk(bytes, page, 0));
    std::size_t mismatches = 0;
    for (std::uint32_t chunk = 0; chunk < page.chunk_count; ++chunk) {
      const auto values = detail::decodeTiffFloatChunk(bytes, page, chunk);
      const std::uint32_t x0 = (chunk % page.chunks_across) * page.chunk_width;
      const std::uint32_t y0 = (chunk / page.chunks_across) * page.chunk_height;
      for (std::uint32_t y = y0; y < std::min(y0 + page.chunk_height, page.height); ++y) {
        for (std::uint32_t x = x0; x < std::min(x0 + page.chunk_width, page.width); ++x) {
          const double value = values[std::size_t{y - y0} * page.chunk_width + (x - x0)];
          mismatches += value != greyValue(x, y, 0, 16) ? 1 : 0;
        }
      }
    }
    EXPECT_EQ(mismatches, 0U);
  }
}

TEST_F(TiffTest, ReadsRegionsAcrossChunks) {
  TiffSpec spec;
  spec.tile = 16;
  spec.compression = 8;
  const TiffStackSource source(writeStack(spec));
  const Box box{{13, 5, 2}, {35, 20, 5}};
  std::vector<std::uint16_t> values(std::size_t{22} * 15 * 3);
  source.readRegion(box, values);
  std::size_t i = 0;
  for (std::int64_t z = 2; z < 5; ++z) {
    for (std::int64_t y = 5; y < 20; ++y) {
      for (std::int64_t x = 13; x < 35; ++x, ++i) {
        ASSERT_EQ(values[i], greyValue(x, y, z, 16)) << x << "," << y << "," << z;
      }
    }
  }
  EXPECT_THROW(source.readRegion(Box{{0, 0, 0}, {41, 1, 1}}, values), std::out_of_range);
}

TEST_F(TiffTest, SortsSlicesNaturally) {
  // slice10 must follow slice9, not slice1.
  TiffSpec spec;
  spec.width = 4;
  spec.height = 2;
  for (std::uint32_t z = 1; z <= 11; ++z) {
    writeFile(dir_ / "stack" / ("slice" + std::to_string(z) + ".tif"),
              writeTiff(spec, {std::vector<std::uint32_t>(8, z)}));
  }
  const TiffStackSource source(dir_ / "stack");
  ASSERT_EQ(source.dims(), (std::array<std::int64_t, 3>{4, 2, 11}));
  const auto values = readAll(source);
  for (std::size_t z = 0; z < 11; ++z) {
    EXPECT_EQ(values[z * 8], z + 1);
  }
}

TEST_F(TiffTest, ReadsMultiPageFiles) {
  TiffSpec spec;
  spec.width = static_cast<std::uint32_t>(kDims[0]);
  spec.height = static_cast<std::uint32_t>(kDims[1]);
  spec.compression = 5;
  std::vector<std::vector<std::uint32_t>> pages;
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    pages.push_back(slicePixels(z, 16));
  }
  writeFile(dir_ / "volume.tiff", writeTiff(spec, pages));
  const TiffStackSource source(dir_ / "volume.tiff");
  expectGreyValues(source, 16);
}

TEST_F(TiffTest, ReadsVoxelSize) {
  TiffSpec centimetre;
  centimetre.resolution_unit = 3;
  centimetre.resolution = 50.0;  // 50 pixels per cm
  EXPECT_NEAR(TiffStackSource(writeStack(centimetre, "cm")).voxelSize()[0], 0.2, 1e-9);

  TiffSpec image_j;
  image_j.resolution_unit = 1;
  image_j.resolution = 20.0;  // 20 pixels per micron
  image_j.description = "ImageJ=1.54f\nimages=6\nunit=micron\n";
  const VoxelSize from_image_j =
      TiffStackSource(writeStack(image_j, "imagej")).fileVoxelSize().value_or(VoxelSize(-1.0));
  EXPECT_NEAR(from_image_j[0], 0.00005, 1e-12);
  EXPECT_NEAR(from_image_j[2], 0.00005, 1e-12);  // no spacing given: cubic

  // ImageJ stacks with a slice spacing: voxels that are not cubes.
  TiffSpec spaced = image_j;
  spaced.resolution = 3.0;  // 3 pixels per mm
  spaced.description = "ImageJ=1.54f\nimages=6\nslices=6\nunit=mm\nspacing=0.6\n";
  const VoxelSize from_spacing =
      TiffStackSource(writeStack(spaced, "spacing")).fileVoxelSize().value_or(VoxelSize(-1.0));
  EXPECT_NEAR(from_spacing[0], 1.0 / 3.0, 1e-9);
  EXPECT_NEAR(from_spacing[1], 1.0 / 3.0, 1e-9);
  EXPECT_NEAR(from_spacing[2], 0.6, 1e-12);

  TiffSpec inch;  // print resolution, not a pixel size
  inch.resolution = 300.0;
  const TiffStackSource unknown(writeStack(inch, "inch"));
  EXPECT_FALSE(unknown.fileVoxelSize().has_value());
  EXPECT_EQ(unknown.voxelSize(), VoxelSize(1.0));
  TiffStackOptions options;
  options.voxel_size = 0.05;
  EXPECT_EQ(TiffStackSource(dir_ / "inch", options).voxelSize(), 0.05);
}

TEST_F(TiffTest, RejectsWhatCannotBeKeptExactly) {
  TiffSpec floating;
  floating.bits = 32;
  floating.sample_format = 3;
  EXPECT_ANY_THROW(readAll(TiffStackSource(writeStack(floating, "float"))));

  TiffSpec colour;
  colour.samples_per_pixel = 3;
  colour.bits = 8;
  EXPECT_ANY_THROW(readAll(TiffStackSource(writeStack(colour, "rgb"))));

  // 32-bit labels are fine up to 65535; larger values would be clipped.
  TiffSpec wide;
  wide.bits = 32;
  wide.width = 4;
  wide.height = 1;
  writeFile(dir_ / "wide" / "a.tif", writeTiff(wide, {{1, 2, 70000, 4}}));
  EXPECT_ANY_THROW(readAll(TiffStackSource(dir_ / "wide")));

  // All slices must have the size of the first.
  TiffSpec small;
  small.width = 4;
  small.height = 4;
  writeFile(dir_ / "mixed" / "a1.tif", writeTiff(small, {std::vector<std::uint32_t>(16, 1)}));
  small.width = 5;
  writeFile(dir_ / "mixed" / "a2.tif", writeTiff(small, {std::vector<std::uint32_t>(20, 1)}));
  EXPECT_ANY_THROW(readAll(TiffStackSource(dir_ / "mixed")));

  EXPECT_ANY_THROW(TiffStackSource(dir_ / "missing"));
  std::filesystem::create_directories(dir_ / "empty");
  EXPECT_ANY_THROW(TiffStackSource(dir_ / "empty"));
}

TEST_F(TiffTest, ReadsZipArchivesAndPrefersGreyValuesOverLabels) {
  TiffSpec spec;
  spec.width = static_cast<std::uint32_t>(kDims[0]);
  spec.height = static_cast<std::uint32_t>(kDims[1]);
  TiffSpec labels = spec;
  labels.bits = 32;
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries;
  entries.emplace_back("volume/", std::vector<std::uint8_t>{});
  // More label slices than grey slices: the grey values must still win.
  for (std::int64_t z = 0; z < kDims[2] + 2; ++z) {
    const std::vector<std::uint32_t> label(static_cast<std::size_t>(kDims[0] * kDims[1]),
                                           static_cast<std::uint32_t>(z % 3));
    entries.emplace_back("volume/target/slice" + std::to_string(z) + ".tif",
                         writeTiff(labels, {label}));
  }
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    entries.emplace_back("volume/data/slice" + std::to_string(z) + ".tif",
                         writeTiff(spec, {slicePixels(z, 16)}));
  }
  entries.emplace_back("volume/readme.txt", std::vector<std::uint8_t>{'h', 'i'});
  for (const bool stored : {true, false}) {
    SCOPED_TRACE(stored ? "stored" : "deflate");
    const auto zip = dir_ / (stored ? "stored.zip" : "deflate.zip");
    writeFile(zip, writeZip(entries, stored));
    const TiffStackSource source(zip);
    EXPECT_EQ(source.folder(), "volume/data");
    EXPECT_EQ(source.otherFolders(), std::vector<std::string>{"volume/target"});
    expectGreyValues(source, 16);

    TiffStackOptions options;
    options.folder = "target";
    const TiffStackSource target(zip, options);
    EXPECT_EQ(target.dims()[2], kDims[2] + 2);
    EXPECT_EQ(target.bitsPerSample(), 32);
    const auto values = readAll(target);
    EXPECT_EQ(values.back(), (kDims[2] + 1) % 3);

    options.folder = "nothing";
    EXPECT_ANY_THROW(TiffStackSource(zip, options));
  }
  // A damaged archive is detected by its checksum.
  auto bytes = writeZip(entries, true);
  const std::string name = "volume/data/slice0.tif";
  const auto at = std::search(bytes.begin(), bytes.end(), name.begin(), name.end());
  ASSERT_NE(at, bytes.end());
  *(at + static_cast<std::ptrdiff_t>(name.size()) + 500) ^= 0xFFU;  // a pixel of the first slice
  writeFile(dir_ / "broken.zip", bytes);
  EXPECT_ANY_THROW(readAll(TiffStackSource(dir_ / "broken.zip")));
}

TEST_F(TiffTest, PhantomStackGivesTheSameDatasetAsTheRawVolume) {
  PhantomSpec phantom = defaultPhantomSpec();
  phantom.dims = {48, 40, 36};
  phantom.outer_size_mm = {3.0, 2.6, 2.4};
  phantom.wall_thickness_mm = 0.6;
  phantom.pores = {{{0.0, 0.0, 1.0}, 0.15}};
  phantom.noise_sigma = 200.0;
  const Volume16 volume = generatePhantom(phantom);
  TiffSpec spec;
  spec.width = static_cast<std::uint32_t>(phantom.dims[0]);
  spec.height = static_cast<std::uint32_t>(phantom.dims[1]);
  spec.compression = 8;
  spec.predictor = 2;
  spec.resolution_unit = 3;
  spec.resolution = 10.0 / phantom.voxel_size[0];
  const auto slice = static_cast<std::size_t>(phantom.dims[0] * phantom.dims[1]);
  for (std::int64_t z = 0; z < phantom.dims[2]; ++z) {
    std::vector<std::uint32_t> pixels(slice);
    for (std::size_t i = 0; i < slice; ++i) {
      pixels[i] = volume.data[static_cast<std::size_t>(z) * slice + i];
    }
    writeFile(dir_ / "phantom" / ("p" + std::to_string(z) + ".tif"), writeTiff(spec, {pixels}));
  }
  const TiffStackSource tiff(dir_ / "phantom");
  EXPECT_NEAR(tiff.voxelSize()[0], phantom.voxel_size[0], 1e-9);
  EXPECT_EQ(readAll(tiff), volume.data);

  DatasetOptions options;
  options.brick_size = 16;
  const DatasetInfo from_tiff = writeDataset(tiff, dir_ / "tiff.vsieve", options);
  const DatasetInfo from_phantom =
      writeDataset(PhantomSource(phantom), dir_ / "phantom.vsieve", options);
  EXPECT_EQ(from_tiff.threshold, from_phantom.threshold);
  EXPECT_EQ(from_tiff.active_voxel_count, from_phantom.active_voxel_count);
}

TEST_F(TiffTest, StudioBrowsesAndImportsTiffStacks) {
  PhantomSpec phantom = defaultPhantomSpec();
  phantom.dims = {40, 40, 32};
  phantom.outer_size_mm = {2.6, 2.6, 2.2};
  phantom.wall_thickness_mm = 0.6;
  phantom.pores = {};
  const Volume16 volume = generatePhantom(phantom);
  TiffSpec spec;
  spec.width = 40;
  spec.height = 40;
  spec.compression = 5;
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries;
  for (std::int64_t z = 0; z < 32; ++z) {
    const auto begin = volume.data.begin() + z * 1600;
    entries.emplace_back("scan/data/slice" + std::to_string(z) + ".tif",
                         writeTiff(spec, {std::vector<std::uint32_t>(begin, begin + 1600)}));
    entries.emplace_back("scan/labels/slice" + std::to_string(z) + ".tif",
                         writeTiff(spec, {std::vector<std::uint32_t>(1600, 1)}));
  }
  writeFile(dir_ / "input" / "scan.zip", writeZip(entries, false));
  writeFile(dir_ / "input" / "slices" / "a.tif", entries.front().second);
  writeFile(dir_ / "input" / "notes.txt", {'x'});

  Studio studio({});
  const auto listing = studio.call("browse", {{"path", (dir_ / "input").string()}});
  std::map<std::string, std::string> kinds;
  for (const auto& entry : listing.at("entries")) {
    kinds[entry.at("name")] = entry.at("kind");
  }
  EXPECT_EQ(kinds["scan.zip"], "tiff");
  EXPECT_EQ(kinds["slices"], "tiff");
  EXPECT_EQ(kinds["notes.txt"], "file");

  studio.call("project_create", {{"path", (dir_ / "project").string()}});
  const auto imported =
      studio.call("run_import_tiff", {{"path", (dir_ / "input" / "scan.zip").string()},
                                      {"voxel_size_mm", 0.1},
                                      {"brick_size", 16}});
  EXPECT_EQ(imported.at("status"), "done");
  EXPECT_EQ(imported.at("summary").at("slices"), 32);
  EXPECT_EQ(imported.at("summary").at("bits_per_sample"), 16);
  const std::string log = imported.at("messages").dump();
  EXPECT_NE(log.find("scan/data"), std::string::npos) << log;
  EXPECT_NE(log.find("scan/labels"), std::string::npos) << log;
  EXPECT_EQ(studio.call("dataset_info", {}).at("step"), imported.at("id"));
}

}  // namespace
}  // namespace voxelsieve
