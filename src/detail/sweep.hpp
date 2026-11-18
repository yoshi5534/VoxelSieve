#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace voxelsieve::detail {

/// Position of a brick (or tile) on a Morton (Z-order) curve: bricks close on the curve are close
/// in space. Indices up to 2^21 per axis.
[[nodiscard]] std::uint64_t mortonKey(const std::array<std::uint64_t, 3>& index);
[[nodiscard]] std::uint64_t mortonKey(const std::array<std::int64_t, 3>& index);

/// Sorts bricks along the Morton curve.
void mortonSort(std::vector<std::array<std::int64_t, 3>>& bricks);

/// Runs `task(i)` for i = 0 … count − 1, started in that order, with at most `in_flight` running
/// at once (ADR 0031). Each task runs isolated: a thread that waits inside it for nested parallel
/// work does not start another task, so the memory in use stays that of `in_flight` tasks.
/// Exceptions end the run and are rethrown.
void runInFlight(std::size_t count, std::size_t in_flight,
                 const std::function<void(std::size_t)>& task);

/// Work buffers that tasks take and give back, so a buffer is allocated once per task in flight
/// instead of once per task: fresh memory costs page faults that can take longer than the work.
template <class Work>
class WorkPool {
 public:
  using Deleter = std::function<void(Work*)>;
  using Handle = std::unique_ptr<Work, Deleter>;

  /// A buffer, given back when the handle is destroyed.
  [[nodiscard]] Handle take() {
    std::unique_ptr<Work> work;
    {
      const std::scoped_lock lock(mutex_);
      if (!free_.empty()) {
        work = std::move(free_.back());
        free_.pop_back();
      }
    }
    if (!work) {
      work = std::make_unique<Work>();
    }
    return Handle(work.release(), [this](Work* returned) {
      const std::scoped_lock lock(mutex_);
      free_.emplace_back(returned);
    });
  }

 private:
  std::mutex mutex_;
  std::vector<std::unique_ptr<Work>> free_;
};

}  // namespace voxelsieve::detail
