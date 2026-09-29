#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <boost/crc.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
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

// --- Writing TIFF files for the tests --------------------------------------------------------

struct TiffSpec {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  int bits = 16;
  int sample_format = 1;
  int samples_per_pixel = 1;
  int compression = 1;  // 1 none, 5 LZW, 8 deflate, 32773 PackBits
  int predictor = 1;
  bool big_endian = false;
  bool bigtiff = false;
  std::uint32_t rows_per_strip = 0;  // 0: one strip
  std::uint32_t tile = 0;            // > 0: square tiles instead of strips
  int resolution_unit = 2;
  double resolution = 0.0;  // pixels per unit, 0: no resolution tags
  std::string description;
};

class Bytes {
 public:
  explicit Bytes(bool big_endian) : big_endian_(big_endian) {}
  void put(std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
      const std::size_t shift = 8 * (big_endian_ ? size - 1 - i : i);
      data_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
  }
  void append(const std::vector<std::uint8_t>& bytes) {
    data_.insert(data_.end(), bytes.begin(), bytes.end());
  }
  void patch(std::size_t at, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
      const std::size_t shift = 8 * (big_endian_ ? size - 1 - i : i);
      data_[at + i] = static_cast<std::uint8_t>((value >> shift) & 0xFFU);
    }
  }
  void align() {
    if (data_.size() % 2 != 0) {
      data_.push_back(0);
    }
  }
  [[nodiscard]] std::size_t size() const { return data_.size(); }
  [[nodiscard]] const std::vector<std::uint8_t>& data() const { return data_; }

 private:
  bool big_endian_;
  std::vector<std::uint8_t> data_;
};

/// TIFF LZW as libtiff writes it: MSB first, code width grows one code early, clear when full.
std::vector<std::uint8_t> lzwEncode(const std::vector<std::uint8_t>& in) {
  std::vector<std::uint8_t> out;
  std::uint64_t buffer = 0;
  int buffered = 0;
  int width = 9;
  const auto emit = [&](std::uint32_t code) {
    buffer = (buffer << static_cast<unsigned>(width)) | code;
    buffered += width;
    while (buffered >= 8) {
      buffered -= 8;
      out.push_back(static_cast<std::uint8_t>((buffer >> static_cast<unsigned>(buffered)) & 0xFFU));
    }
  };
  // Words as strings: GCC 13 at -O3 warns falsely on comparing vectors of bytes
  // (stringop-overread).
  std::map<std::string, std::uint32_t> table;
  std::uint32_t next = 258;
  emit(256);
  std::string word;
  const auto code = [&](const std::string& w) {
    return w.size() == 1 ? std::uint32_t{static_cast<std::uint8_t>(w[0])} : table.at(w);
  };
  for (const std::uint8_t c : in) {
    std::string longer = word;
    longer.push_back(static_cast<char>(c));
    if (word.empty() || longer.size() == 1 || table.contains(longer)) {
      word = std::move(longer);
      continue;
    }
    emit(code(word));
    table[longer] = next++;
    if (next == 4094) {
      emit(256);
      table.clear();
      next = 258;
      width = 9;
    } else if (next > (1U << static_cast<unsigned>(width)) - 1) {
      ++width;
    }
    word = std::string(1, static_cast<char>(c));
  }
  if (!word.empty()) {
    emit(code(word));
  }
  emit(257);
  if (buffered > 0) {
    out.push_back(
        static_cast<std::uint8_t>((buffer << static_cast<unsigned>(8 - buffered)) & 0xFFU));
  }
  return out;
}

std::vector<std::uint8_t> packBitsEncode(const std::vector<std::uint8_t>& in) {
  std::vector<std::uint8_t> out;
  std::size_t i = 0;
  while (i < in.size()) {
    std::size_t run = 1;
    while (i + run < in.size() && run < 128 && in[i + run] == in[i]) {
      ++run;
    }
    if (run >= 3) {
      out.push_back(static_cast<std::uint8_t>(257 - run));  // -(run - 1)
      out.push_back(in[i]);
      i += run;
      continue;
    }
    std::size_t literal = 0;
    while (i + literal < in.size() && literal < 128 &&
           (i + literal + 2 >= in.size() || in[i + literal] != in[i + literal + 1] ||
            in[i + literal] != in[i + literal + 2])) {
      ++literal;
    }
    literal = std::max<std::size_t>(literal, 1);
    out.push_back(static_cast<std::uint8_t>(literal - 1));
    out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(i),
               in.begin() + static_cast<std::ptrdiff_t>(i + literal));
    i += literal;
  }
  return out;
}

