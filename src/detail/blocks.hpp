#pragma once

// Internal building blocks shared by the in-memory sieve and the streaming dataset writer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace voxelsieve::detail {

inline constexpr std::size_t kHistogramBins = 65536;
using Histogram = std::vector<std::uint64_t>;

struct ThresholdResult {
  float threshold = 0.0F;
  /// Mean grey value of the histogram entries below the threshold.
  float air_level = 0.0F;
};

/// Otsu's threshold for a 16-bit grey-value histogram. Throws if the histogram is empty.
[[nodiscard]] ThresholdResult otsuThreshold(const Histogram& histogram);

/// Threshold between air and the least dense material. Otsu's split alone lands between the two
/// largest classes, which in a scan of several materials can lie above a whole material (a
/// plastic pipe, organic fillings among stones), so that material would count as air. The air
/// peak is the lowest clear peak of the smoothed histogram below Otsu's split; when a clear
/// valley separates it from the next peak, the threshold lies in that valley, otherwise it is
/// Otsu's. Throws if the histogram is empty.
[[nodiscard]] ThresholdResult airThreshold(const Histogram& histogram);

enum class BlockState : std::uint8_t { kAir, kMaterial, kOutsideAir };

/// One state per 8^3 block, x fastest.
struct BlockGrid {
  std::array<std::int64_t, 3> dims{};
  std::vector<BlockState> states;

  [[nodiscard]] std::size_t index(std::int64_t bx, std::int64_t by, std::int64_t bz) const {
    return static_cast<std::size_t>(bx + dims[0] * (by + dims[1] * bz));
  }
  [[nodiscard]] bool contains(std::int64_t bx, std::int64_t by, std::int64_t bz) const {
    return bx >= 0 && by >= 0 && bz >= 0 && bx < dims[0] && by < dims[1] && bz < dims[2];
  }
  /// True for blocks the sieve keeps: material and internal air.
  [[nodiscard]] bool kept(std::int64_t bx, std::int64_t by, std::int64_t bz) const {
    return states[index(bx, by, bz)] != BlockState::kOutsideAir;
  }
};

[[nodiscard]] inline std::int64_t ceilDiv(std::int64_t a, std::int64_t b) {
  return (a + b - 1) / b;
}

/// Marks air blocks that are face-connected to the volume boundary as outside air. Outside air
/// enters only through the two boundary faces of each axis in `open_axes` (x, y, z).
void floodFillOutsideAir(BlockGrid& blocks,
                         const std::array<bool, 3>& open_axes = {true, true, true});

}  // namespace voxelsieve::detail
