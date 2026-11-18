#pragma once

#include <boost/iostreams/device/mapped_file.hpp>
#include <cstddef>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

namespace voxelsieve::detail {

/// Runs `read(context)`, which reads from memory-mapped files, and returns false instead of
/// ending the process when reading the mapping fails: an I/O error of the device or a file cut
/// short while mapped (Windows: EXCEPTION_IN_PAGE_ERROR, elsewhere SIGBUS). On POSIX systems the
/// failed read is left by a jump, so `read` must not hold objects with destructors while it
/// touches the mapping.
[[nodiscard]] bool tryMappedRead(void (*read)(void*), void* context);

/// Throws std::runtime_error about a failed read of the mapped `file`.
[[noreturn]] void throwMappedReadError(const std::string& file);

/// `tryMappedRead` of a callable; throws std::runtime_error naming `file` when the read fails.
template <class Read>
void readMapped(const std::string& file, Read&& read) {
  using Callable = std::remove_reference_t<Read>;
  Callable* callable = &read;
  const auto call = [](void* context) { (*static_cast<Callable*>(context))(); };
  if (!tryMappedRead(call, const_cast<void*>(static_cast<const void*>(callable)))) {
    throwMappedReadError(file);
  }
}

/// Asks the system to read the pages of [data, data + size) of a file mapping ahead, for reads
/// that will follow soon. Only a hint; failure changes nothing.
void prefetchMappedPages(const void* data, std::size_t size);

/// Files mapped for reading, the most recently used kept open (at most `capacity`), shared by the
/// threads that read them; a file in use stays mapped while a reader holds it.
class MappedFilePool {
 public:
  using Mapped = std::shared_ptr<const boost::iostreams::mapped_file_source>;

  explicit MappedFilePool(std::size_t capacity = 1024) : capacity_(capacity) {}

  /// `file` mapped, under `key`.
  [[nodiscard]] Mapped get(std::size_t key, const std::filesystem::path& file) const {
    {
      const std::scoped_lock lock(mutex_);
      if (const auto it = mapped_.find(key); it != mapped_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.second);
        return it->second.first;
      }
    }
    auto opened = std::make_shared<const boost::iostreams::mapped_file_source>(file.string());
    Mapped evicted;  // unmapped outside the lock
    const std::scoped_lock lock(mutex_);
    if (const auto it = mapped_.find(key); it != mapped_.end()) {
      return it->second.first;  // mapped by another thread meanwhile
    }
    lru_.push_front(key);
    mapped_.emplace(key, std::pair{opened, lru_.begin()});
    if (mapped_.size() > capacity_) {
      const auto oldest = mapped_.find(lru_.back());
      evicted = std::move(oldest->second.first);
      mapped_.erase(oldest);
      lru_.pop_back();
    }
    return opened;
  }

  /// Unmaps the files no reader holds.
  void clear() const {
    std::map<std::size_t, std::pair<Mapped, std::list<std::size_t>::iterator>> released;
    const std::scoped_lock lock(mutex_);
    released.swap(mapped_);
    lru_.clear();
  }

 private:
  std::size_t capacity_;
  mutable std::mutex mutex_;
  mutable std::map<std::size_t, std::pair<Mapped, std::list<std::size_t>::iterator>> mapped_;
  mutable std::list<std::size_t> lru_;
};

}  // namespace voxelsieve::detail
