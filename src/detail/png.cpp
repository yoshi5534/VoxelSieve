#include "detail/png.hpp"

#include <algorithm>
#include <array>
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

void writeRgbPng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                 std::span<const std::uint8_t> rgb) {
  const std::size_t row = std::size_t{width} * 3;
  if (rgb.size() != row * height || width == 0 || height == 0) {
    throw std::invalid_argument("Image size does not match its pixel data");
  }
  // Filter type 0 before every row.
  std::vector<std::uint8_t> raw;
  raw.reserve((row + 1) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    const auto line = rgb.subspan(y * row, row);
    raw.insert(raw.end(), line.begin(), line.end());
  }

  // zlib stream of stored deflate blocks (at most 65535 bytes each) plus Adler-32.
  std::vector<std::uint8_t> zlib{0x78, 0x01};
  constexpr std::size_t kMaxBlock = 65535;
  for (std::size_t offset = 0; offset < raw.size(); offset += kMaxBlock) {
    const std::size_t size = std::min(kMaxBlock, raw.size() - offset);
    zlib.push_back(offset + size == raw.size() ? 1 : 0);
    const auto len = static_cast<std::uint16_t>(size);
    const auto nlen = static_cast<std::uint16_t>(~len);
    zlib.insert(zlib.end(),
                {static_cast<std::uint8_t>(len & 0xFFU), static_cast<std::uint8_t>(len >> 8U),
                 static_cast<std::uint8_t>(nlen & 0xFFU), static_cast<std::uint8_t>(nlen >> 8U)});
    zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                raw.begin() + static_cast<std::ptrdiff_t>(offset + size));
  }
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (const std::uint8_t byte : raw) {
    a = (a + byte) % 65521U;
    b = (b + a) % 65521U;
  }
  appendBigEndian(zlib, (b << 16U) | a);

  std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<std::uint8_t> header;
  appendBigEndian(header, width);
  appendBigEndian(header, height);
  header.insert(header.end(), {8, 2, 0, 0, 0});  // 8 bit, RGB, deflate, no filter, no interlace
  appendChunk(png, "IHDR", header);
  appendChunk(png, "IDAT", zlib);
  appendChunk(png, "IEND", {});

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  if (!out) {
    throw std::runtime_error("Cannot write " + path.string());
  }
}

}  // namespace voxelsieve::detail
