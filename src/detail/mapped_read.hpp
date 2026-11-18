#pragma once

#include <cstddef>
#include <string>
#include <type_traits>

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

}  // namespace voxelsieve::detail
