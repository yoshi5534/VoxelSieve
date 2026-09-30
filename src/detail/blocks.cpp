#include "detail/blocks.hpp"

#include <array>
#include <deque>
#include <numeric>
#include <stdexcept>

namespace voxelsieve::detail {

ThresholdResult otsuThreshold(const Histogram& histogram) {
  const std::uint64_t total = std::accumulate(histogram.begin(), histogram.end(), std::uint64_t{0});
  if (total == 0) {
    throw std::invalid_argument("Cannot estimate a threshold for an empty volume");
  }

  double sum_all = 0.0;
  for (std::size_t value = 0; value < histogram.size(); ++value) {
    sum_all += static_cast<double>(value) * static_cast<double>(histogram[value]);
  }
  // Maximise the between-class variance; class "below" holds values <= t.
  double best_variance = -1.0;
  std::size_t best_value = 0;
  double weight_below = 0.0;
  double sum_below = 0.0;
  for (std::size_t value = 0; value + 1 < histogram.size(); ++value) {
    weight_below += static_cast<double>(histogram[value]);
    sum_below += static_cast<double>(value) * static_cast<double>(histogram[value]);
    const double weight_above = static_cast<double>(total) - weight_below;
    if (weight_below == 0.0 || weight_above == 0.0) {
      continue;
    }
    const double mean_below = sum_below / weight_below;
    const double mean_above = (sum_all - sum_below) / weight_above;
    const double variance =
        weight_below * weight_above * (mean_below - mean_above) * (mean_below - mean_above);
    if (variance > best_variance) {
      best_variance = variance;
      best_value = value;
    }
  }

  double air_weight = 0.0;
  double air_sum = 0.0;
  for (std::size_t value = 0; value <= best_value; ++value) {
    air_weight += static_cast<double>(histogram[value]);
    air_sum += static_cast<double>(value) * static_cast<double>(histogram[value]);
  }
  // Values <= best_value are air, so the separating grey value lies half a step above.
  return {static_cast<float>(static_cast<double>(best_value) + 0.5),
          static_cast<float>(air_weight > 0.0 ? air_sum / air_weight : 0.0)};
}

void floodFillOutsideAir(BlockGrid& blocks, const std::array<bool, 3>& open_axes) {
  std::deque<std::array<std::int64_t, 3>> queue;
  const auto visit = [&](std::int64_t bx, std::int64_t by, std::int64_t bz) {
    if (bx < 0 || by < 0 || bz < 0 || bx >= blocks.dims[0] || by >= blocks.dims[1] ||
        bz >= blocks.dims[2]) {
      return;
    }
    BlockState& state = blocks.states[blocks.index(bx, by, bz)];
    if (state == BlockState::kAir) {
      state = BlockState::kOutsideAir;
      queue.push_back({bx, by, bz});
    }
  };

  const auto& d = blocks.dims;
  if (open_axes[2]) {
    for (std::int64_t a = 0; a < d[0]; ++a) {
      for (std::int64_t b = 0; b < d[1]; ++b) {
        visit(a, b, 0);
        visit(a, b, d[2] - 1);
      }
    }
  }
  if (open_axes[1]) {
    for (std::int64_t a = 0; a < d[0]; ++a) {
      for (std::int64_t c = 0; c < d[2]; ++c) {
        visit(a, 0, c);
        visit(a, d[1] - 1, c);
      }
    }
  }
  if (open_axes[0]) {
    for (std::int64_t b = 0; b < d[1]; ++b) {
      for (std::int64_t c = 0; c < d[2]; ++c) {
        visit(0, b, c);
        visit(d[0] - 1, b, c);
      }
    }
  }

  while (!queue.empty()) {
    const auto [bx, by, bz] = queue.front();
    queue.pop_front();
    visit(bx - 1, by, bz);
    visit(bx + 1, by, bz);
    visit(bx, by - 1, bz);
    visit(bx, by + 1, bz);
    visit(bx, by, bz - 1);
    visit(bx, by, bz + 1);
  }
}

}  // namespace voxelsieve::detail
