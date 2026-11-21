// Read access to a cell at full resolution, planes and profiles.

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <utility>

#include "detail/battery.hpp"

namespace voxelsieve::detail::battery {

/// The entry of the ascending `planes` nearest to `index`.
std::int64_t nearestPlane(const std::vector<std::int64_t>& planes, std::int64_t index) {
  const auto it = std::lower_bound(planes.begin(), planes.end(), index);
  if (it == planes.end()) {
    return planes.back();
  }
  if (it != planes.begin() && index - *std::prev(it) < *it - index) {
    return *std::prev(it);
  }
  return *it;
}

VolumeView viewOf(const Dataset& dataset) {
  const DatasetInfo& info = dataset.info();
  return VolumeView({info.dims, info.voxel_size, info.air_level, info.threshold, {}},
                    [&dataset](const Box& box, std::span<float> out, float fill) {
                      dataset.readRegion(0, box, out, fill);
                    });
}

std::pair<int, int> planeAxes(int axis) {
  return axis == 0 ? std::pair{1, 2} : (axis == 1 ? std::pair{0, 2} : std::pair{0, 1});
}

/// Reads a plane into `plane`, reusing its memory.
void readPlaneInto(Plane& plane, const VolumeView& volume, int axis, std::int64_t index,
                   const Index3& lo, const Index3& hi, float fill) {
  const auto [u, v] = planeAxes(axis);
  plane.u_axis = u;
  plane.v_axis = v;
  plane.u0 = lo[at(u)];
  plane.v0 = lo[at(v)];
  plane.nu = hi[at(u)] - lo[at(u)];
  plane.nv = hi[at(v)] - lo[at(v)];
  const VoxelSize& pitch = volume.info().voxel_size;
  plane.pu = pitch[at(u)];
  plane.pv = pitch[at(v)];
  plane.fill = fill;
  Box box{lo, hi};
  box.min[at(axis)] = index;
  box.max[at(axis)] = index + 1;
  plane.values.resize(at(box.voxelCount()));
  volume.readRegion(box, plane.values, fill);
}

Plane readPlane(const VolumeView& volume, int axis, std::int64_t index, const Index3& lo,
                const Index3& hi, float fill) {
  Plane plane;
  readPlaneInto(plane, volume, axis, index, lo, hi, fill);
  return plane;
}

/// Radial profile in `plane` from `center` along `angle`, `count` samples of `step` mm from
/// radius 0, averaged over 2 * average + 1 parallel rays one step apart. `noise` receives the
/// noise of the averaged profile from the spread between the rays, which see the same layers.
std::vector<double> rayProfile(const Plane& plane, const std::array<double, 2>& center,
                               double angle, double step, std::int64_t count, int average,
                               double& noise) {
  const double c = std::cos(angle);
  const double s = std::sin(angle);
  const auto n = static_cast<double>(2 * average + 1);
  std::vector<double> profile(at(count), 0.0);
  std::vector<double> spread(at(count), 0.0);
  for (std::int64_t i = 0; i < count; ++i) {
    const auto r = static_cast<double>(i) * step;
    double sum = 0.0;
    double sum_sq = 0.0;
    for (int t = -average; t <= average; ++t) {
      const double off = t * step;
      const double v = plane.sample(center[0] + r * c - off * s, center[1] + r * s + off * c);
      sum += v;
      sum_sq += v * v;
    }
    profile[at(i)] = sum / n;
    spread[at(i)] = std::sqrt(std::max(0.0, sum_sq / n - profile[at(i)] * profile[at(i)]));
  }
  noise = average > 0 ? median(spread) * std::sqrt(n / (n - 1.0)) / std::sqrt(n) : 0.0;
  return profile;
}

std::vector<double> smooth3(std::span<const double> p) {
  std::vector<double> out(p.begin(), p.end());
  for (std::size_t i = 1; i + 1 < p.size(); ++i) {
    out[i] = 0.25 * p[i - 1] + 0.5 * p[i] + 0.25 * p[i + 1];
  }
  return out;
}

double quantile(std::span<const double> p, double q) {
  std::vector<double> v(p.begin(), p.end());
  if (v.empty()) {
    return kNaN;
  }
  const auto k = static_cast<std::ptrdiff_t>(q * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + k, v.end());
  return v[static_cast<std::size_t>(k)];
}

/// Layers along a profile within [from, to): maxima of the smoothed profile with at least
/// `min_prominence`, located at the centroid of their upper half.
std::vector<Peak> findPeaks(std::span<const double> profile, std::size_t from, std::size_t to,
                            double min_prominence) {
  std::vector<Peak> peaks;
  to = std::min(to, profile.size());
  if (to < from + 3) {
    return peaks;
  }
  const std::vector<double> p = smooth3(profile.subspan(from, to - from));
  const std::size_t n = p.size();
  std::size_t i = 1;
  while (i + 1 < n) {
    if (p[i] <= p[i - 1]) {
      ++i;
      continue;
    }
    // A plateau of equal values counts as one maximum at its middle.
    std::size_t j = i;
    while (j + 1 < n && p[j + 1] == p[i]) {
      ++j;
    }
    if (j + 1 >= n || p[j + 1] > p[i]) {
      i = j + 1;
      continue;
    }
    double left = p[i];
    for (std::size_t k = i; k-- > 0;) {
      if (p[k] > p[i]) {
        break;
      }
      left = std::min(left, p[k]);
    }
    double right = p[i];
    for (std::size_t k = j + 1; k < n; ++k) {
      if (p[k] > p[i]) {
        break;
      }
      right = std::min(right, p[k]);
    }
    const double prominence = p[i] - std::max(left, right);
    if (prominence >= min_prominence) {
      const double half = p[i] - 0.5 * prominence;
      std::size_t a = i;
      while (a > 0 && p[a - 1] > half) {
        --a;
      }
      std::size_t b = j;
      while (b + 1 < n && p[b + 1] > half) {
        ++b;
      }
      double sum = 0.0;
      double weight = 0.0;
      for (std::size_t k = a; k <= b; ++k) {
        sum += static_cast<double>(k) * (p[k] - half);
        weight += p[k] - half;
      }
      const double centre = weight > 0.0 ? sum / weight : 0.5 * static_cast<double>(i + j);
      peaks.push_back({static_cast<double>(from) + centre, p[i], prominence, false});
    }
    i = j + 1;
  }
  return peaks;
}

/// Smallest prominence of a layer in a profile with `noise`: well above the noise and a small
/// share of the contrast.
double minProminence(std::span<const double> profile, double noise) {
  const double contrast = quantile(profile, 0.95) - quantile(profile, 0.05);
  return std::max(4.0 * 0.61 * noise, 0.08 * contrast);  // 0.61: after the 1-2-1 smoothing
}

/// Mean of the minima between neighbouring peaks: the level of the gaps between the layers.
void collectValleys(std::span<const double> profile, const std::vector<Peak>& peaks,
                    std::vector<double>& valleys) {
  for (std::size_t k = 0; k + 1 < peaks.size(); ++k) {
    const auto a = static_cast<std::size_t>(std::ceil(peaks[k].position));
    const auto b = static_cast<std::size_t>(std::floor(peaks[k + 1].position));
    double low = std::numeric_limits<double>::infinity();
    for (std::size_t i = a; i <= b && i < profile.size(); ++i) {
      low = std::min(low, profile[i]);
    }
    if (std::isfinite(low)) {
      valleys.push_back(low);
    }
  }
}

Classes classify(std::vector<double> values) {
  Classes c;
  if (values.size() < 4) {
    return c;
  }
  std::sort(values.begin(), values.end());
  const std::size_t n = values.size();
  std::vector<double> prefix(n + 1, 0.0);
  std::vector<double> prefix_sq(n + 1, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    prefix[i + 1] = prefix[i] + values[i];
    prefix_sq[i + 1] = prefix_sq[i] + values[i] * values[i];
  }
  double best = std::numeric_limits<double>::infinity();
  std::size_t best_k = n / 2;
  for (std::size_t k = 1; k < n; ++k) {
    const auto n1 = static_cast<double>(k);
    const auto n2 = static_cast<double>(n - k);
    const double ss1 = prefix_sq[k] - prefix[k] * prefix[k] / n1;
    const double ss2 =
        (prefix_sq[n] - prefix_sq[k]) - (prefix[n] - prefix[k]) * (prefix[n] - prefix[k]) / n2;
    if (ss1 + ss2 < best) {
      best = ss1 + ss2;
      best_k = k;
    }
  }
  const auto n1 = static_cast<double>(best_k);
  const auto n2 = static_cast<double>(n - best_k);
  c.low = prefix[best_k] / n1;
  c.high = (prefix[n] - prefix[best_k]) / n2;
  c.split = 0.5 * (c.low + c.high);
  const double pooled = std::sqrt(best / static_cast<double>(n));
  const double share = std::min(n1, n2) / static_cast<double>(n);
  c.distinct = share >= 0.15 && c.high - c.low >= 3.0 * pooled && c.high - c.low >= 0.1 * c.high;
  if (!c.distinct) {
    c.low = c.high = prefix[n] / static_cast<double>(n);
    c.split = -std::numeric_limits<double>::infinity();
  }
  return c;
}

void label(std::vector<Peak>& peaks, const Classes& classes, bool cathode_brighter) {
  for (Peak& peak : peaks) {
    const bool bright = peak.value >= classes.split;
    peak.cathode = classes.distinct ? bright == cathode_brighter : true;
  }
}

Band firstBand(std::span<const double> p, double threshold, double air, std::size_t window) {
  Band band;
  std::size_t o = 0;
  while (o < p.size() && p[o] < threshold) {
    ++o;
  }
  if (o >= p.size()) {
    return band;
  }
  const std::vector<double> s = smooth3(p);
  std::size_t m = o;
  // The first maximum that is the largest within two samples.
  while (m + 1 < s.size()) {
    bool local = true;
    for (std::size_t k = m > 2 ? m - 2 : 0; k <= std::min(s.size() - 1, m + 2); ++k) {
      local = local && s[k] <= s[m];
    }
    if (local) {
      break;
    }
    ++m;
  }
  band.peak = s[m];
  const double outer_level = 0.5 * (band.peak + air);
  std::size_t a = m;
  while (a > 0 && s[a - 1] >= outer_level) {
    --a;
  }
  band.outer = a == 0 ? 0.0
                      : static_cast<double>(a - 1) +
                            (outer_level - s[a - 1]) / std::max(1e-9, s[a] - s[a - 1]);
  double low = band.peak;
  for (std::size_t k = m; k < std::min(s.size(), m + window); ++k) {
    low = std::min(low, s[k]);
  }
  if (band.peak - low < 0.2 * (band.peak - air)) {
    return band;
  }
  const double inner_level = 0.5 * (band.peak + low);
  std::size_t b = m;
  while (b + 1 < s.size() && s[b + 1] >= inner_level) {
    ++b;
  }
  if (b + 1 < s.size()) {
    band.inner = static_cast<double>(b) + (s[b] - inner_level) / std::max(1e-9, s[b] - s[b + 1]);
  }
  return band;
}

/// Least-squares circle through points (Kåsa): centre and radius.
std::pair<std::array<double, 2>, double> fitCircle(const std::vector<std::array<double, 2>>& pts) {
  // Solve [sum xx, xy, x; xy, yy, y; x, y, n] [D E F] = -[sum x(xx+yy), y(xx+yy), xx+yy].
  std::array<double, 9> a{};
  std::array<double, 3> b{};
  for (const auto& q : pts) {
    const double x = q[0];
    const double y = q[1];
    const double r2 = x * x + y * y;
    const std::array<double, 3> row{x, y, 1.0};
    for (std::size_t i = 0; i < 3; ++i) {
      for (std::size_t j = 0; j < 3; ++j) {
        a[3 * i + j] += row[i] * row[j];
      }
      b[i] -= row[i] * r2;
    }
  }
  // Gaussian elimination with partial pivoting.
  std::array<std::size_t, 3> order{0, 1, 2};
  for (std::size_t col = 0; col < 3; ++col) {
    std::size_t pivot = col;
    for (std::size_t r = col + 1; r < 3; ++r) {
      if (std::abs(a[3 * order[r] + col]) > std::abs(a[3 * order[pivot] + col])) {
        pivot = r;
      }
    }
    std::swap(order[col], order[pivot]);
    const double d = a[3 * order[col] + col];
    if (std::abs(d) < 1e-300) {
      return {{kNaN, kNaN}, kNaN};
    }
    for (std::size_t r = col + 1; r < 3; ++r) {
      const double f = a[3 * order[r] + col] / d;
      for (std::size_t c = col; c < 3; ++c) {
        a[3 * order[r] + c] -= f * a[3 * order[col] + c];
      }
      b[order[r]] -= f * b[order[col]];
    }
  }
  std::array<double, 3> x{};
  for (std::size_t col = 3; col-- > 0;) {
    double sum = b[order[col]];
    for (std::size_t c = col + 1; c < 3; ++c) {
      sum -= a[3 * order[col] + c] * x[c];
    }
    x[col] = sum / a[3 * order[col] + col];
  }
  const std::array<double, 2> centre{-0.5 * x[0], -0.5 * x[1]};
  const double r2 = centre[0] * centre[0] + centre[1] * centre[1] - x[2];
  return {centre, r2 > 0.0 ? std::sqrt(r2) : kNaN};
}

}  // namespace voxelsieve::detail::battery
