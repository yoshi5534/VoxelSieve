#include "detail/zip.hpp"

#include <zip.h>

#include <stdexcept>

namespace voxelsieve::detail {

void ZipArchive::Close::operator()(zip* archive) const { zip_discard(archive); }

ZipArchive::Handle ZipArchive::open() const {
  zip_error_t error;
  zip_error_init(&error);
  zip_source_t* source = zip_source_buffer_create(file_.data(), file_.size(), 0, &error);
  zip_t* archive = source == nullptr
                       ? nullptr
                       : zip_open_from_source(source, ZIP_RDONLY | ZIP_CHECKCONS, &error);
  if (archive == nullptr) {
    zip_source_free(source);  // the archive owns it once open
    const std::string message = zip_error_strerror(&error);
    zip_error_fini(&error);
    throw std::runtime_error("Not a readable ZIP archive: " + path_.string() + " (" + message +
                             ")");
  }
  zip_error_fini(&error);
  return Handle(archive);
}

ZipArchive::ZipArchive(const std::filesystem::path& file) : path_(file) {
  if (!std::filesystem::is_regular_file(file)) {
    throw std::runtime_error("Cannot open " + file.string());
  }
  if (std::filesystem::file_size(file) == 0) {
    throw std::runtime_error("Not a readable ZIP archive: " + file.string() + " (empty file)");
  }
  file_.open(file.string());
  if (!file_.is_open()) {
    throw std::runtime_error("Cannot map " + file.string());
  }
  Handle archive = open();
  const zip_int64_t count = zip_get_num_entries(archive.get(), 0);
  entries_.reserve(static_cast<std::size_t>(std::max<zip_int64_t>(count, 0)));
  for (zip_int64_t i = 0; i < count; ++i) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(archive.get(), static_cast<zip_uint64_t>(i), 0, &stat) != 0 ||
        (stat.valid & (ZIP_STAT_NAME | ZIP_STAT_SIZE)) != (ZIP_STAT_NAME | ZIP_STAT_SIZE)) {
      throw std::runtime_error("Broken central directory in " + file.string());
    }
    entries_.push_back({stat.name, static_cast<std::uint64_t>(i), stat.size});
  }
  idle_.push_back(std::move(archive));
}

ZipArchive::~ZipArchive() = default;

std::vector<std::uint8_t> ZipArchive::read(const Entry& entry) const {
  Handle archive;
  {
    const std::lock_guard lock(mutex_);
    if (!idle_.empty()) {
      archive = std::move(idle_.back());
      idle_.pop_back();
    }
  }
  if (!archive) {
    archive = open();
  }
  std::vector<std::uint8_t> data(entry.size);
  {
    zip_file_t* file = zip_fopen_index(archive.get(), entry.index, 0);
    if (file == nullptr) {
      throw std::runtime_error("Cannot read " + entry.name + " in " + path_.string() + ": " +
                               zip_strerror(archive.get()));
    }
    std::uint64_t done = 0;
    while (done < data.size()) {
      const zip_int64_t n = zip_fread(file, data.data() + done, data.size() - done);
      if (n <= 0) {
        break;
      }
      done += static_cast<std::uint64_t>(n);
    }
    // Reading past the end makes libzip compare the CRC.
    std::uint8_t extra = 0;
    const zip_int64_t tail = done == data.size() ? zip_fread(file, &extra, 1) : -1;
    const std::string message = zip_file_strerror(file);
    zip_fclose(file);
    if (done != data.size() || tail != 0) {
      throw std::runtime_error("Cannot inflate " + entry.name + " in " + path_.string() + ": " +
                               message);
    }
  }
  const std::lock_guard lock(mutex_);
  idle_.push_back(std::move(archive));
  return data;
}

}  // namespace voxelsieve::detail
