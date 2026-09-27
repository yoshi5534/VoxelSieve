#pragma once

// Minimal PNG writer without a compression library: the image data is stored in uncompressed
// deflate blocks. Good enough for analysis previews, which are small.

#include <cstdint>
#include <filesystem>
#include <span>

namespace voxelsieve::detail {

/// Writes an 8-bit RGB image, rows top to bottom, 3 bytes per pixel.
void writeRgbPng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                 std::span<const std::uint8_t> rgb);

}  // namespace voxelsieve::detail
