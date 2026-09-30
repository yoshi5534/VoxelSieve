#pragma once

// Read access to ZIP archives through libzip (ADR 0016), without extracting them.

#include <boost/iostreams/device/mapped_file.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct zip;

namespace voxelsieve::detail {

class ZipArchive {
 public:
  struct Entry {
    std::string name;  // path inside the archive, '/' separated
    std::uint64_t index = 0;
    std::uint64_t size = 0;
  };

  /// Reads the central directory; throws if `file` is not a ZIP archive.
  explicit ZipArchive(const std::filesystem::path& file);
  ZipArchive(const ZipArchive&) = delete;
  ZipArchive& operator=(const ZipArchive&) = delete;
  ZipArchive(ZipArchive&&) = delete;
  ZipArchive& operator=(ZipArchive&&) = delete;
  ~ZipArchive();

  [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

  /// Decompressed content of an entry, checked against its CRC. Thread-safe: every concurrent
  /// reader gets its own handle on the archive, so entries are inflated in parallel.
  [[nodiscard]] std::vector<std::uint8_t> read(const Entry& entry) const;

 private:
  struct Close {
    void operator()(zip* archive) const;
  };
  using Handle = std::unique_ptr<zip, Close>;

  [[nodiscard]] Handle open() const;

  std::filesystem::path path_;
  /// The archive, memory-mapped: libzip reads it from memory, so the operating system reads the
  /// file in large blocks instead of libzip's 4 KiB reads, each after a seek.
  boost::iostreams::mapped_file_source file_;
  std::vector<Entry> entries_;
  mutable std::mutex mutex_;
  mutable std::vector<Handle> idle_;  // open handles not in use
};

}  // namespace voxelsieve::detail
