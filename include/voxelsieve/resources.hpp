#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace voxelsieve {

/// Physical memory this process may use, in bytes: the installed memory, or the memory limit of
/// the process's control group on Linux where that is lower. 0 when it cannot be read.
[[nodiscard]] std::uint64_t usableMemoryBytes();

/// Limits the threads of all parallel work in the process (TBB) to `threads`; 0 lifts the limit.
/// Lasts until the next call. Fewer threads also mean fewer per-thread buffers and fewer bricks in
/// use at once.
void setThreadLimit(int threads);

/// The most threads parallel work of the process uses.
[[nodiscard]] int threadLimit();

/// For command-line tools, which all take these options: applies `--cache <MB>` (budget of the
/// shared brick cache, `Dataset::setSharedCacheBytes`) or `--threads <N>` (`setThreadLimit`).
/// `value` reads the option's value. Returns false for any other argument.
bool applyResourceOption(std::string_view arg, const std::function<std::string()>& value);

}  // namespace voxelsieve
