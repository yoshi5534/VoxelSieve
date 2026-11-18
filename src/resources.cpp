#include "voxelsieve/resources.hpp"

#include <tbb/global_control.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <fstream>
#endif
#endif

#include <tbb/task_arena.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "detail/memory.hpp"
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

std::atomic<std::uint64_t> g_memory_budget{0};  // 0: the default

/// Operations admitted, and the bytes they claimed.
std::mutex g_admission_mutex;
std::condition_variable g_admission_changed;
int g_admitted = 0;                  // guarded by g_admission_mutex
std::uint64_t g_admitted_bytes = 0;  // guarded by g_admission_mutex
std::atomic<int> g_admitted_count{0};

/// The budget less the caches, at least an eighth of it.
std::uint64_t workMemoryTotal() {
  const std::uint64_t budget = memoryBudgetBytes();
  const std::uint64_t caches = detail::cacheBudgetBytes();
  return std::max(caches < budget ? budget - caches : 0, budget / 8);
}

}  // namespace

std::uint64_t memoryBudgetBytes() {
  if (const std::uint64_t set = g_memory_budget.load(); set > 0) {
    return set;
  }
  const std::uint64_t memory = usableMemoryBytes();
  return memory > 0 ? memory / 4 * 3 : std::uint64_t{4} << 30U;
}

void setMemoryBudget(std::uint64_t bytes) {
  g_memory_budget = bytes;
  // A shared cache of the default size follows the budget.
  detail::sharedCacheFollowsBudget();
}

std::uint64_t workMemoryBytes() {
  return workMemoryTotal() / static_cast<std::uint64_t>(std::max(1, g_admitted_count.load()));
}

std::size_t tasksInMemory(std::uint64_t task_bytes) {
  const auto threads = static_cast<std::uint64_t>(
      std::max(1, std::min(tbb::this_task_arena::max_concurrency(), threadLimit())));
  return static_cast<std::size_t>(std::clamp<std::uint64_t>(
      workMemoryBytes() / std::max<std::uint64_t>(task_bytes, 1), 1, threads));
}

Admission::Admission(std::uint64_t bytes) : bytes_(bytes), admitted_(true) {}

Admission::Admission(Admission&& other) noexcept
    : bytes_(std::exchange(other.bytes_, 0)), admitted_(std::exchange(other.admitted_, false)) {}

Admission& Admission::operator=(Admission&& other) noexcept {
  if (this != &other) {
    release();
    bytes_ = std::exchange(other.bytes_, 0);
    admitted_ = std::exchange(other.admitted_, false);
  }
  return *this;
}

Admission::~Admission() { release(); }

void Admission::release() {
  if (!admitted_) {
    return;
  }
  {
    const std::scoped_lock lock(g_admission_mutex);
    --g_admitted;
    g_admitted_bytes -= bytes_;
    g_admitted_count = g_admitted;
  }
  g_admission_changed.notify_all();
  admitted_ = false;
  bytes_ = 0;
}

Admission admitOperation(std::uint64_t bytes, const std::atomic<bool>* cancel) {
  std::unique_lock lock(g_admission_mutex);
  while (g_admitted > 0 && g_admitted_bytes + bytes > workMemoryTotal()) {
    if (cancel != nullptr && cancel->load()) {
      throw std::runtime_error("Cancelled");
    }
    // Woken when an operation is done; the timeout looks at `cancel` again.
    g_admission_changed.wait_for(lock, std::chrono::milliseconds(100));
  }
  ++g_admitted;
  g_admitted_bytes += bytes;
  g_admitted_count = g_admitted;
  // Constructed under the lock, so the counts and the admission agree.
  return Admission(bytes);
}

int admittedOperations() { return g_admitted_count.load(); }

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
  if (arg == "--memory") {
    setMemoryBudget(static_cast<std::uint64_t>(std::stoull(value())) << 20U);
    return true;
  }
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
