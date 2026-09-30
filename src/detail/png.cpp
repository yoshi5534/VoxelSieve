#include "detail/png.hpp"

#include <png.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace voxelsieve::detail {

std::vector<std::uint8_t> encodePng(std::uint32_t width, std::uint32_t height, int channels,
                                    std::span<const std::uint8_t> pixels) {
  if (channels != 1 && channels != 3 && channels != 4) {
    throw std::invalid_argument("PNG images need 1, 3 or 4 channels");
  }
  const std::size_t row = std::size_t{width} * static_cast<std::size_t>(channels);
  if (pixels.size() != row * height || width == 0 || height == 0) {
    throw std::invalid_argument("Image size does not match its pixel data");
  }
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  image.width = width;
  image.height = height;
  image.format = channels == 1 ? PNG_FORMAT_GRAY : channels == 3 ? PNG_FORMAT_RGB : PNG_FORMAT_RGBA;
  // The first call measures, the second writes.
  png_alloc_size_t size = 0;
  if (png_image_write_to_memory(&image, nullptr, &size, 0, pixels.data(), 0, nullptr) == 0) {
    const std::string message = image.message;
    png_image_free(&image);
    throw std::runtime_error("Cannot encode PNG: " + message);
  }
  std::vector<std::uint8_t> png(size);
  if (png_image_write_to_memory(&image, png.data(), &size, 0, pixels.data(), 0, nullptr) == 0) {
    const std::string message = image.message;
    png_image_free(&image);
    throw std::runtime_error("Cannot encode PNG: " + message);
  }
  png.resize(size);
  return png;
}

void writeRgbPng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                 std::span<const std::uint8_t> rgb) {
  const std::vector<std::uint8_t> png = encodePng(width, height, 3, rgb);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  if (!out) {
    throw std::runtime_error("Cannot write " + path.string());
  }
}

void writeRgbPng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                 std::span<const std::uint8_t> rgb, double pixel_width, double pixel_height) {
  const double finer = std::min(pixel_width, pixel_height);
  const auto out_width = static_cast<std::uint32_t>(
      std::max(1.0, std::round(static_cast<double>(width) * pixel_width / finer)));
  const auto out_height = static_cast<std::uint32_t>(
      std::max(1.0, std::round(static_cast<double>(height) * pixel_height / finer)));
  if (out_width == width && out_height == height) {
    writeRgbPng(path, width, height, rgb);
    return;
  }
  std::vector<std::uint8_t> stretched(std::size_t{out_width} * out_height * 3);
  for (std::uint32_t y = 0; y < out_height; ++y) {
    const std::size_t sy = std::min<std::size_t>(height - 1, std::size_t{y} * height / out_height);
    for (std::uint32_t x = 0; x < out_width; ++x) {
      const std::size_t sx = std::min<std::size_t>(width - 1, std::size_t{x} * width / out_width);
      const std::size_t from = (sy * width + sx) * 3;
      const std::size_t to = (std::size_t{y} * out_width + x) * 3;
      std::copy_n(rgb.begin() + static_cast<std::ptrdiff_t>(from), 3,
                  stretched.begin() + static_cast<std::ptrdiff_t>(to));
    }
  }
  writeRgbPng(path, out_width, out_height, stretched);
}

}  // namespace voxelsieve::detail