/// zlib stream, or raw deflate as in ZIP archives.
std::vector<std::uint8_t> deflateEncode(const std::vector<std::uint8_t>& in, bool raw = false) {
  namespace io = boost::iostreams;
  std::string out;
  {
    io::zlib_params params;
    params.noheader = raw;
    io::filtering_ostream stream;
    stream.push(io::zlib_compressor(params));
    stream.push(io::back_inserter(out));
    stream.write(reinterpret_cast<const char*>(in.data()), static_cast<std::streamsize>(in.size()));
  }
  return {out.begin(), out.end()};
}

/// One strip or tile of `pixels` (width x height, x fastest) in the file's layout and encoding.
/// Float samples are given as their bit patterns.
std::vector<std::uint8_t> encodeChunk(const TiffSpec& spec,
                                      const std::vector<std::uint64_t>& pixels, std::uint32_t x0,
                                      std::uint32_t y0, std::uint32_t w, std::uint32_t h) {
  const std::size_t sample_bytes = static_cast<std::size_t>(spec.bits) / 8;
  const auto samples = static_cast<std::uint32_t>(spec.samples_per_pixel);
  const auto pixel = [&](std::uint32_t x, std::uint32_t y) -> std::uint64_t {
    return x < spec.width && y < spec.height ? pixels[std::size_t{y} * spec.width + x] : 0;
  };
  Bytes bytes(spec.big_endian);
  for (std::uint32_t y = y0; y < y0 + h; ++y) {
    if (spec.predictor == 3) {
      // Floating-point predictor: byte planes, most significant first, differenced along the row.
      std::vector<std::uint8_t> row(w * sample_bytes);
      for (std::uint32_t x = 0; x < w; ++x) {
        for (std::size_t b = 0; b < sample_bytes; ++b) {
          row[b * w + x] =
              static_cast<std::uint8_t>((pixel(x0 + x, y) >> (8 * (sample_bytes - 1 - b))) & 0xFFU);
        }
      }
      for (std::size_t i = row.size() - 1; i > 0; --i) {
        row[i] = static_cast<std::uint8_t>(row[i] - row[i - 1]);
      }
      bytes.append(row);
      continue;
    }
    std::uint64_t previous = 0;
    for (std::uint32_t x = x0; x < x0 + w; ++x) {
      for (std::uint32_t s = 0; s < samples; ++s) {
        const std::uint64_t value = pixel(x, y);
        const std::uint64_t mask = sample_bytes == 8 ? ~0ULL : (1ULL << (8 * sample_bytes)) - 1;
        bytes.put(spec.predictor == 2 ? (value - previous) & mask : value, sample_bytes);
        if (s + 1 == samples) {
          previous = value;
        }
      }
    }
  }
  switch (spec.compression) {
    case 5:
      return lzwEncode(bytes.data());
    case 8:
      return deflateEncode(bytes.data());
    case 32773:
      return packBitsEncode(bytes.data());
    default:
      return bytes.data();
  }
}

struct Field {
  std::uint16_t tag;
  std::uint16_t type;  // 2 ascii, 3 short, 4 long, 5 rational, 16 long8
  std::vector<std::uint64_t> values;
  std::string text;
};

std::size_t typeSize(std::uint16_t type) {
  switch (type) {
    case 2:
      return 1;
    case 3:
      return 2;
    case 4:
      return 4;
    default:
      return 8;
  }
}

