#pragma once

// PNG reading and writing through libpng (ADR 0016).

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace voxelsieve::detail {

/// A grey image decoded from a PNG file, rows top to bottom.
struct GreyImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  int bit_depth = 8;  // 8 or 16
  std::vector<std::uint16_t> values;
};

/// Decodes a PNG file into grey values: 8-bit images keep their values (0 to 255), 16-bit images
/// theirs (0 to 65535); colour images are converted to grey and alpha is dropped.
[[nodiscard]] GreyImage decodeGreyPng(std::span<const std::uint8_t> png);

/// Encodes an 8-bit image with 1 (grey), 3 (RGB) or 4 (RGBA) channels, rows top to bottom.
[[nodiscard]] std::vector<std::uint8_t> encodePng(std::uint32_t width, std::uint32_t height,
                                                  int channels,
                                                  std::span<const std::uint8_t> pixels);

/// Writes an 8-bit RGB image, rows top to bottom, 3 bytes per pixel.
void writeRgbPng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                 std::span<const std::uint8_t> rgb);

/// As above for pixels of `pixel_width` x `pixel_height` (e.g. mm): the image is stretched along
/// the coarser axis, nearest neighbour, so that it shows true proportions (ADR 0012).
void writeRgbPng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                 std::span<const std::uint8_t> rgb, double pixel_width, double pixel_height);

}  // namespace voxelsieve::detail
