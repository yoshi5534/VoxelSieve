#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace voxelsieve {

/// Physical memory this process may use, in bytes: the installed memory, or the memory limit of
/// the process's control group on Linux where that is lower. 0 when it cannot be read.
[[nodiscard]] std::uint64_t usableMemoryBytes();

/// The memory budget of the process (ADR 0034): what its brick caches and the work buffers of its
/// algorithms may use together. Default: three quarters of the usable memory, 4 GiB when that is
/// unknown.
[[nodiscard]] std::uint64_t memoryBudgetBytes();

/// Sets the memory budget; 0 restores the default. The shared brick cache takes its default share
/// of the new budget unless its size was set (`Dataset::setSharedCacheBytes`).
void setMemoryBudget(std::uint64_t bytes);

/// Memory for work buffers: the budget less the budgets of all brick caches (at least an eighth
/// of the budget), split evenly between the operations admitted at the moment
/// (`admitOperation`). Algorithms size the work they keep in flight from it.
[[nodiscard]] std::uint64_t workMemoryBytes();

/// How many tasks of `task_bytes` each fit into the work memory at once: at least 1, at most the
/// thread limit.
[[nodiscard]] std::size_t tasksInMemory(std::uint64_t task_bytes);

/// An operation's claim on the work memory, from `admitOperation`; released when destroyed.
class Admission {
 public:
  Admission() = default;
  Admission(const Admission&) = delete;
  Admission& operator=(const Admission&) = delete;
  Admission(Admission&& other) noexcept;
  Admission& operator=(Admission&& other) noexcept;
  ~Admission();

  /// Bytes claimed; 0 for an empty admission.
  [[nodiscard]] std::uint64_t bytes() const { return bytes_; }

 private:
  friend Admission admitOperation(std::uint64_t bytes, const std::atomic<bool>* cancel);
  explicit Admission(std::uint64_t bytes);
  void release();
  std::uint64_t bytes_ = 0;
  bool admitted_ = false;
};

/// Admits an operation that expects to need `bytes` of work memory. Returns at once when no other
/// operation is admitted or when the claims together fit into the work memory; otherwise waits
/// until they fit or the others are done. Each admitted operation gets an even share of the work
/// memory (`workMemoryBytes`). Throws std::runtime_error("Cancelled") once `*cancel` is set.
[[nodiscard]] Admission admitOperation(std::uint64_t bytes,
                                       const std::atomic<bool>* cancel = nullptr);

/// Operations admitted at the moment.
[[nodiscard]] int admittedOperations();

/// Limits the threads of all parallel work in the process (TBB) to `threads`; 0 lifts the limit.
/// Lasts until the next call. Fewer threads also mean fewer per-thread buffers and fewer bricks in
/// use at once.
void setThreadLimit(int threads);

/// The most threads parallel work of the process uses.
[[nodiscard]] int threadLimit();

/// The id of this process.
[[nodiscard]] std::uint64_t processId();

/// Whether a process with this id runs (or may run: true when the system does not say).
[[nodiscard]] bool processRunning(std::uint64_t id);

/// Gives memory freed in the heaps of all threads back to the system (glibc: malloc_trim, macOS:
/// pressure relief; nothing elsewhere). The brick cache calls it after evicting bricks of a quarter
/// of its budget, whose thousands of small leaf blocks would otherwise keep every thread's heap
/// near the size of the cache; passes that free large trees of small blocks call it at their end.
void releaseFreedMemory();

/// For command-line tools, which all take these options: applies `--memory <MB>` (the memory
/// budget, `setMemoryBudget`), `--cache <MB>` (budget of the shared brick cache, a part of it,
/// `Dataset::setSharedCacheBytes`) or `--threads <N>` (`setThreadLimit`). `value` reads the
/// option's value. Returns false for any other argument.
bool applyResourceOption(std::string_view arg, const std::function<std::string()>& value);

}  // namespace voxelsieve
