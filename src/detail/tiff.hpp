#pragma once

// TIFF reading for grey-value slices: classic and BigTIFF, strips and tiles, uncompressed, LZW,
// Deflate and PackBits, with horizontal differencing (integers) or the floating-point predictor.
// Integer samples come out as 16-bit values, float samples as doubles.

#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace voxelsieve::detail {

/// Random access to the bytes of a file or a buffer. `read` must be thread-safe.
class ByteSource {
 public:
  ByteSource() = default;
  ByteSource(const ByteSource&) = delete;
  ByteSource& operator=(const ByteSource&) = delete;
  ByteSource(ByteSource&&) = delete;
  ByteSource& operator=(ByteSource&&) = delete;
  virtual ~ByteSource() = default;
  [[nodiscard]] virtual std::uint64_t size() const = 0;
  virtual void read(std::uint64_t offset, std::span<std::uint8_t> out) const = 0;
};

/// A file, opened for every read so that many files can be used without holding descriptors.
class FileBytes final : public ByteSource {
 public:
  explicit FileBytes(std::filesystem::path path);
  [[nodiscard]] std::uint64_t size() const override { return size_; }
  void read(std::uint64_t offset, std::span<std::uint8_t> out) const override;

 private:
  std::filesystem::path path_;
  std::uint64_t size_ = 0;
};

class MemoryBytes final : public ByteSource {
 public:
  explicit MemoryBytes(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
  [[nodiscard]] std::uint64_t size() const override { return bytes_.size(); }
  void read(std::uint64_t offset, std::span<std::uint8_t> out) const override;

 private:
  std::vector<std::uint8_t> bytes_;
};

/// One image (IFD) of a TIFF file. Strips are treated as tiles as wide as the image.
struct TiffPage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  int bits = 0;
  int sample_format = 1;  // 1 unsigned, 2 signed, 3 float
  int samples_per_pixel = 1;
  int compression = 1;
  int predictor = 1;
  int photometric = 1;
  bool big_endian = false;
  std::uint32_t chunk_width = 0;
  std::uint32_t chunk_height = 0;
  std::uint32_t chunks_across = 1;
  std::vector<std::uint64_t> offsets;
  std::vector<std::uint64_t> byte_counts;
  /// Pixel width and height in mm from a centimetre resolution or an ImageJ description, 0 if
  /// unknown; the slice spacing only from an ImageJ description ("spacing=").
  double pixel_size_mm = 0.0;
  double pixel_height_mm = 0.0;
  double slice_spacing_mm = 0.0;
  std::string description;
};

/// Reads the image directories; stops after `max_pages`.
[[nodiscard]] std::vector<TiffPage> readTiffPages(
    const ByteSource& bytes, std::size_t max_pages = std::numeric_limits<std::size_t>::max());

/// Throws unless the page is a single-channel unsigned 8, 16 or 32 bit or float 32 or 64 bit image
/// in a supported compression.
void checkSupported(const TiffPage& page);

/// Decodes one strip or tile into chunk_width x chunk_height samples (rows below the image are
/// left 0). 32-bit samples must not exceed 65535, so that they are kept exactly.
[[nodiscard]] std::vector<std::uint16_t> decodeTiffChunk(const ByteSource& bytes,
                                                         const TiffPage& page, std::size_t chunk);

/// Decodes one strip or tile of a float image into chunk_width x chunk_height values (rows below
/// the image are left 0).
[[nodiscard]] std::vector<double> decodeTiffFloatChunk(const ByteSource& bytes,
                                                       const TiffPage& page, std::size_t chunk);

/// TIFF LZW (MSB first, early change); `expected` bounds the output.
[[nodiscard]] std::vector<std::uint8_t> lzwDecode(std::span<const std::uint8_t> in,
                                                  std::size_t expected);
[[nodiscard]] std::vector<std::uint8_t> packBitsDecode(std::span<const std::uint8_t> in,
                                                       std::size_t expected);

}  // namespace voxelsieve::detail
