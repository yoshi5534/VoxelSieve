#pragma once

// Minimal PNG writer; the image data is compressed with zlib through Boost.Iostreams.

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

}  // namespace voxelsieve::detail