/// A TIFF file with one page per entry of `pages`.
std::vector<std::uint8_t> writeTiff(const TiffSpec& spec,
                                    const std::vector<std::vector<std::uint64_t>>& pages) {
  Bytes out(spec.big_endian);
  out.put(spec.big_endian ? 0x4D4D : 0x4949, 2);
  const std::size_t offset_size = spec.bigtiff ? 8 : 4;
  std::size_t next_ifd_at = 0;
  if (spec.bigtiff) {
    out.put(43, 2);
    out.put(8, 2);
    out.put(0, 2);
  } else {
    out.put(42, 2);
  }
  next_ifd_at = out.size();
  out.put(0, offset_size);
  const std::uint16_t offset_type = spec.bigtiff ? 16 : 4;
  for (const auto& pixels : pages) {
    const std::uint32_t chunk_w = spec.tile > 0 ? spec.tile : spec.width;
    const std::uint32_t chunk_h =
        spec.tile > 0 ? spec.tile : (spec.rows_per_strip > 0 ? spec.rows_per_strip : spec.height);
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint64_t> counts;
    for (std::uint32_t y = 0; y < spec.height; y += chunk_h) {
      for (std::uint32_t x = 0; x < spec.width; x += chunk_w) {
        // The last strip holds only the rows that are left; tiles are always whole.
        const std::uint32_t h = spec.tile > 0 ? chunk_h : std::min(chunk_h, spec.height - y);
        const auto chunk = encodeChunk(spec, pixels, x, y, chunk_w, h);
        offsets.push_back(out.size());
        counts.push_back(chunk.size());
        out.append(chunk);
        out.align();
      }
    }
    std::vector<Field> fields = {
        {256, 4, {spec.width}, {}},
        {257, 4, {spec.height}, {}},
        {258,
         3,
         std::vector<std::uint64_t>(static_cast<std::size_t>(spec.samples_per_pixel),
                                    static_cast<std::uint64_t>(spec.bits)),
         {}},
        {259, 3, {static_cast<std::uint64_t>(spec.compression)}, {}},
        {262, 3, {spec.samples_per_pixel == 3 ? 2U : 1U}, {}},
        {277, 3, {static_cast<std::uint64_t>(spec.samples_per_pixel)}, {}},
        {339, 3, {static_cast<std::uint64_t>(spec.sample_format)}, {}},
    };
    if (!spec.description.empty()) {
      fields.push_back({270, 2, {}, spec.description});
    }
    if (spec.tile > 0) {
      fields.push_back({322, 3, {spec.tile}, {}});
      fields.push_back({323, 3, {spec.tile}, {}});
      fields.push_back({324, offset_type, offsets, {}});
      fields.push_back({325, offset_type, counts, {}});
    } else {
      fields.push_back({273, offset_type, offsets, {}});
      fields.push_back({278, 4, {chunk_h}, {}});
      fields.push_back({279, offset_type, counts, {}});
    }
    if (spec.predictor != 1) {
      fields.push_back({317, 3, {static_cast<std::uint64_t>(spec.predictor)}, {}});
    }
    if (spec.resolution > 0.0) {
      const auto numerator = static_cast<std::uint64_t>(spec.resolution * 1000.0);
      fields.push_back({282, 5, {numerator, 1000}, {}});
      fields.push_back({283, 5, {numerator, 1000}, {}});
      fields.push_back({296, 3, {static_cast<std::uint64_t>(spec.resolution_unit)}, {}});
    }
    std::ranges::sort(fields, {}, &Field::tag);
    // Values too large for the entry go before the directory.
    std::vector<std::uint64_t> value_offsets;
    for (const Field& field : fields) {
      const std::size_t count = field.type == 2   ? field.text.size() + 1
                                : field.type == 5 ? field.values.size() / 2
                                                  : field.values.size();
      if (count * typeSize(field.type) <= offset_size) {
        value_offsets.push_back(0);
        continue;
      }
      value_offsets.push_back(out.size());
      if (field.type == 2) {
        for (const char c : field.text) {
          out.put(static_cast<std::uint8_t>(c), 1);
        }
        out.put(0, 1);
      } else {
        for (const std::uint64_t v : field.values) {
          out.put(v, field.type == 5 ? 4 : typeSize(field.type));
        }
      }
      out.align();
    }
    out.patch(next_ifd_at, out.size(), offset_size);
    out.put(fields.size(), spec.bigtiff ? 8 : 2);
    for (std::size_t i = 0; i < fields.size(); ++i) {
      const Field& field = fields[i];
      const std::size_t count = field.type == 2   ? field.text.size() + 1
                                : field.type == 5 ? field.values.size() / 2
                                                  : field.values.size();
      out.put(field.tag, 2);
      out.put(field.type, 2);
      out.put(count, offset_size);
      if (value_offsets[i] != 0) {
        out.put(value_offsets[i], offset_size);
      } else {
        std::size_t written = 0;
        if (field.type == 2) {
          for (const char c : field.text) {
            out.put(static_cast<std::uint8_t>(c), 1);
          }
          out.put(0, 1);
          written = field.text.size() + 1;
        } else {
          for (const std::uint64_t v : field.values) {
            out.put(v, typeSize(field.type));
            written += typeSize(field.type);
          }
        }
        out.put(0, offset_size - written);
      }
    }
    next_ifd_at = out.size();
    out.put(0, offset_size);
  }
  return out.data();
}

