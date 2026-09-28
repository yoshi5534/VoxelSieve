#include "detail/tiff.hpp"

#include <algorithm>
#include <array>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string_view>

namespace voxelsieve::detail {
namespace {

enum Tag : std::uint16_t {
  kWidth = 256,
  kHeight = 257,
  kBitsPerSample = 258,
  kCompression = 259,
  kPhotometric = 262,
  kDescription = 270,
  kStripOffsets = 273,
  kSamplesPerPixel = 277,
  kRowsPerStrip = 278,
  kStripByteCounts = 279,
  kXResolution = 282,
  kYResolution = 283,
  kResolutionUnit = 296,
  kPredictor = 317,
  kTileWidth = 322,
  kTileLength = 323,
  kTileOffsets = 324,
  kTileByteCounts = 325,
  kSampleFormat = 339,
};

std::size_t typeSize(std::uint16_t type) {
  switch (type) {
    case 1:
    case 2:
    case 6:
    case 7:
      return 1;
    case 3:
    case 8:
      return 2;
    case 4:
    case 9:
    case 11:
    case 13:
      return 4;
    case 5:
    case 10:
    case 12:
    case 16:
    case 17:
    case 18:
      return 8;
    default:
      return 0;
  }
}

class Reader {
 public:
  Reader(const ByteSource& bytes, bool big_endian) : bytes_(bytes), big_endian_(big_endian) {}

  [[nodiscard]] std::uint64_t unsignedAt(std::uint64_t offset, std::size_t size) const {
    std::array<std::uint8_t, 8> b{};
    bytes_.read(offset, std::span(b.data(), size));
    return decode(b.data(), size);
  }
  [[nodiscard]] std::uint64_t decode(const std::uint8_t* p, std::size_t size) const {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
      const std::size_t k = big_endian_ ? i : size - 1 - i;
      value = (value << 8U) | p[k];
    }
    return value;
  }
  [[nodiscard]] const ByteSource& bytes() const { return bytes_; }

