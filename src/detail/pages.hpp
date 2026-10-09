#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace voxelsieve::detail {

/// Lets the pages of a file mapping go from the process's memory. Pages of a read-only mapping
/// are read from the file again when needed, written pages of a shared mapping are kept by the
/// file cache and written back; either way the data stays as it is. Only a hint; failure changes
/// nothing.
inline void releaseMappedPages(const void* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return;
  }
#if defined(_WIN32)
  // Unlocking pages that were never locked removes them from the working set.
  (void)VirtualUnlock(const_cast<void*>(data), size);
#else
  // madvise needs a page-aligned start.
  const auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
  const auto begin = reinterpret_cast<std::uintptr_t>(data) & ~(page - 1);
  const auto end = reinterpret_cast<std::uintptr_t>(data) + size;
  (void)madvise(reinterpret_cast<void*>(begin), end - begin, MADV_DONTNEED);  // NOLINT
#endif
}

}  // namespace voxelsieve::detail