std::vector<std::uint8_t> writeTiff(const TiffSpec& spec,
                                    const std::vector<std::vector<std::uint32_t>>& pages) {
  std::vector<std::vector<std::uint64_t>> wide;
  wide.reserve(pages.size());
  for (const auto& page : pages) {
    wide.emplace_back(page.begin(), page.end());
  }
  return writeTiff(spec, wide);
}

void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary)
      .write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
}

/// A ZIP archive; entries are deflated unless `stored`.
std::vector<std::uint8_t> writeZip(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries, bool stored) {
  Bytes out(false);
  Bytes directory(false);
  for (const auto& [name, data] : entries) {
    boost::crc_32_type crc;
    crc.process_bytes(data.data(), data.size());
    const std::vector<std::uint8_t> payload = stored ? data : deflateEncode(data, true);
    const std::uint16_t method = stored ? 0 : 8;
    const std::size_t local_offset = out.size();
    out.put(0x04034b50, 4);
    out.put(20, 2);
    out.put(0, 2);
    out.put(method, 2);
    out.put(0, 4);  // time and date
    out.put(crc.checksum(), 4);
    out.put(payload.size(), 4);
    out.put(data.size(), 4);
    out.put(name.size(), 2);
    out.put(0, 2);
    for (const char c : name) {
      out.put(static_cast<std::uint8_t>(c), 1);
    }
    out.append(payload);

    directory.put(0x02014b50, 4);
    directory.put(20, 2);
    directory.put(20, 2);
    directory.put(0, 2);
    directory.put(method, 2);
    directory.put(0, 4);
    directory.put(crc.checksum(), 4);
    directory.put(payload.size(), 4);
    directory.put(data.size(), 4);
    directory.put(name.size(), 2);
    directory.put(0, 2);  // extra
    directory.put(0, 2);  // comment
    directory.put(0, 2);  // disk
    directory.put(0, 2);  // internal attributes
    directory.put(0, 4);  // external attributes
    directory.put(local_offset, 4);
    for (const char c : name) {
      directory.put(static_cast<std::uint8_t>(c), 1);
    }
  }
  const std::size_t directory_offset = out.size();
  out.append(directory.data());
  out.put(0x06054b50, 4);
  out.put(0, 2);
  out.put(0, 2);
  out.put(entries.size(), 2);
  out.put(entries.size(), 2);
  out.put(directory.size(), 4);
  out.put(directory_offset, 4);
  out.put(0, 2);
  return out.data();
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

TEST(Tiff, LzwAndPackBitsRoundTrip) {
  std::vector<std::uint8_t> data;
  data.reserve(30500);
  for (int i = 0; i < 30000; ++i) {
    data.push_back(static_cast<std::uint8_t>(i % 7 == 0 ? 42 : (i * 31 + i / 100) % 251));
  }
  data.insert(data.end(), 500, 9);  // long runs
  EXPECT_EQ(detail::lzwDecode(lzwEncode(data), data.size()), data);
  EXPECT_EQ(detail::packBitsDecode(packBitsEncode(data), data.size()), data);
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
  TiffSpec half;  // 16-bit float
  half.bits = 16;
  half.sample_format = 3;
  EXPECT_ANY_THROW(readAll(TiffStackSource(writeStack(half, "half"))));

  TiffSpec is_signed;
  is_signed.sample_format = 2;
  EXPECT_ANY_THROW(readAll(TiffStackSource(writeStack(is_signed, "signed"))));

  // A value range is only for float slices.
  TiffStackOptions ranged;
  ranged.value_range = std::array<double, 2>{0.0, 1.0};
  EXPECT_ANY_THROW(TiffStackSource(writeStack({}, "integer"), ranged));

  TiffSpec colour;
  colour.samples_per_pixel = 3;
  colour.bits = 8;
  EXPECT_ANY_THROW(readAll(TiffStackSource(writeStack(colour, "rgb"))));

  // 32-bit labels are fine up to 65535; larger values would be clipped.
  TiffSpec wide;
  wide.bits = 32;
  wide.width = 4;
  wide.height = 1;
  writeFile(dir_ / "wide" / "a.tif", writeTiff(wide, {std::vector<std::uint32_t>{1, 2, 70000, 4}}));
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

// --- Float slices (ADR 0015) ---------------------------------------------------------------------

/// Attenuation-like float values, negative ones included, that are not on any 16-bit grid.
double floatValue(std::int64_t x, std::int64_t y, std::int64_t z) {
  return -0.03 + 0.0011 * static_cast<double>(x) + 0.0023 * static_cast<double>(y) +
         0.017 * static_cast<double>(z) + 1.3e-5 * static_cast<double>((x * 7 + y * 3) % 11);
}

/// The values of slice `z` as 32 or 64-bit bit patterns, with one not-a-number when `with_nan`.
std::vector<std::uint64_t> floatPixels(std::int64_t z, int bits, bool with_nan = false) {
  std::vector<std::uint64_t> pixels;
  for (std::int64_t y = 0; y < kDims[1]; ++y) {
    for (std::int64_t x = 0; x < kDims[0]; ++x) {
      const double v = with_nan && x == 3 && y == 2 ? std::nan("") : floatValue(x, y, z);
      pixels.push_back(bits == 32 ? std::bit_cast<std::uint32_t>(static_cast<float>(v))
                                  : std::bit_cast<std::uint64_t>(v));
    }
  }
  return pixels;
}

std::filesystem::path writeFloatStack(const std::filesystem::path& folder, TiffSpec spec) {
  spec.width = static_cast<std::uint32_t>(kDims[0]);
  spec.height = static_cast<std::uint32_t>(kDims[1]);
  spec.sample_format = 3;
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    writeFile(folder / ("f" + std::to_string(z) + ".tif"),
              writeTiff(spec, std::vector<std::vector<std::uint64_t>>{floatPixels(z, spec.bits)}));
  }
  return folder;
}

/// Every grey value maps back to its float value within half a grey step.
void expectFloatValues(const TiffStackSource& source, int bits) {
  ASSERT_EQ(source.dims(), kDims);
  const ValueMapping mapping = source.valueMapping();
  ASSERT_FALSE(mapping.isIdentity());
  EXPECT_DOUBLE_EQ(mapping.toValue(0.0), source.valueRange()[0]);
  EXPECT_NEAR(mapping.toValue(65535.0), source.valueRange()[1], 1e-12);
  const auto grey = readAll(source);
  std::size_t i = 0;
  double worst = 0.0;
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    for (std::int64_t y = 0; y < kDims[1]; ++y) {
      for (std::int64_t x = 0; x < kDims[0]; ++x, ++i) {
        const double stored = bits == 32
                                  ? static_cast<double>(static_cast<float>(floatValue(x, y, z)))
                                  : floatValue(x, y, z);
        worst = std::max(worst, std::abs(mapping.toValue(grey[i]) - stored));
      }
    }
  }
  EXPECT_LE(worst, 0.5 * mapping.scale * (1.0 + 1e-9));
  EXPECT_EQ(source.clippedValues(), 0U);
}

