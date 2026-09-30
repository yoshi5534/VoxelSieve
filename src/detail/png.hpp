#pragma once

// PNG writing through libpng (ADR 0016).

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace voxelsieve::detail {

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
