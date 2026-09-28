#pragma once

// Read access to ZIP archives (stored and deflated entries, ZIP64) without extracting them.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace voxelsieve::detail {

class ZipArchive {
 public:
  struct Entry {
    std::string name;  // path inside the archive, '/' separated
    std::uint16_t method = 0;
    std::uint32_t crc32 = 0;
    std::uint64_t compressed_size = 0;
    std::uint64_t size = 0;
    std::uint64_t local_header_offset = 0;
  };

  /// Reads the central directory; throws if `file` is not a ZIP archive.
  explicit ZipArchive(const std::filesystem::path& file);

  [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

  /// Decompressed content of an entry, checked against its CRC. Thread-safe.
  [[nodiscard]] std::vector<std::uint8_t> read(const Entry& entry) const;

 private:
  void readAt(std::uint64_t offset, void* out, std::size_t size) const;

  std::filesystem::path path_;
  std::uint64_t file_size_ = 0;
  mutable std::ifstream in_;
  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
};

}  // namespace voxelsieve::detail
