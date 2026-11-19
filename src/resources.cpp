#include "voxelsieve/resources.hpp"

#include <tbb/global_control.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>

#include <cerrno>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <fstream>
#endif
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "voxelsieve/dataset.hpp"

namespace voxelsieve {
namespace {

#if defined(__linux__)
/// The memory limit of the process's control group (cgroup v2), or 0 when there is none.
std::uint64_t cgroupMemoryLimit() {
  std::ifstream in("/sys/fs/cgroup/memory.max");
  std::string text;
  if (!(in >> text) || text == "max") {
    return 0;
  }
  try {
    return std::stoull(text);
  } catch (const std::exception&) {
    return 0;
  }
}
#endif

std::mutex g_threads_mutex;
std::unique_ptr<tbb::global_control> g_threads;  // guarded by g_threads_mutex

}  // namespace

std::uint64_t usableMemoryBytes() {
#if defined(_WIN32)
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  return GlobalMemoryStatusEx(&status) != 0 ? status.ullTotalPhys : 0;
#elif defined(__APPLE__)
  std::uint64_t total = 0;
  std::size_t size = sizeof(total);
  return sysctlbyname("hw.memsize", &total, &size, nullptr, 0) == 0 ? total : 0;
#else
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long page_size = sysconf(_SC_PAGESIZE);
  std::uint64_t total = pages > 0 && page_size > 0 ? static_cast<std::uint64_t>(pages) *
                                                         static_cast<std::uint64_t>(page_size)
                                                   : 0;
  if (const std::uint64_t limit = cgroupMemoryLimit(); limit > 0 && (total == 0 || limit < total)) {
    total = limit;
  }
  return total;
#endif
}

void setThreadLimit(int threads) {
  if (threads < 0) {
    throw std::invalid_argument("The thread limit must not be negative");
  }
  const std::scoped_lock lock(g_threads_mutex);
  g_threads.reset();
  if (threads > 0) {
    g_threads = std::make_unique<tbb::global_control>(tbb::global_control::max_allowed_parallelism,
                                                      static_cast<std::size_t>(threads));
  }
}

int threadLimit() {
  return static_cast<int>(
      tbb::global_control::active_value(tbb::global_control::max_allowed_parallelism));
}

std::uint64_t processId() {
#if defined(_WIN32)
  return GetCurrentProcessId();
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

bool processRunning(std::uint64_t id) {
#if defined(_WIN32)
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(id));
  if (process == nullptr) {
    // No such process; any other error (such as no access) means it exists.
    return GetLastError() != ERROR_INVALID_PARAMETER;
  }
  DWORD code = 0;
  const bool running = GetExitCodeProcess(process, &code) == 0 || code == STILL_ACTIVE;
  CloseHandle(process);
  return running;
#else
  return kill(static_cast<pid_t>(id), 0) == 0 || errno != ESRCH;
#endif
}

bool applyResourceOption(std::string_view arg, const std::function<std::string()>& value) {
  if (arg == "--cache") {
    Dataset::setSharedCacheBytes(static_cast<std::size_t>(std::stoull(value())) << 20U);
    return true;
  }
  if (arg == "--threads") {
    setThreadLimit(std::stoi(value()));
    return true;
  }
  return false;
}

}  // namespace voxelsieve
