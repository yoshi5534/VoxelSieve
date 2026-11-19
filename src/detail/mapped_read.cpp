#include "detail/mapped_read.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>

#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <mutex>
#endif

#include <stdexcept>

namespace voxelsieve::detail {

#if defined(_WIN32)

namespace {

int inPageError(unsigned long code) {
  return code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

bool tryMappedRead(void (*read)(void*), void* context) {
  // No C++ objects in this frame: structured exception handling cannot unwind them.
  __try {
    read(context);
  } __except (inPageError(GetExceptionCode())) {
    return false;
  }
  return true;
}

void prefetchMappedPages(const void* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return;
  }
  WIN32_MEMORY_RANGE_ENTRY range{const_cast<void*>(data), size};
  (void)PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
}

#else

namespace {

/// Where a failed read of this thread jumps to; null outside tryMappedRead.
thread_local sigjmp_buf* t_jump = nullptr;
struct sigaction g_previous {};

void onBusError(int number, siginfo_t* info, void* ucontext) {
  if (t_jump != nullptr) {
    siglongjmp(*t_jump, 1);
  }
  // Not a guarded read: what the process did before.
  if ((g_previous.sa_flags & SA_SIGINFO) != 0 && g_previous.sa_sigaction != nullptr) {
    g_previous.sa_sigaction(number, info, ucontext);
    return;
  }
  if (g_previous.sa_handler != SIG_DFL && g_previous.sa_handler != SIG_IGN &&
      g_previous.sa_handler != nullptr) {
    g_previous.sa_handler(number);
    return;
  }
  ::signal(number, SIG_DFL);
  ::raise(number);
}

void installHandler() {
  static std::once_flag once;
  std::call_once(once, [] {
    struct sigaction action {};
    action.sa_sigaction = onBusError;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGBUS, &action, &g_previous);
  });
}

}  // namespace

// GCC warns that the arguments might be clobbered by the jump; they are not used after it.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclobbered"
#endif
bool tryMappedRead(void (*read)(void*), void* context) {
  installHandler();
  sigjmp_buf jump;
  sigjmp_buf* const outer = t_jump;
  if (sigsetjmp(jump, 1) != 0) {
    t_jump = outer;
    return false;
  }
  t_jump = &jump;
  try {
    read(context);
  } catch (...) {
    t_jump = outer;  // an exception of `read` leaves this frame as well
    throw;
  }
  t_jump = outer;
  return true;
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

void prefetchMappedPages(const void* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return;
  }
  // madvise needs a page-aligned start.
  const auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
  const auto begin = reinterpret_cast<std::uintptr_t>(data) & ~(page - 1);
  const auto end = reinterpret_cast<std::uintptr_t>(data) + size;
  (void)madvise(reinterpret_cast<void*>(begin), end - begin, MADV_WILLNEED);  // NOLINT
}

#endif

void throwMappedReadError(const std::string& file) {
  throw std::runtime_error("Cannot read " + file +
                           ": the device reported an error, or the file was cut short while "
                           "it was read");
}

}  // namespace voxelsieve::detail