 private:
  const ByteSource& bytes_;
  bool big_endian_;
};

struct Field {
  std::uint16_t type = 0;
  std::uint64_t count = 0;
  std::vector<std::uint8_t> data;  // count * type size bytes
};

std::vector<std::uint64_t> integers(const Reader& reader, const Field& field) {
  const std::size_t size = typeSize(field.type);
  std::vector<std::uint64_t> values;
  if (field.type != 1 && field.type != 3 && field.type != 4 && field.type != 16 &&
      field.type != 13 && field.type != 18) {
    throw std::runtime_error("TIFF field of type " + std::to_string(field.type) +
                             " where an integer is expected");
  }
  values.reserve(field.count);
  for (std::uint64_t i = 0; i < field.count; ++i) {
    values.push_back(reader.decode(&field.data[i * size], size));
  }
  return values;
}

std::uint64_t integer(const Reader& reader, const std::map<std::uint16_t, Field>& fields,
                      std::uint16_t tag, std::uint64_t fallback) {
  const auto found = fields.find(tag);
  if (found == fields.end() || found->second.count == 0) {
    return fallback;
  }
  return integers(reader, found->second).front();
}

double rational(const Reader& reader, const Field& field) {
  if (field.type != 5 || field.count == 0) {
    return 0.0;
  }
  const auto numerator = static_cast<double>(reader.decode(field.data.data(), 4));
  const auto denominator = static_cast<double>(reader.decode(field.data.data() + 4, 4));
  return denominator > 0.0 ? numerator / denominator : 0.0;
}

/// Number after `key` in an ImageJ description ("spacing=0.6"); 0 if missing.
double imageJNumber(const std::string& description, const std::string& key) {
  const auto pos = description.find(key);
  if (pos == std::string::npos || (pos > 0 && description[pos - 1] != '\n')) {
    return 0.0;
  }
  try {
    const double value = std::stod(description.substr(pos + key.size()));
    return value > 0.0 && std::isfinite(value) ? value : 0.0;
  } catch (const std::exception&) {
    return 0.0;
  }
}

/// Length unit of an ImageJ description ("unit=micron"), in mm; 0 if none.
double imageJUnitMm(const std::string& description) {
  const auto pos = description.find("unit=");
  if (pos == std::string::npos) {
    return 0.0;
  }
  const auto end = description.find('\n', pos);
  std::string unit = description.substr(pos + 5, end == std::string::npos ? end : end - pos - 5);
  std::erase_if(unit, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
  if (unit == "micron" || unit == "um" || unit == "µm" || unit == "microns") {
    return 1e-3;
  }
  if (unit == "mm") {
    return 1.0;
  }
  if (unit == "cm") {
    return 10.0;
  }
  if (unit == "nm") {
    return 1e-6;
  }
  return 0.0;
}

void inflate(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out) {
  namespace io = boost::iostreams;
  io::filtering_istream stream;
  stream.push(io::zlib_decompressor());
  stream.push(io::array_source(reinterpret_cast<const char*>(in.data()), in.size()));
  stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
  out.resize(static_cast<std::size_t>(stream.gcount()));
}

}  // namespace

FileBytes::FileBytes(std::filesystem::path path) : path_(std::move(path)) {
  size_ = std::filesystem::file_size(path_);
}

void FileBytes::read(std::uint64_t offset, std::span<std::uint8_t> out) const {
  if (offset + out.size() > size_) {
    throw std::runtime_error("Read beyond the end of " + path_.string());
  }
  std::ifstream in(path_, std::ios::binary);
  in.seekg(static_cast<std::streamoff>(offset));
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
  if (!in) {
    throw std::runtime_error("Cannot read " + path_.string());
  }
}

void MemoryBytes::read(std::uint64_t offset, std::span<std::uint8_t> out) const {
  if (offset + out.size() > bytes_.size()) {
    throw std::runtime_error("Read beyond the end of a TIFF image");
  }
  std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
}

std::vector<TiffPage> readTiffPages(const ByteSource& bytes, std::size_t max_pages) {
  if (bytes.size() < 8) {
    throw std::runtime_error("Not a TIFF file");
  }
  std::array<std::uint8_t, 4> magic{};
  bytes.read(0, magic);
  bool big_endian = false;
  if (magic[0] == 'M' && magic[1] == 'M') {
    big_endian = true;
  } else if (magic[0] != 'I' || magic[1] != 'I') {
    throw std::runtime_error("Not a TIFF file");
  }
  const Reader reader(bytes, big_endian);
  const std::uint64_t version = reader.decode(magic.data() + 2, 2);
  const bool big_tiff = version == 43;
  if (version != 42 && !big_tiff) {
    throw std::runtime_error("Not a TIFF file");
  }
  const std::size_t offset_size = big_tiff ? 8 : 4;
  std::uint64_t ifd = reader.unsignedAt(big_tiff ? 8 : 4, offset_size);
  std::vector<TiffPage> pages;
  std::vector<std::uint64_t> seen;
  while (ifd != 0 && pages.size() < max_pages) {
    if (std::find(seen.begin(), seen.end(), ifd) != seen.end() || ifd >= bytes.size()) {
      throw std::runtime_error("Broken TIFF directory chain");
    }
    seen.push_back(ifd);
    const std::size_t count_size = big_tiff ? 8 : 2;
    const std::size_t entry_size = big_tiff ? 20 : 12;
    const std::uint64_t count = reader.unsignedAt(ifd, count_size);
    std::vector<std::uint8_t> entries(count * entry_size);
    bytes.read(ifd + count_size, entries);
    std::map<std::uint16_t, Field> fields;
    for (std::uint64_t i = 0; i < count; ++i) {
      const std::uint8_t* e = &entries[i * entry_size];
      const auto tag = static_cast<std::uint16_t>(reader.decode(e, 2));
      Field field;
      field.type = static_cast<std::uint16_t>(reader.decode(e + 2, 2));
      field.count = reader.decode(e + 4, offset_size);
      const std::size_t size = typeSize(field.type);
      if (size == 0) {
        continue;
      }
      const std::uint64_t length = field.count * size;
      if (length > (std::uint64_t{1} << 30)) {
        throw std::runtime_error("TIFF field too large");
      }
      field.data.resize(length);
      if (length <= offset_size) {
        std::copy_n(e + 4 + offset_size, length, field.data.begin());
      } else {
        bytes.read(reader.decode(e + 4 + offset_size, offset_size), field.data);
      }
      fields[tag] = std::move(field);
    }
    ifd = reader.unsignedAt(ifd + count_size + count * entry_size, offset_size);

    TiffPage page;
    page.big_endian = big_endian;
    page.width = static_cast<std::uint32_t>(integer(reader, fields, kWidth, 0));
    page.height = static_cast<std::uint32_t>(integer(reader, fields, kHeight, 0));
    page.bits = static_cast<int>(integer(reader, fields, kBitsPerSample, 1));
    page.sample_format = static_cast<int>(integer(reader, fields, kSampleFormat, 1));
    page.samples_per_pixel = static_cast<int>(integer(reader, fields, kSamplesPerPixel, 1));
    page.compression = static_cast<int>(integer(reader, fields, kCompression, 1));
    page.predictor = static_cast<int>(integer(reader, fields, kPredictor, 1));
    page.photometric = static_cast<int>(integer(reader, fields, kPhotometric, 1));
    if (const auto d = fields.find(kDescription); d != fields.end()) {
      page.description.assign(d->second.data.begin(), d->second.data.end());
      page.description = page.description.c_str();  // up to the terminating zero
    }
    if (fields.contains(kTileWidth)) {
      page.chunk_width = static_cast<std::uint32_t>(integer(reader, fields, kTileWidth, 0));
      page.chunk_height = static_cast<std::uint32_t>(integer(reader, fields, kTileLength, 0));
      if (page.chunk_width == 0 || page.chunk_height == 0) {
        throw std::runtime_error("TIFF tile of size 0");
      }
      page.chunks_across = (page.width + page.chunk_width - 1) / page.chunk_width;
      page.offsets = integers(reader, fields.at(kTileOffsets));
      page.byte_counts = integers(reader, fields.at(kTileByteCounts));
    } else {
      if (!fields.contains(kStripOffsets) || !fields.contains(kStripByteCounts)) {
        throw std::runtime_error("TIFF image without strips or tiles");
      }
      page.chunk_width = page.width;
      page.chunk_height = static_cast<std::uint32_t>(std::min<std::uint64_t>(
          integer(reader, fields, kRowsPerStrip, page.height), page.height));
      page.offsets = integers(reader, fields.at(kStripOffsets));
      page.byte_counts = integers(reader, fields.at(kStripByteCounts));
    }
    if (page.width == 0 || page.height == 0 || page.chunk_height == 0) {
      throw std::runtime_error("TIFF image of size 0");
    }
    const std::uint64_t chunks_down = (page.height + page.chunk_height - 1) / page.chunk_height;
    if (page.offsets.size() < chunks_down * page.chunks_across ||
        page.byte_counts.size() < page.offsets.size()) {
      throw std::runtime_error("TIFF image with missing strips or tiles");
    }
    // Pixel size: a centimetre resolution, or pixels per unit of an ImageJ description. Inch
    // resolutions are left out: they are almost always a printing default such as 72 dpi.
    // The slice spacing comes only from an ImageJ description ("spacing=").
    const auto unit = integer(reader, fields, kResolutionUnit, 2);
    const double image_j_unit_mm = unit == 1 ? imageJUnitMm(page.description) : 0.0;
    const auto size_of = [&](std::uint16_t tag) {
      const auto field = fields.find(tag);
      const double resolution = field == fields.end() ? 0.0 : rational(reader, field->second);
      if (resolution > 0.0 && unit == 3) {
        return 10.0 / resolution;
      }
      return resolution > 0.0 && image_j_unit_mm > 0.0 ? image_j_unit_mm / resolution : 0.0;
    };
    page.pixel_size_mm = size_of(kXResolution);
    page.pixel_height_mm = size_of(kYResolution);
    page.slice_spacing_mm = image_j_unit_mm * imageJNumber(page.description, "spacing=");
    pages.push_back(std::move(page));
  }
  if (pages.empty()) {
    throw std::runtime_error("TIFF file without images");
  }
  return pages;
}

void checkSupported(const TiffPage& page) {
  if (page.samples_per_pixel != 1 || page.photometric > 1) {
    throw std::runtime_error("Only grey-value TIFF images are supported, not colour");
  }
  if (page.sample_format != 1) {
    throw std::runtime_error(
        page.sample_format == 3
            ? "Float TIFF images are not supported: grey values are kept as 16-bit integers"
            : "Signed TIFF images are not supported: grey values are kept as unsigned 16-bit");
  }
  if (page.bits != 8 && page.bits != 16 && page.bits != 32) {
    throw std::runtime_error("TIFF images with " + std::to_string(page.bits) +
                             " bits per sample are not supported (8, 16 or 32)");
  }
  if (page.compression != 1 && page.compression != 5 && page.compression != 8 &&
      page.compression != 32946 && page.compression != 32773) {
    throw std::runtime_error("TIFF compression " + std::to_string(page.compression) +
                             " is not supported (none, LZW, Deflate, PackBits)");
  }
  if (page.predictor != 1 && page.predictor != 2) {
    throw std::runtime_error("TIFF predictor " + std::to_string(page.predictor) +
                             " is not supported");
  }
}

std::vector<std::uint8_t> lzwDecode(std::span<const std::uint8_t> in, std::size_t expected) {
  constexpr std::uint32_t kClear = 256;
  constexpr std::uint32_t kEnd = 257;
  std::array<std::uint16_t, 4096> prefix{};
  std::array<std::uint8_t, 4096> suffix{};
  std::array<std::uint8_t, 4096> first{};
  std::array<std::uint16_t, 4096> length{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    suffix[i] = static_cast<std::uint8_t>(i);
    first[i] = static_cast<std::uint8_t>(i);
    length[i] = 1;
  }
  std::vector<std::uint8_t> out;
  out.reserve(expected);
  std::uint64_t bit = 0;
  const std::uint64_t bits = in.size() * 8;
  std::uint32_t width = 9;
  std::uint32_t next = 258;
  std::uint32_t old = kClear;
  const auto emit = [&](std::uint32_t code) {
    const std::size_t start = out.size();
    out.resize(start + length[code]);
    for (std::size_t i = length[code]; i > 0; --i) {
      out[start + i - 1] = suffix[code];
      code = prefix[code];
    }
  };
  while (bit + width <= bits && out.size() < expected) {
    std::uint32_t code = 0;
    for (std::uint32_t i = 0; i < width; ++i, ++bit) {
      code = (code << 1U) | ((in[bit >> 3U] >> (7U - (bit & 7U))) & 1U);
    }
    if (code == kEnd) {
      break;
    }
    if (code == kClear) {
      width = 9;
      next = 258;
      old = kClear;
      continue;
    }
    if (old == kClear) {
      if (code > 255) {
        throw std::runtime_error("Broken LZW data");
      }
      emit(code);
      old = code;
      continue;
    }
    if (code > next || next >= 4096) {
      throw std::runtime_error("Broken LZW data");
    }
    const std::uint8_t head = code < next ? first[code] : first[old];
    prefix[next] = static_cast<std::uint16_t>(old);
    suffix[next] = head;
    first[next] = first[old];
    length[next] = static_cast<std::uint16_t>(length[old] + 1);
    ++next;
    emit(code);
    old = code;
    if (next + 1 >= (1U << width) && width < 12) {
      ++width;  // early change, as in libtiff
    }
  }
  if (out.size() > expected) {
    out.resize(expected);
  }
  return out;
}

std::vector<std::uint8_t> packBitsDecode(std::span<const std::uint8_t> in, std::size_t expected) {
  std::vector<std::uint8_t> out;
  out.reserve(expected);
  for (std::size_t i = 0; i < in.size() && out.size() < expected;) {
    const auto n = static_cast<std::int8_t>(in[i++]);
    if (n >= 0) {
      const std::size_t count =
          std::min<std::size_t>(static_cast<std::size_t>(n) + 1, in.size() - i);
      out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(i),
                 in.begin() + static_cast<std::ptrdiff_t>(i + count));
      i += count;
    } else if (n != -128 && i < in.size()) {
      out.insert(out.end(), static_cast<std::size_t>(1 - n), in[i++]);
    }
  }
  if (out.size() > expected) {
    out.resize(expected);
  }
  return out;
}

