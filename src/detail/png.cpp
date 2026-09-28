#include "detail/png.hpp"

#include <algorithm>
#include <array>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace voxelsieve::detail {
namespace {

std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0xFFFFFFFFU) {
  static constexpr std::array<std::uint32_t, 256> kTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
      std::uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
      }
      table[n] = c;
    }
    return table;
  }();
  for (const std::uint8_t b : bytes) {
    crc = kTable[(crc ^ b) & 0xFFU] ^ (crc >> 8U);
  }
  return crc;
}

void appendBigEndian(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
  }
}

void appendChunk(std::vector<std::uint8_t>& out, const char* type,
                 std::span<const std::uint8_t> data) {
  appendBigEndian(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  appendBigEndian(out, crc32(std::span(out).subspan(start)) ^ 0xFFFFFFFFU);
}

}  // namespace

std::vector<std::uint8_t> encodePng(std::uint32_t width, std::uint32_t height, int channels,
                                    std::span<const std::uint8_t> pixels) {
  if (channels != 1 && channels != 3 && channels != 4) {
    throw std::invalid_argument("PNG images need 1, 3 or 4 channels");
  }
  const std::size_t row = std::size_t{width} * static_cast<std::size_t>(channels);
  if (pixels.size() != row * height || width == 0 || height == 0) {
    throw std::invalid_argument("Image size does not match its pixel data");
  }
  // Filter type 0 before every row.
  std::vector<char> raw;
  raw.reserve((row + 1) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    const auto line = pixels.subspan(y * row, row);
    raw.insert(raw.end(), line.begin(), line.end());
  }
  std::vector<char> compressed;
  {
    namespace io = boost::iostreams;
    io::filtering_ostream out;
    out.push(io::zlib_compressor(io::zlib::default_compression));
    out.push(io::back_inserter(compressed));
    out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
  }

  std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<std::uint8_t> header;
  appendBigEndian(header, width);
  appendBigEndian(header, height);
  const std::uint8_t color_type = channels == 1 ? 0 : channels == 3 ? 2 : 6;
  header.insert(header.end(), {8, color_type, 0, 0, 0});  // 8 bit, deflate, no filter, no interlace
  appendChunk(png, "IHDR", header);
  const std::vector<std::uint8_t> data(compressed.begin(), compressed.end());
  appendChunk(png, "IDAT", data);
  appendChunk(png, "IEND", {});
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
