#include "detail/blocks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <numeric>
#include <stdexcept>

namespace voxelsieve::detail {

namespace {

/// Mean grey value of the histogram entries below the threshold.
float meanBelow(const Histogram& histogram, float threshold) {
  double weight = 0.0;
  double sum = 0.0;
  for (std::size_t value = 0; value < histogram.size() && static_cast<float>(value) < threshold;
       ++value) {
    weight += static_cast<double>(histogram[value]);
    sum += static_cast<double>(value) * static_cast<double>(histogram[value]);
  }
  return static_cast<float>(weight > 0.0 ? sum / weight : 0.0);
}

}  // namespace

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

  // Values <= best_value are air, so the separating grey value lies half a step above.
  const auto threshold = static_cast<float>(static_cast<double>(best_value) + 0.5);
  return {threshold, meanBelow(histogram, threshold)};
}

ThresholdResult airThreshold(const Histogram& histogram) {
  const ThresholdResult otsu = otsuThreshold(histogram);

  // The lowest and highest grey value often collect clipped or masked voxels (outside the
  // reconstructed circle, say); such a spike is no material, so the search leaves them out.
  Histogram trimmed = histogram;
  trimmed.front() = 0;
  trimmed.back() = 0;
  const std::uint64_t total = std::accumulate(trimmed.begin(), trimmed.end(), std::uint64_t{0});
  if (total == 0) {
    return otsu;
  }
  // The search runs over 256 bins between the 0.1 and 99.9 % quantiles, smoothed, so that noise
  // in single grey values makes no peaks.
  std::size_t lo = 0;
  std::size_t hi = trimmed.size() - 1;
  for (std::uint64_t count = 0; count + trimmed[lo] <= total / 1000; ++lo) {
    count += trimmed[lo];
  }
  for (std::uint64_t count = 0; hi > lo && count + trimmed[hi] <= total / 1000; --hi) {
    count += trimmed[hi];
  }
  constexpr std::size_t kBins = 256;
  const std::size_t width = std::max<std::size_t>(1, (hi - lo + kBins) / kBins);
  const std::size_t bins = (hi - lo + width) / width;
  if (bins < 8) {
    return otsu;
  }
  std::vector<double> coarse(bins, 0.0);
  for (std::size_t value = lo; value <= hi; ++value) {
    coarse[(value - lo) / width] += static_cast<double>(trimmed[value]);
  }
  constexpr int kRadius = 5;
  constexpr double kSigma = 1.5;
  std::vector<double> smooth(bins, 0.0);
  for (std::size_t i = 0; i < bins; ++i) {
    double sum = 0.0;
    double weight_sum = 0.0;
    for (int k = -kRadius; k <= kRadius; ++k) {
      const auto j = static_cast<std::ptrdiff_t>(i) + k;
      if (j < 0 || j >= static_cast<std::ptrdiff_t>(bins)) {
        continue;
      }
      const double weight = std::exp(-0.5 * (k / kSigma) * (k / kSigma));
      sum += weight * coarse[static_cast<std::size_t>(j)];
      weight_sum += weight;
    }
    smooth[i] = sum / weight_sum;
  }
  const auto centre = [&](std::size_t bin) {
    return static_cast<double>(lo + bin * width) + (static_cast<double>(width) - 1.0) / 2.0;
  };

  std::vector<std::size_t> peaks;
  for (std::size_t i = 1; i + 1 < bins && centre(i) < otsu.threshold; ++i) {
    if (smooth[i] >= smooth[i - 1] && smooth[i] > smooth[i + 1]) {
      peaks.push_back(i);
    }
  }
  // A valley counts when it lies at least 5 % of the highest bin below both neighbouring peaks.
  const double depth = 0.05 * *std::max_element(smooth.begin(), smooth.end());
  if (peaks.empty()) {
    return otsu;
  }
  std::size_t air = peaks.front();
  for (std::size_t n = 1; n < peaks.size(); ++n) {
    const std::size_t next = peaks[n];
    const auto valley = static_cast<std::size_t>(
        std::min_element(smooth.begin() + static_cast<std::ptrdiff_t>(air),
                         smooth.begin() + static_cast<std::ptrdiff_t>(next) + 1) -
        smooth.begin());
    if (std::min(smooth[air], smooth[next]) - smooth[valley] >= depth) {
      const auto threshold = static_cast<float>(centre(valley));
      return {threshold, meanBelow(histogram, threshold)};
    }
    if (smooth[next] > smooth[air]) {
      air = next;  // no valley in between: one peak, the higher one stands for it
    }
  }
  return otsu;
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