std::vector<std::uint16_t> decodeTiffChunk(const ByteSource& bytes, const TiffPage& page,
                                           std::size_t chunk) {
  checkSupported(page);
  if (chunk >= page.offsets.size()) {
    throw std::out_of_range("TIFF chunk out of range");
  }
  const std::size_t sample_bytes = static_cast<std::size_t>(page.bits) / 8;
  const std::size_t row_bytes = static_cast<std::size_t>(page.chunk_width) * sample_bytes;
  // A strip at the bottom holds only the rows that are left; tiles are always complete.
  std::size_t rows = page.chunk_height;
  if (page.chunks_across == 1 && page.chunk_width == page.width) {
    const std::size_t top = chunk * page.chunk_height;
    rows = std::min<std::size_t>(page.chunk_height, page.height - top);
  }
  const std::size_t expected = rows * row_bytes;
  std::vector<std::uint8_t> raw(page.byte_counts[chunk]);
  bytes.read(page.offsets[chunk], raw);
  std::vector<std::uint8_t> data;
  switch (page.compression) {
    case 1:
      data = std::move(raw);
      break;
    case 5:
      data = lzwDecode(raw, expected);
      break;
    case 8:
    case 32946:
      data.resize(expected);
      inflate(raw, data);
      break;
    case 32773:
      data = packBitsDecode(raw, expected);
      break;
    default:
      break;
  }
  if (data.size() < expected) {
    throw std::runtime_error("TIFF strip or tile is shorter than its image");
  }
  const Reader reader(bytes, page.big_endian);
  std::vector<std::uint16_t> out(static_cast<std::size_t>(page.chunk_width) * page.chunk_height, 0);
  for (std::size_t y = 0; y < rows; ++y) {
    const std::uint8_t* row = &data[y * row_bytes];
    std::uint64_t previous = 0;
    for (std::size_t x = 0; x < page.chunk_width; ++x) {
      std::uint64_t value =
          sample_bytes == 1 ? row[x] : reader.decode(&row[x * sample_bytes], sample_bytes);
      if (page.predictor == 2) {
        const std::uint64_t mask = page.bits == 64 ? ~0ULL : (1ULL << page.bits) - 1;
        value = (value + previous) & mask;
        previous = value;
      }
      if (value > 0xFFFF) {
        throw std::runtime_error("32-bit TIFF value " + std::to_string(value) +
                                 " exceeds 65535 and cannot be kept exactly");
      }
      out[y * page.chunk_width + x] = static_cast<std::uint16_t>(value);
    }
  }
  return out;
}

}  // namespace voxelsieve::detail
