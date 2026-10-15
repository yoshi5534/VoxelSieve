#include "detail/tiff.hpp"

#include <tiffio.h>

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace voxelsieve::detail {
namespace {

/// A ByteSource as libtiff sees a file: a read position and the callbacks of TIFFClientOpen.
struct Stream {
  const ByteSource* bytes = nullptr;
  std::uint64_t position = 0;
  std::string error;  // first message of libtiff's error handler
};

tmsize_t readProc(thandle_t handle, void* buffer, tmsize_t size) {
  auto& stream = *static_cast<Stream*>(handle);
  const std::uint64_t total = stream.bytes->size();
  const std::uint64_t n =
      std::min<std::uint64_t>(static_cast<std::uint64_t>(std::max<tmsize_t>(size, 0)),
                              total - std::min(stream.position, total));
  try {
    stream.bytes->read(stream.position, std::span(static_cast<std::uint8_t*>(buffer), n));
  } catch (const std::exception& e) {
    stream.error = e.what();
    return -1;
  }
  stream.position += n;
  return static_cast<tmsize_t>(n);
}

tmsize_t writeProc(thandle_t /*handle*/, void* /*buffer*/, tmsize_t /*size*/) { return -1; }

toff_t seekProc(thandle_t handle, toff_t offset, int whence) {
  auto& stream = *static_cast<Stream*>(handle);
  switch (whence) {
    case SEEK_SET:
      stream.position = offset;
      break;
    case SEEK_CUR:
      stream.position += offset;
      break;
    case SEEK_END:
      stream.position = stream.bytes->size() + offset;
      break;
    default:
      return static_cast<toff_t>(-1);
  }
  return stream.position;
}

int closeProc(thandle_t /*handle*/) { return 0; }

toff_t sizeProc(thandle_t handle) { return static_cast<Stream*>(handle)->bytes->size(); }

int errorHandler(TIFF* /*tiff*/, void* user_data, const char* module, const char* format,
                 va_list args) {
  auto& stream = *static_cast<Stream*>(user_data);
  if (stream.error.empty()) {
    std::array<char, 512> message{};
    std::vsnprintf(message.data(), message.size(), format, args);
    stream.error = std::string(module != nullptr ? module : "TIFF") + ": " + message.data();
  }
  return 1;  // handled, nothing on stderr
}

int warningHandler(TIFF* /*tiff*/, void* /*user_data*/, const char* /*module*/,
                   const char* /*format*/, va_list /*args*/) {
  return 1;  // unknown tags and the like are no reason to stop
}

/// An open TIFF image on a ByteSource; closed on destruction.
class Tiff {
 public:
  /// Opens the file; `header_only` skips reading the first directory.
  Tiff(const ByteSource& bytes, bool header_only) {
    stream_.bytes = &bytes;
    TIFFOpenOptions* options = TIFFOpenOptionsAlloc();
    TIFFOpenOptionsSetErrorHandlerExtR(options, errorHandler, &stream_);
    TIFFOpenOptionsSetWarningHandlerExtR(options, warningHandler, &stream_);
    tiff_ = TIFFClientOpenExt("tiff", header_only ? "rhm" : "rm", &stream_, readProc, writeProc,
                              seekProc, closeProc, sizeProc, nullptr, nullptr, options);
    TIFFOpenOptionsFree(options);
    if (tiff_ == nullptr) {
      fail("Not a TIFF file");
    }
  }
  Tiff(const Tiff&) = delete;
  Tiff& operator=(const Tiff&) = delete;
  Tiff(Tiff&&) = delete;
  Tiff& operator=(Tiff&&) = delete;
  ~Tiff() {
    if (tiff_ != nullptr) {
      TIFFClose(tiff_);
    }
  }

  [[nodiscard]] TIFF* get() const { return tiff_; }

  /// Throws with libtiff's message, if it gave one.
  [[noreturn]] void fail(const std::string& what) const {
    throw std::runtime_error(stream_.error.empty() ? what : what + " (" + stream_.error + ")");
  }

 private:
  Stream stream_;
  TIFF* tiff_ = nullptr;
};

template <typename T>
T field(TIFF* tiff, ttag_t tag, T fallback) {
  T value = fallback;
  if (TIFFGetField(tiff, tag, &value) == 0) {
    return fallback;
  }
  return value;
}

/// Number after `key` in a key=value image description ("spacing=0.6"); 0 if missing.
double descriptionNumber(const std::string& description, const std::string& key) {
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

/// A length unit in mm ("micron", "um", "µm", "nm", "mm", "cm", "m"); 0 if unknown.
double unitMm(std::string unit) {
  std::erase_if(unit, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
  if (unit == "micron" || unit == "um" || unit == "µm" || unit == "microns" ||
      unit == "μm") {
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
  if (unit == "m") {
    return 1e3;
  }
  return 0.0;
}

/// Length unit of a key=value image description ("unit=micron"), in mm; 0 if none.
double descriptionUnitMm(const std::string& description) {
  const auto pos = description.find("unit=");
  if (pos == std::string::npos) {
    return 0.0;
  }
  const auto end = description.find('\n', pos);
  return unitMm(description.substr(pos + 5, end == std::string::npos ? end : end - pos - 5));
}

/// Pixel size of an OME-XML image description (PhysicalSizeX, Y and Z of its Pixels element, in
/// micrometres unless a unit is given), in mm; zeros for what it does not give.
std::array<double, 3> omePixelSizeMm(const std::string& description) {
  std::array<double, 3> size{};
  if (description.find("<OME") == std::string::npos) {
    return size;
  }
  pugi::xml_document document;
  if (!document.load_string(description.c_str())) {
    return size;
  }
  const pugi::xml_node pixels = document.find_node([](const pugi::xml_node& node) {
    const std::string_view name = node.name();
    return name == "Pixels" || name.ends_with(":Pixels");
  });
  constexpr std::array<const char*, 3> kAxes = {"X", "Y", "Z"};
  for (std::size_t a = 0; a < 3; ++a) {
    const std::string key = std::string("PhysicalSize") + kAxes[a];
    const double value = pixels.attribute(key.c_str()).as_double(0.0);
    const pugi::xml_attribute unit = pixels.attribute((key + "Unit").c_str());
    const double unit_mm = unit ? unitMm(unit.as_string()) : 1e-3;
    if (value > 0.0 && std::isfinite(value)) {
      size[a] = value * unit_mm;
    }
  }
  return size;
}

/// Whether a resolution in pixels per inch is a real pixel size rather than a printing default
/// such as 72 or 300 dpi that most image writers put into every file.
bool physicalInchResolution(double dpi) {
  constexpr std::array<double, 11> kPrintDefaults = {72.0,  96.0,  100.0,  120.0,  150.0, 200.0,
                                                     300.0, 400.0, 600.0, 1200.0, 2400.0};
  constexpr double kFewest = 10.0;  // 2.5 mm pixels: no CT scan, often 1 dpi as a placeholder
  return dpi > kFewest && std::isfinite(dpi) &&
         std::ranges::none_of(kPrintDefaults,
                              [dpi](double d) { return std::abs(dpi - d) < 1e-3; });
}

TiffPage currentPage(TIFF* tiff) {
  TiffPage page;
  page.directory_offset = TIFFCurrentDirOffset(tiff);
  page.width = field<std::uint32_t>(tiff, TIFFTAG_IMAGEWIDTH, 0);
  page.height = field<std::uint32_t>(tiff, TIFFTAG_IMAGELENGTH, 0);
  page.bits = field<std::uint16_t>(tiff, TIFFTAG_BITSPERSAMPLE, 1);
  page.sample_format = field<std::uint16_t>(tiff, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  page.samples_per_pixel = field<std::uint16_t>(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
  page.compression = field<std::uint16_t>(tiff, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  page.predictor = field<std::uint16_t>(tiff, TIFFTAG_PREDICTOR, PREDICTOR_NONE);
  page.photometric = field<std::uint16_t>(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  if (const char* description = field<const char*>(tiff, TIFFTAG_IMAGEDESCRIPTION, nullptr)) {
    page.description = description;
  }
  if (page.width == 0 || page.height == 0) {
    throw std::runtime_error("TIFF image of size 0");
  }
  if (TIFFIsTiled(tiff) != 0) {
    page.chunk_width = field<std::uint32_t>(tiff, TIFFTAG_TILEWIDTH, 0);
    page.chunk_height = field<std::uint32_t>(tiff, TIFFTAG_TILELENGTH, 0);
    if (page.chunk_width == 0 || page.chunk_height == 0) {
      throw std::runtime_error("TIFF tile of size 0");
    }
    page.chunks_across = (page.width + page.chunk_width - 1) / page.chunk_width;
    page.chunk_count = TIFFNumberOfTiles(tiff);
  } else {
    page.chunk_width = page.width;
    page.chunk_height =
        std::min(field<std::uint32_t>(tiff, TIFFTAG_ROWSPERSTRIP, page.height), page.height);
    page.chunk_count = TIFFNumberOfStrips(tiff);
  }
  const std::uint64_t chunks_down = (page.height + page.chunk_height - 1) / page.chunk_height;
  if (page.chunk_height == 0 || page.chunk_count < chunks_down * page.chunks_across) {
    throw std::runtime_error("TIFF image with missing strips or tiles");
  }
  // Pixel size: from an OME-XML image description, else a centimetre resolution, an inch
  // resolution that is not a printing default such as 72 or 300 dpi, or pixels per unit of a
  // key=value image description. The slice spacing comes from the OME-XML or a key=value image
  // description ("spacing=").
  const auto ome = omePixelSizeMm(page.description);
  if (ome[0] > 0.0) {
    page.pixel_size_mm = ome[0];
    page.pixel_height_mm = ome[1];
    page.slice_spacing_mm = ome[2];
    return page;
  }
  const auto unit = field<std::uint16_t>(tiff, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
  const double description_unit_mm =
      unit == RESUNIT_NONE ? descriptionUnitMm(page.description) : 0.0;
  constexpr double kMmPerInch = 25.4;
  const auto size_of = [&](ttag_t tag) {
    const double resolution = field<float>(tiff, tag, 0.0F);
    if (resolution > 0.0 && unit == RESUNIT_CENTIMETER) {
      return 10.0 / resolution;
    }
    if (unit == RESUNIT_INCH && physicalInchResolution(resolution)) {
      return kMmPerInch / resolution;
    }
    return resolution > 0.0 && description_unit_mm > 0.0 ? description_unit_mm / resolution : 0.0;
  };
  page.pixel_size_mm = size_of(TIFFTAG_XRESOLUTION);
  page.pixel_height_mm = size_of(TIFFTAG_YRESOLUTION);
  page.slice_spacing_mm = description_unit_mm * descriptionNumber(page.description, "spacing=");
  return page;
}

/// Throws unless libtiff can decode the page into single samples of a kind VoxelSieve reads.
void checkDecodable(const TiffPage& page) {
  if (page.samples_per_pixel != 1 || page.photometric > PHOTOMETRIC_MINISBLACK) {
    throw std::runtime_error("Only grey-value TIFF images are supported, not colour");
  }
  const bool is_float = page.sample_format == SAMPLEFORMAT_IEEEFP;
  if (is_float ? page.bits != 32 && page.bits != 64
               : page.bits != 8 && page.bits != 16 && page.bits != 32) {
    throw std::runtime_error("TIFF images with " + std::to_string(page.bits) +
                             " bits per sample are not supported (" +
                             (is_float ? "32 or 64" : "8, 16 or 32") + ")");
  }
  if (TIFFIsCODECConfigured(static_cast<std::uint16_t>(page.compression)) == 0) {
    throw std::runtime_error("TIFF compression " + std::to_string(page.compression) +
                             " is not supported");
  }
}

/// The decoded bytes of one strip or tile, in native byte order and with the predictor undone,
/// and the number of its rows inside the image.
struct ChunkBytes {
  std::vector<std::uint8_t> data;
  std::size_t rows = 0;
};

ChunkBytes chunkBytes(const ByteSource& bytes, const TiffPage& page, std::size_t chunk) {
  checkDecodable(page);
  if (chunk >= page.chunk_count) {
    throw std::out_of_range("TIFF chunk out of range");
  }
  const Tiff tiff(bytes, true);
  if (TIFFSetSubDirectory(tiff.get(), page.directory_offset) == 0) {
    tiff.fail("Broken TIFF directory");
  }
  const bool tiled = TIFFIsTiled(tiff.get()) != 0;
  const std::size_t row_bytes =
      std::size_t{page.chunk_width} * static_cast<std::size_t>(page.bits / 8);
  ChunkBytes result;
  // A strip at the bottom holds only the rows that are left; tiles are always complete.
  result.rows = page.chunk_height;
  if (!tiled) {
    const std::size_t top = chunk * page.chunk_height;
    result.rows = std::min<std::size_t>(page.chunk_height, page.height - top);
  }
  const std::size_t expected = result.rows * row_bytes;
  result.data.resize(std::size_t{page.chunk_height} * row_bytes);
  const auto index = static_cast<std::uint32_t>(chunk);
  const auto size = static_cast<tmsize_t>(result.data.size());
  const tmsize_t decoded = tiled
                               ? TIFFReadEncodedTile(tiff.get(), index, result.data.data(), size)
                               : TIFFReadEncodedStrip(tiff.get(), index, result.data.data(), size);
  if (decoded < 0) {
    tiff.fail("Cannot decode TIFF strip or tile " + std::to_string(chunk));
  }
  if (static_cast<std::size_t>(decoded) < expected) {
    throw std::runtime_error("TIFF strip or tile is shorter than its image");
  }
  return result;
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
  const Tiff tiff(bytes, false);
  std::vector<TiffPage> pages;
  do {
    pages.push_back(currentPage(tiff.get()));
  } while (pages.size() < max_pages && TIFFReadDirectory(tiff.get()) != 0);
  return pages;
}

void checkSupported(const TiffPage& page) {
  if (page.sample_format != SAMPLEFORMAT_UINT && page.sample_format != SAMPLEFORMAT_IEEEFP) {
    throw std::runtime_error(
        "Signed TIFF images are not supported: grey values are kept as unsigned 16-bit");
  }
  checkDecodable(page);
}

std::vector<std::uint16_t> decodeTiffChunk(const ByteSource& bytes, const TiffPage& page,
                                           std::size_t chunk) {
  if (page.sample_format == SAMPLEFORMAT_IEEEFP) {
    throw std::invalid_argument("Float TIFF chunks are decoded with decodeTiffFloatChunk");
  }
  checkSupported(page);
  const ChunkBytes chunk_bytes = chunkBytes(bytes, page, chunk);
  const std::size_t count = std::size_t{page.chunk_width} * chunk_bytes.rows;
  std::vector<std::uint16_t> out(std::size_t{page.chunk_width} * page.chunk_height, 0);
  const std::uint8_t* data = chunk_bytes.data.data();
  if (page.bits == 8) {
    std::copy_n(data, count, out.begin());
  } else if (page.bits == 16) {
    std::memcpy(out.data(), data, count * sizeof(std::uint16_t));
  } else {
    for (std::size_t i = 0; i < count; ++i) {
      std::uint32_t value = 0;
      std::memcpy(&value, data + i * sizeof(value), sizeof(value));
      if (value > 0xFFFF) {
        throw std::runtime_error("32-bit TIFF value " + std::to_string(value) +
                                 " exceeds 65535 and cannot be kept exactly");
      }
      out[i] = static_cast<std::uint16_t>(value);
    }
  }
  return out;
}

std::vector<double> decodeTiffFloatChunk(const ByteSource& bytes, const TiffPage& page,
                                         std::size_t chunk) {
  if (page.sample_format != SAMPLEFORMAT_IEEEFP) {
    throw std::invalid_argument("decodeTiffFloatChunk needs a float TIFF image");
  }
  const ChunkBytes chunk_bytes = chunkBytes(bytes, page, chunk);
  const std::size_t count = std::size_t{page.chunk_width} * chunk_bytes.rows;
  std::vector<double> out(std::size_t{page.chunk_width} * page.chunk_height, 0.0);
  const std::uint8_t* data = chunk_bytes.data.data();
  if (page.bits == 32) {
    for (std::size_t i = 0; i < count; ++i) {
      float value = 0.0F;
      std::memcpy(&value, data + i * sizeof(value), sizeof(value));
      out[i] = value;
    }
  } else {
    std::memcpy(out.data(), data, count * sizeof(double));
  }
  return out;
}

}  // namespace voxelsieve::detail
