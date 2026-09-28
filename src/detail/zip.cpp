#include "detail/zip.hpp"

#include <algorithm>
#include <array>
#include <boost/crc.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace voxelsieve::detail {
namespace {

constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50;
constexpr std::uint32_t kZip64Locator = 0x07064b50;
constexpr std::uint32_t kZip64End = 0x06064b50;
constexpr std::uint32_t kCentralHeader = 0x02014b50;
constexpr std::uint32_t kLocalHeader = 0x04034b50;
constexpr std::uint16_t kZip64Extra = 0x0001;
constexpr std::uint32_t kMax32 = std::numeric_limits<std::uint32_t>::max();

template <typename T>
T little(const std::uint8_t* p) {
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value = static_cast<T>(value | (static_cast<T>(p[i]) << (8U * i)));
  }
  return value;
}

}  // namespace

ZipArchive::ZipArchive(const std::filesystem::path& file)
    : path_(file), in_(file, std::ios::binary) {
  if (!in_) {
    throw std::runtime_error("Cannot open " + file.string());
  }
  file_size_ = std::filesystem::file_size(file);
  // The end record is at most 22 bytes plus a comment of up to 64 KiB from the end.
  const std::uint64_t tail_size = std::min<std::uint64_t>(file_size_, 22 + 65535);
  std::vector<std::uint8_t> tail(tail_size);
  readAt(file_size_ - tail_size, tail.data(), tail.size());
  std::int64_t end = -1;
  for (std::int64_t i = static_cast<std::int64_t>(tail.size()) - 22; i >= 0; --i) {
    if (little<std::uint32_t>(&tail[static_cast<std::size_t>(i)]) == kEndOfCentralDirectory) {
      end = i;
      break;
    }
  }
  if (end < 0) {
    throw std::runtime_error("Not a ZIP archive: " + file.string());
  }
  const std::uint8_t* e = &tail[static_cast<std::size_t>(end)];
  std::uint64_t count = little<std::uint16_t>(e + 10);
  std::uint64_t directory_size = little<std::uint32_t>(e + 12);
  std::uint64_t directory_offset = little<std::uint32_t>(e + 16);
  const std::uint64_t end_offset = file_size_ - tail_size + static_cast<std::uint64_t>(end);
  if (end_offset >= 20) {
    std::array<std::uint8_t, 20> locator{};
    readAt(end_offset - 20, locator.data(), locator.size());
    if (little<std::uint32_t>(locator.data()) == kZip64Locator) {
      std::array<std::uint8_t, 56> end64{};
      readAt(little<std::uint64_t>(locator.data() + 8), end64.data(), end64.size());
      if (little<std::uint32_t>(end64.data()) != kZip64End) {
        throw std::runtime_error("Broken ZIP64 end record in " + file.string());
      }
      count = little<std::uint64_t>(end64.data() + 32);
      directory_size = little<std::uint64_t>(end64.data() + 40);
      directory_offset = little<std::uint64_t>(end64.data() + 48);
    }
  }
  if (directory_offset + directory_size > file_size_) {
    throw std::runtime_error("Broken central directory in " + file.string());
  }
  std::vector<std::uint8_t> directory(directory_size);
  readAt(directory_offset, directory.data(), directory.size());
  std::size_t pos = 0;
  entries_.reserve(count);
  for (std::uint64_t i = 0; i < count; ++i) {
    if (pos + 46 > directory.size() || little<std::uint32_t>(&directory[pos]) != kCentralHeader) {
      throw std::runtime_error("Broken central directory in " + file.string());
    }
    const std::uint8_t* h = &directory[pos];
    Entry entry;
    entry.method = little<std::uint16_t>(h + 10);
    entry.crc32 = little<std::uint32_t>(h + 16);
    entry.compressed_size = little<std::uint32_t>(h + 20);
    entry.size = little<std::uint32_t>(h + 24);
    const std::size_t name_length = little<std::uint16_t>(h + 28);
    const std::size_t extra_length = little<std::uint16_t>(h + 30);
    const std::size_t comment_length = little<std::uint16_t>(h + 32);
    entry.local_header_offset = little<std::uint32_t>(h + 42);
    if (pos + 46 + name_length + extra_length + comment_length > directory.size()) {
      throw std::runtime_error("Broken central directory in " + file.string());
    }
    entry.name.assign(reinterpret_cast<const char*>(h + 46), name_length);
    // ZIP64: the fields that overflowed follow in this order in the extra field.
    const std::uint8_t* extra = h + 46 + name_length;
    for (std::size_t x = 0; x + 4 <= extra_length;) {
      const auto id = little<std::uint16_t>(extra + x);
      const std::size_t length = little<std::uint16_t>(extra + x + 2);
      if (id == kZip64Extra) {
        std::size_t field = x + 4;
        for (std::uint64_t* value :
             {&entry.size, &entry.compressed_size, &entry.local_header_offset}) {
          if (*value == kMax32 && field + 8 <= x + 4 + length) {
            *value = little<std::uint64_t>(extra + field);
            field += 8;
          }
        }
      }
      x += 4 + length;
    }
    entries_.push_back(std::move(entry));
    pos += 46 + name_length + extra_length + comment_length;
  }
}

void ZipArchive::readAt(std::uint64_t offset, void* out, std::size_t size) const {
  const std::lock_guard lock(mutex_);
  in_.clear();
  in_.seekg(static_cast<std::streamoff>(offset));
  in_.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
  if (!in_) {
    throw std::runtime_error("Cannot read " + path_.string());
  }
}

std::vector<std::uint8_t> ZipArchive::read(const Entry& entry) const {
  std::array<std::uint8_t, 30> local{};
  readAt(entry.local_header_offset, local.data(), local.size());
  if (little<std::uint32_t>(local.data()) != kLocalHeader) {
    throw std::runtime_error("Broken entry " + entry.name + " in " + path_.string());
  }
  const std::uint64_t data_offset = entry.local_header_offset + 30 +
                                    little<std::uint16_t>(local.data() + 26) +
                                    little<std::uint16_t>(local.data() + 28);
  if (data_offset + entry.compressed_size > file_size_) {
    throw std::runtime_error("Truncated entry " + entry.name + " in " + path_.string());
  }
  std::vector<std::uint8_t> compressed(entry.compressed_size);
  readAt(data_offset, compressed.data(), compressed.size());
  std::vector<std::uint8_t> data;
  if (entry.method == 0) {
    data = std::move(compressed);
  } else if (entry.method == 8) {
    namespace io = boost::iostreams;
    io::zlib_params params;
    params.noheader = true;  // raw deflate
    io::filtering_istream stream;
    stream.push(io::zlib_decompressor(params));
    stream.push(
        io::array_source(reinterpret_cast<const char*>(compressed.data()), compressed.size()));
    data.resize(entry.size);
    stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (static_cast<std::uint64_t>(stream.gcount()) != entry.size) {
      throw std::runtime_error("Cannot inflate " + entry.name + " in " + path_.string());
    }
  } else {
    throw std::runtime_error("Unsupported compression method " + std::to_string(entry.method) +
                             " of " + entry.name + " (only stored and deflate)");
  }
  if (data.size() != entry.size) {
    throw std::runtime_error("Wrong size of " + entry.name + " in " + path_.string());
  }
  boost::crc_32_type crc;
  crc.process_bytes(data.data(), data.size());
  if (crc.checksum() != entry.crc32) {
    throw std::runtime_error("CRC mismatch in " + entry.name + " of " + path_.string());
  }
  return data;
}

}  // namespace voxelsieve::detail