TEST_F(TiffTest, MapsFloatSlicesOntoGreyValues) {
  struct Case {
    std::string name;
    TiffSpec spec;
  };
  std::vector<Case> cases;
  const auto add = [&cases](const std::string& name, auto&& change) {
    TiffSpec spec;
    spec.bits = 32;
    change(spec);
    cases.push_back({name, spec});
  };
  add("float32", [](TiffSpec&) {});
  add("float32_deflate_predictor", [](TiffSpec& s) {
    s.compression = 8;
    s.predictor = 3;
  });
  add("float32_lzw_tiles_predictor", [](TiffSpec& s) {
    s.compression = 5;
    s.predictor = 3;
    s.tile = 16;
  });
  add("float32_big_endian_strips", [](TiffSpec& s) {
    s.big_endian = true;
    s.rows_per_strip = 5;
  });
  add("float64", [](TiffSpec& s) { s.bits = 64; });
  add("float64_deflate_predictor_big_endian", [](TiffSpec& s) {
    s.bits = 64;
    s.compression = 8;
    s.predictor = 3;
    s.big_endian = true;
  });
  const double low = floatValue(0, 0, 0);
  const double high = floatValue(kDims[0] - 1, kDims[1] - 1, kDims[2] - 1);
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    const auto folder = writeFloatStack(dir_ / c.name, c.spec);
    const TiffStackSource estimated(folder);
    EXPECT_TRUE(estimated.isFloat());
    EXPECT_EQ(estimated.bitsPerSample(), c.spec.bits);
    // The estimate covers every value with a margin of 5 % of the range on each side.
    EXPECT_NEAR(estimated.valueRange()[0], low - 0.05 * (high - low), 1e-6);
    EXPECT_NEAR(estimated.valueRange()[1], high + 0.05 * (high - low), 1e-6);
    expectFloatValues(estimated, c.spec.bits);

    TiffStackOptions options;
    options.value_range = std::array<double, 2>{-0.1, 0.4};
    const TiffStackSource given(folder, options);
    EXPECT_EQ(given.valueRange(), options.value_range);
    expectFloatValues(given, c.spec.bits);
  }
}

