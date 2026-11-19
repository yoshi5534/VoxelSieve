#include "detail/sweep.hpp"

#include <tbb/parallel_pipeline.h>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>

#include <algorithm>
#include <tuple>

namespace voxelsieve::detail {

std::uint64_t mortonKey(const std::array<std::uint64_t, 3>& index) {
  std::uint64_t key = 0;
  for (std::uint64_t bit = 0; bit < 21; ++bit) {
    for (std::uint64_t axis = 0; axis < 3; ++axis) {
      key |= ((index[axis] >> bit) & 1U) << (3 * bit + axis);
    }
  }
  return key;
}

std::uint64_t mortonKey(const std::array<std::int64_t, 3>& index) {
  return mortonKey(std::array<std::uint64_t, 3>{static_cast<std::uint64_t>(index[0]),
                                                static_cast<std::uint64_t>(index[1]),
                                                static_cast<std::uint64_t>(index[2])});
}

void mortonSort(std::vector<std::array<std::int64_t, 3>>& bricks) {
  std::vector<std::pair<std::uint64_t, std::array<std::int64_t, 3>>> keyed;
  keyed.reserve(bricks.size());
  for (const auto& brick : bricks) {
    keyed.emplace_back(mortonKey(brick), brick);
  }
  std::sort(keyed.begin(), keyed.end());
  for (std::size_t i = 0; i < keyed.size(); ++i) {
    bricks[i] = keyed[i].second;
  }
}

void runInFlight(std::size_t count, std::size_t in_flight,
                 const std::function<void(std::size_t)>& task, const ProgressToken& progress) {
  if (count == 0) {
    return;
  }
  ProgressSteps steps(progress, count);
  std::size_t next = 0;
  tbb::parallel_pipeline(
      std::max<std::size_t>(in_flight, 1),
      tbb::make_filter<void, std::size_t>(tbb::filter_mode::serial_in_order,
                                          [&](tbb::flow_control& control) -> std::size_t {
                                            if (next == count) {
                                              control.stop();
                                              return 0;
                                            }
                                            return next++;
                                          }) &
          tbb::make_filter<std::size_t, void>(tbb::filter_mode::parallel, [&](std::size_t i) {
            steps.step(0);  // cancelled: no further task starts
            tbb::task_group_context context(tbb::task_group_context::isolated);
            tbb::task_group group(context);
            tbb::this_task_arena::isolate([&] { group.run_and_wait([&] { task(i); }); });
            steps.step();
          }));
}

void runIsolated(const std::function<void()>& task) {
  tbb::task_group_context context(tbb::task_group_context::isolated);
  tbb::task_group group(context);
  tbb::this_task_arena::isolate([&] { group.run_and_wait([&] { task(); }); });
}

}  // namespace voxelsieve::detail