TEST_F(TiffTest, CountsClippedFloatValuesOnce) {
  TiffSpec spec;
  spec.bits = 32;
  spec.width = static_cast<std::uint32_t>(kDims[0]);
  spec.height = static_cast<std::uint32_t>(kDims[1]);
  spec.sample_format = 3;
  spec.tile = 16;  // padding beyond the image must not count
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    writeFile(dir_ / "clip" / ("f" + std::to_string(z) + ".tif"),
              writeTiff(spec, std::vector<std::vector<std::uint64_t>>{
                                  floatPixels(z, 32, /*with_nan=*/z == 1)}));
  }
  TiffStackOptions options;
  const double limit = 0.05;
  options.value_range = std::array<double, 2>{0.0, limit};
  const TiffStackSource source(dir_ / "clip", options);
  std::uint64_t outside = 1;  // the not-a-number
  for (std::int64_t z = 0; z < kDims[2]; ++z) {
    for (std::int64_t y = 0; y < kDims[1]; ++y) {
      for (std::int64_t x = 0; x < kDims[0]; ++x) {
        const auto v = static_cast<double>(static_cast<float>(floatValue(x, y, z)));
        const bool nan = z == 1 && x == 3 && y == 2;
        outside += !nan && (std::round(v / limit * 65535.0) < 0.0 ||
                            std::round(v / limit * 65535.0) > 65535.0)
                       ? 1
                       : 0;
      }
    }
  }
  const auto grey = readAll(source);
  (void)readAll(source);  // a second pass, as the sieve makes, counts nothing new
  EXPECT_EQ(source.clippedValues(), outside);
  EXPECT_EQ(grey[0], 0);                                       // -0.03: below the range
  EXPECT_EQ(grey[kDims[0] * kDims[1] + 2 * kDims[0] + 3], 0);  // not a number
  EXPECT_EQ(grey.back(), 65535);                               // above the range
}

TEST_F(TiffTest, DatasetKeepsTheValueMapping) {
  TiffSpec spec;
  spec.bits = 32;
  spec.compression = 8;
  spec.predictor = 3;
  const auto folder = writeFloatStack(dir_ / "float", spec);
  const TiffStackSource source(folder);
  DatasetOptions options;
  options.brick_size = 16;
  const DatasetInfo written = writeDataset(source, dir_ / "float.vsieve", options);
  EXPECT_EQ(written.value_mapping, source.valueMapping());
  const DatasetInfo read = readDatasetInfo(dir_ / "float.vsieve");
  EXPECT_DOUBLE_EQ(read.value_mapping.offset, source.valueMapping().offset);
  EXPECT_DOUBLE_EQ(read.value_mapping.scale, source.valueMapping().scale);
  // Integer scans keep the identity.
  const DatasetInfo integer =
      writeDataset(TiffStackSource(writeStack({}, "integer")), dir_ / "integer.vsieve", options);
  EXPECT_TRUE(readDatasetInfo(dir_ / "integer.vsieve").value_mapping.isIdentity());
  EXPECT_EQ(integer.value_mapping, ValueMapping{});

  // Joined parts must map their values alike.
  TiffStackOptions other;
  other.value_range = std::array<double, 2>{-1.0, 1.0};
  std::vector<std::unique_ptr<VolumeSource>> parts;
  parts.push_back(std::make_unique<TiffStackSource>(folder));
  parts.push_back(std::make_unique<TiffStackSource>(folder, other));
  EXPECT_THROW(ConcatSource(std::move(parts), 2), std::invalid_argument);
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

  // Float slices with a given value range.
  TiffSpec floating;
  floating.bits = 32;
  const auto folder = writeFloatStack(dir_ / "input" / "float", floating);
  const auto from_float =
      studio.call("run_import_tiff",
                  {{"path", folder.string()}, {"value_range", {-0.1, 0.4}}, {"brick_size", 16}});
  EXPECT_EQ(from_float.at("status"), "done");
  EXPECT_EQ(from_float.at("summary").at("value_range"), (std::array<double, 2>{-0.1, 0.4}));
  EXPECT_EQ(from_float.at("summary").at("clipped_values"), 0);
  EXPECT_DOUBLE_EQ(from_float.at("summary").at("value_mapping").at("scale").get<double>(),
                   0.5 / 65535.0);
}

}  // namespace
}  // namespace voxelsieve
