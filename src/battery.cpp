#include "voxelsieve/battery.hpp"

#include <tbb/blocked_range.h>
#include <tbb/combinable.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <numbers>
#include <numeric>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include "detail/blocks.hpp"
#include "detail/png.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {
namespace {

using Index3 = std::array<std::int64_t, 3>;
using Vec3 = std::array<double, 3>;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kPi = std::numbers::pi;
/// Bright or dark voxels collected in the electrode stack at most; more means the threshold
/// catches the layers themselves.
constexpr std::size_t kMaxFindingVoxels = 20'000'000;

std::size_t at(std::int64_t i) { return static_cast<std::size_t>(i); }

double median(std::vector<double> values) {
  values.erase(std::remove_if(values.begin(), values.end(), [](double v) { return std::isnan(v); }),
               values.end());
  if (values.empty()) {
    return kNaN;
  }
  const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), mid, values.end());
  return *mid;
}

int medianInt(const std::vector<int>& values) {
  if (values.empty()) {
    return 0;
  }
  const std::vector<double> v(values.begin(), values.end());
  return static_cast<int>(std::lround(median(v)));
}

double nanMin(double a, double b) {
  if (std::isnan(a)) {
    return b;
  }
  return std::isnan(b) ? a : std::min(a, b);
}

double nanMax(double a, double b) {
  if (std::isnan(a)) {
    return b;
  }
  return std::isnan(b) ? a : std::max(a, b);
}

// ---------------------------------------------------------------------------------------------
// Read access at full resolution: a dataset, or a volume made of images.

struct VolumeInfo {
  Index3 dims{};
  VoxelSize voxel_size;
  float air_level = 0.0F;
  float threshold = 0.0F;
  /// Indices along z of the planes that hold measured values, when not all do (cross-sections
  /// given as images; the others repeat the nearest one). Empty: all.
  std::vector<std::int64_t> planes;
};

/// Level 0 of a volume: the regions the analysis reads, with `fill` outside the volume.
class VolumeView {
 public:
  using Reader = std::function<void(const Box&, std::span<float>, float)>;
  VolumeView(VolumeInfo info, Reader reader) : info_(std::move(info)), reader_(std::move(reader)) {}

  [[nodiscard]] const VolumeInfo& info() const { return info_; }
  void readRegion(const Box& box, std::span<float> out, float fill) const {
    reader_(box, out, fill);
  }

 private:
  VolumeInfo info_;
  Reader reader_;
};

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

// ---------------------------------------------------------------------------------------------
// Planes and profiles.

/// A slice of level 0 normal to `axis`, over a range of the two other axes (u < v).
struct Plane {
  int u_axis = 0;
  int v_axis = 1;
  std::int64_t u0 = 0;
  std::int64_t v0 = 0;
  std::int64_t nu = 0;
  std::int64_t nv = 0;
  double pu = 1.0;  // pitch in mm
  double pv = 1.0;
  float fill = 0.0F;
  std::vector<float> values;  // u fastest

  [[nodiscard]] float voxel(std::int64_t i, std::int64_t j) const {
    if (i < 0 || j < 0 || i >= nu || j >= nv) {
      return fill;
    }
    return values[at(i + nu * j)];
  }

  /// Bilinear value at (u, v) in mm (voxel index times pitch).
  [[nodiscard]] double sample(double u_mm, double v_mm) const {
    const double fu = u_mm / pu - static_cast<double>(u0);
    const double fv = v_mm / pv - static_cast<double>(v0);
    const double iu = std::floor(fu);
    const double iv = std::floor(fv);
    const double tu = fu - iu;
    const double tv = fv - iv;
    const auto i = static_cast<std::int64_t>(iu);
    const auto j = static_cast<std::int64_t>(iv);
    return (1.0 - tv) * ((1.0 - tu) * voxel(i, j) + tu * voxel(i + 1, j)) +
           tv * ((1.0 - tu) * voxel(i, j + 1) + tu * voxel(i + 1, j + 1));
  }
};

std::pair<int, int> planeAxes(int axis) {
  return axis == 0 ? std::pair{1, 2} : (axis == 1 ? std::pair{0, 2} : std::pair{0, 1});
}

Plane readPlane(const VolumeView& volume, int axis, std::int64_t index, const Index3& lo,
                const Index3& hi, float fill) {
  const auto [u, v] = planeAxes(axis);
  Plane plane;
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

struct Peak {
  double position = 0.0;  // sample index, sub-sample
  double value = 0.0;     // smoothed peak value
  double prominence = 0.0;
  bool cathode = false;
};

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

/// Two classes of layer brightness by the split of least variance.
struct Classes {
  double split = 0.0;
  double low = 0.0;
  double high = 0.0;
  bool distinct = false;
};

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

/// The first bright band from the start of a profile (a wall seen from outside): its outer and
/// inner edge at half height, in samples, and its peak. NaN inner edge when nothing darker
/// follows it.
struct Band {
  double outer = kNaN;
  double inner = kNaN;
  double peak = kNaN;
};

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

// ---------------------------------------------------------------------------------------------
// Sections across an edge and the layer tracks in them.

/// Grey values of a section across an edge: `across` samples across the layers (radius, or
/// position along the stack normal) by `along` samples towards the edge, row by row from the
/// inside outwards, across fastest.
struct SectionImage {
  std::int64_t across = 0;
  std::int64_t along = 0;
  double across_from_mm = 0.0;
  double across_step_mm = 1.0;
  double along_from_mm = 0.0;  // position along the axis of row 0
  double along_step_mm = 1.0;  // signed: towards the edge
  std::vector<float> values;
  /// Rows are valid up to this across sample (the wall of a cylindrical can).
  std::int64_t valid_across = 0;

  [[nodiscard]] double value(std::int64_t a, std::int64_t s) const {
    return values[at(a + across * s)];
  }
};

struct Track {
  double start = 0.0;  // across sample at row 0
  bool cathode = false;
  bool ended = false;
  double end = 0.0;  // row, sub-sample
  double across_end = 0.0;
  std::vector<std::int64_t> path;  // across sample per row
  std::vector<double> along;       // grey value on the track per row, smoothed
};

std::vector<Track> trackLayers(const SectionImage& image, const Classes& classes,
                               bool cathode_brighter, double gap_level) {
  std::vector<Track> tracks;
  if (image.along < 4 || image.valid_across < 4) {
    return tracks;
  }
  std::vector<double> start(at(image.valid_across), 0.0);
  const std::int64_t rows = std::min<std::int64_t>(4, image.along);
  for (std::int64_t s = 0; s < rows; ++s) {
    for (std::int64_t a = 0; a < image.valid_across; ++a) {
      start[at(a)] += image.value(a, s) / static_cast<double>(rows);
    }
  }
  // The rows see the same layers: their differences are noise.
  std::vector<double> diff;
  for (std::int64_t s = 0; s + 1 < rows; ++s) {
    for (std::int64_t a = 0; a < image.valid_across; ++a) {
      diff.push_back(image.value(a, s) - image.value(a, s + 1));
    }
  }
  const double m = median(diff);
  for (double& d : diff) {
    d = std::abs(d - m);
  }
  const double noise = diff.empty() ? 0.0
                                    : 1.4826 * median(diff) / std::numbers::sqrt2 /
                                          std::sqrt(static_cast<double>(rows));
  std::vector<Peak> peaks = findPeaks(start, 1, start.size() - 1, minProminence(start, noise));
  label(peaks, classes, cathode_brighter);
  std::vector<double> along(at(image.along));
  for (const Peak& peak : peaks) {
    Track track;
    track.start = peak.position;
    track.cathode = peak.cathode;
    const double end_level = 0.5 * (peak.value + gap_level);
    auto pos = static_cast<std::int64_t>(std::lround(peak.position));
    for (std::int64_t s = 0; s < image.along; ++s) {
      std::int64_t best = pos;
      double best_value = -std::numeric_limits<double>::infinity();
      for (std::int64_t a = std::max<std::int64_t>(0, pos - 1);
           a <= std::min(image.valid_across - 1, pos + 1); ++a) {
        if (image.value(a, s) > best_value) {
          best_value = image.value(a, s);
          best = a;
        }
      }
      along[at(s)] = best_value;
      if (best_value >= end_level) {
        pos = best;  // follows the layer by at most one sample per row
      }
      track.path.push_back(pos);
    }
    std::vector<double> smoothed = smooth3(along);
    for (std::int64_t s = 1; s + 1 < image.along; ++s) {
      if (smoothed[at(s)] < end_level && smoothed[at(s + 1)] < end_level) {
        const double before = smoothed[at(s - 1)];
        const double frac = (before - end_level) / std::max(1e-9, before - smoothed[at(s)]);
        track.ended = true;
        track.end = static_cast<double>(s - 1) + std::clamp(frac, 0.0, 1.0);
        break;
      }
    }
    track.across_end = static_cast<double>(pos);
    track.along = std::move(smoothed);
    tracks.push_back(std::move(track));
  }
  return tracks;
}

/// Overhang samples of the cathode tracks of one section.
void overhangOfSection(const SectionImage& image, const std::vector<Track>& tracks, int edge,
                       double section, std::vector<OverhangSample>& samples, bool& unfinished) {
  // Neighbours are anode layers at the usual distance; a layer much farther away belongs to
  // another part of the winding, as at the start and end of the strip.
  std::vector<double> spacing;
  for (std::size_t t = 1; t < tracks.size(); ++t) {
    spacing.push_back(tracks[t].start - tracks[t - 1].start);
  }
  const double usual = median(spacing);
  int layer = 0;
  for (std::size_t t = 0; t < tracks.size(); ++t) {
    if (!tracks[t].cathode) {
      continue;
    }
    const int this_layer = layer++;
    if (!tracks[t].ended) {
      unfinished = true;
      continue;
    }
    double overhang = std::numeric_limits<double>::infinity();
    double anode_end = kNaN;
    double anode_across = kNaN;
    for (const std::size_t n : {t - 1, t + 1}) {
      if (n >= tracks.size() || tracks[n].cathode ||
          std::abs(std::abs(tracks[n].start - tracks[t].start) - usual) > 0.4 * usual) {
        continue;  // t - 1 wraps for t == 0
      }
      if (!tracks[n].ended) {
        unfinished = true;
        continue;
      }
      const double o = (tracks[n].end - tracks[t].end) * std::abs(image.along_step_mm);
      if (o < overhang) {
        overhang = o;
        anode_end = tracks[n].end;
        anode_across = tracks[n].across_end;
      }
    }
    if (!std::isfinite(overhang)) {
      continue;
    }
    OverhangSample sample;
    sample.edge = edge;
    sample.section = section;
    sample.layer = this_layer;
    sample.across_mm = image.across_from_mm + tracks[t].start * image.across_step_mm;
    sample.cathode_end_mm = image.along_from_mm + tracks[t].end * image.along_step_mm;
    sample.anode_end_mm = image.along_from_mm + anode_end * image.along_step_mm;
    sample.anode_across_mm = image.across_from_mm + anode_across * image.across_step_mm;
    sample.overhang_mm = overhang;
    samples.push_back(sample);
  }
}

/// Overhang samples where only the cathode layers stand out in the winding, as with hard carbon
/// or graphite anodes between metal oxide cathodes in a reconstruction with little contrast:
/// every track is a cathode, and the anode is looked for in the middle between two neighbouring
/// cathodes. Beyond the cathode ends the anode stands between darker electrolyte, and it ends
/// where its grey value falls half way to that of the space beyond the cathode ends. Where the
/// anode does not stand out there by `min_contrast`, as when the space beyond the cathodes is
/// filled with other material, the pair is left out and `hidden` is set.
void overhangBetweenCathodes(const SectionImage& image, const std::vector<Track>& tracks, int edge,
                             double section, double min_contrast,
                             std::vector<OverhangSample>& samples, bool& unfinished, bool& hidden) {
  std::vector<double> spacing;
  for (std::size_t t = 1; t < tracks.size(); ++t) {
    spacing.push_back(tracks[t].start - tracks[t - 1].start);
  }
  const double usual = median(spacing);
  if (!(usual > 2.0)) {
    return;
  }
  // Windows in rows, relative to the layer pitch so that they mean the same at any resolution.
  const auto half_width = std::max<std::int64_t>(1, std::lround(0.12 * usual));
  const auto smoothing = std::max<std::int64_t>(1, std::lround(0.17 * usual));
  const auto run = std::max<std::int64_t>(2, std::lround(0.2 * usual));
  const auto probe = std::max<std::int64_t>(3, std::lround(0.3 * usual));
  // Anode end per pair of neighbouring cathodes, in rows; NaN where not found.
  std::vector<double> anode_end(tracks.size(), kNaN);
  std::vector<double> anode_across(tracks.size(), kNaN);
  int hidden_pairs = 0;
  int seen_pairs = 0;
  for (std::size_t t = 0; t + 1 < tracks.size(); ++t) {
    const Track& a = tracks[t];
    const Track& b = tracks[t + 1];
    if (std::abs((b.start - a.start) - usual) > 0.4 * usual) {
      continue;  // not neighbours in the winding
    }
    if (!a.ended || !b.ended) {
      unfinished = true;
      continue;
    }
    std::vector<double> mid(at(image.along), 0.0);
    for (std::int64_t s = 0; s < image.along; ++s) {
      const std::int64_t c = (a.path[at(s)] + b.path[at(s)]) / 2;
      double sum = 0.0;
      double count = 0.0;
      for (std::int64_t k = std::max<std::int64_t>(0, c - half_width);
           k <= std::min(image.valid_across - 1, c + half_width); ++k) {
        sum += image.value(k, s);
        count += 1.0;
      }
      mid[at(s)] = count > 0.0 ? sum / count : kNaN;
    }
    std::vector<double> smoothed(mid.size(), kNaN);
    for (std::int64_t s = 0; s < image.along; ++s) {
      double sum = 0.0;
      double count = 0.0;
      for (std::int64_t k = std::max<std::int64_t>(0, s - smoothing);
           k <= std::min(image.along - 1, s + smoothing); ++k) {
        sum += mid[at(k)];
        count += 1.0;
      }
      smoothed[at(s)] = sum / count;
    }
    const auto first = static_cast<std::int64_t>(std::ceil(std::min(a.end, b.end)));
    const auto last = static_cast<std::int64_t>(std::ceil(std::max(a.end, b.end)));
    std::vector<double> anode_level;
    std::vector<double> dark;
    for (std::int64_t s = last + 1; s <= last + probe && s < image.along; ++s) {
      anode_level.push_back(smoothed[at(s)]);
    }
    for (const Track* track : {&a, &b}) {
      const auto end = static_cast<std::int64_t>(std::ceil(track->end));
      for (std::int64_t s = end + 2; s <= end + 1 + probe && s < image.along; ++s) {
        dark.push_back(track->along[at(s)]);
      }
    }
    const double level = median(anode_level);
    const double beyond = median(dark);
    if (!(level - beyond >= min_contrast)) {
      ++hidden_pairs;
      continue;
    }
    ++seen_pairs;
    const double end_level = 0.5 * (level + beyond);
    for (std::int64_t s = std::max<std::int64_t>(first, 1); s + run < image.along; ++s) {
      bool below = true;
      for (std::int64_t k = s; k < s + run && below; ++k) {
        below = smoothed[at(k)] < end_level;
      }
      if (below) {
        const double before = smoothed[at(s - 1)];
        const double frac = (before - end_level) / std::max(1e-9, before - smoothed[at(s)]);
        anode_end[t] = static_cast<double>(s - 1) + std::clamp(frac, 0.0, 1.0);
        anode_across[t] = 0.5 * static_cast<double>(a.path[at(s)] + b.path[at(s)]);
        break;
      }
    }
    if (std::isnan(anode_end[t])) {
      unfinished = true;
    }
  }
  // Hidden where the anode does not stand out between most pairs.
  hidden = hidden_pairs > seen_pairs;
  int layer = 0;
  for (std::size_t t = 0; t < tracks.size(); ++t) {
    const int this_layer = layer++;
    if (!tracks[t].ended) {
      continue;
    }
    double overhang = std::numeric_limits<double>::infinity();
    std::size_t pair = tracks.size();
    for (const std::size_t n : {t - 1, t}) {
      if (n < tracks.size() && !std::isnan(anode_end[n])) {  // t - 1 wraps for t == 0
        const double o = (anode_end[n] - tracks[t].end) * std::abs(image.along_step_mm);
        if (o < overhang) {
          overhang = o;
          pair = n;
        }
      }
    }
    if (pair == tracks.size()) {
      continue;
    }
    OverhangSample sample;
    sample.edge = edge;
    sample.section = section;
    sample.layer = this_layer;
    sample.across_mm = image.across_from_mm + tracks[t].start * image.across_step_mm;
    sample.cathode_end_mm = image.along_from_mm + tracks[t].end * image.along_step_mm;
    sample.anode_end_mm = image.along_from_mm + anode_end[pair] * image.along_step_mm;
    sample.anode_across_mm = image.across_from_mm + anode_across[pair] * image.across_step_mm;
    sample.overhang_mm = overhang;
    samples.push_back(sample);
  }
}

/// Overhang samples of one section across an edge, with the anode as its own layers where the
/// electrodes differ in brightness, else between the cathodes.
void sectionOverhang(const SectionImage& image, const BatteryLevels& levels,
                     const BatteryOptions& options, int edge, double section,
                     std::vector<OverhangSample>& samples, bool& unfinished, bool& hidden) {
  Classes classes;
  classes.distinct = levels.electrodes_distinct;
  classes.split = 0.5 * (levels.anode + levels.cathode);
  classes.low = std::min(levels.anode, levels.cathode);
  classes.high = std::max(levels.anode, levels.cathode);
  const std::vector<Track> tracks =
      trackLayers(image, classes, options.cathode_brighter, levels.gap);
  if (levels.electrodes_distinct) {
    overhangOfSection(image, tracks, edge, section, samples, unfinished);
  } else {
    overhangBetweenCathodes(image, tracks, edge, section, 0.12 * (levels.cathode - levels.gap),
                            samples, unfinished, hidden);
  }
}

/// Leaves out the layer ends of one edge that lie far from the others: a layer lost by the
/// tracking, a tab or the start of a strip, not the edge of the electrode stack. Returns how many.
std::int64_t dropStrayEnds(std::vector<std::vector<OverhangSample>>& per_section, double limit) {
  std::vector<double> cathode;
  std::vector<double> anode;
  for (const auto& samples : per_section) {
    for (const OverhangSample& s : samples) {
      cathode.push_back(s.cathode_end_mm);
      anode.push_back(s.anode_end_mm);
    }
  }
  if (cathode.empty()) {
    return 0;
  }
  const double cathode_median = median(cathode);
  const double anode_median = median(anode);
  std::int64_t dropped = 0;
  for (auto& samples : per_section) {
    const auto stray = [&](const OverhangSample& s) {
      return std::abs(s.cathode_end_mm - cathode_median) > limit ||
             std::abs(s.anode_end_mm - anode_median) > limit;
    };
    const auto before = samples.size();
    samples.erase(std::remove_if(samples.begin(), samples.end(), stray), samples.end());
    dropped += static_cast<std::int64_t>(before - samples.size());
  }
  return dropped;
}

void warnStrayEnds(BatteryResult& result, int edge, std::int64_t dropped, double limit) {
  if (dropped > 0) {
    std::ostringstream text;
    text << "At " << result.edges[at(edge)].name << ", " << dropped << " layer ends more than "
         << limit << " mm from the edge of the electrode stack (lost layers, tabs) are left out";
    result.warnings.push_back(text.str());
  }
}

/// Warnings about layer ends that were not measured.
void warnOverhang(BatteryResult& result, bool unfinished, const std::vector<int>& hidden_edges) {
  if (unfinished) {
    result.warnings.emplace_back(
        "Some electrode layers did not end inside the analysed sections, or their neighbours "
        "did not; their overhang is left out");
  }
  for (const int edge : hidden_edges) {
    result.warnings.push_back("At " + result.edges[at(edge)].name +
                              " the anode does not stand out from the material beyond the "
                              "cathode ends in most sections; the overhang there is not measured");
  }
}

void summarizeEdges(BatteryResult& result) {
  for (std::size_t e = 0; e < result.edges.size(); ++e) {
    BatteryEdge& edge = result.edges[e];
    double sum = 0.0;
    double sum_sq = 0.0;
    double cmin = kNaN;
    double cmax = kNaN;
    double amin = kNaN;
    double amax = kNaN;
    double anode_sum = 0.0;
    double cathode_sum = 0.0;
    edge.min_mm = kNaN;
    edge.max_mm = kNaN;
    for (std::size_t i = 0; i < result.overhang.size(); ++i) {
      const OverhangSample& s = result.overhang[i];
      if (s.edge != static_cast<int>(e)) {
        continue;
      }
      ++edge.samples;
      sum += s.overhang_mm;
      sum_sq += s.overhang_mm * s.overhang_mm;
      if (std::isnan(edge.min_mm) || s.overhang_mm < edge.min_mm) {
        edge.min_mm = s.overhang_mm;
        edge.worst = static_cast<std::int64_t>(i);
      }
      edge.max_mm = nanMax(edge.max_mm, s.overhang_mm);
      cmin = nanMin(cmin, s.cathode_end_mm);
      cmax = nanMax(cmax, s.cathode_end_mm);
      amin = nanMin(amin, s.anode_end_mm);
      amax = nanMax(amax, s.anode_end_mm);
      anode_sum += s.anode_end_mm;
      cathode_sum += s.cathode_end_mm;
    }
    if (edge.samples > 0) {
      const auto n = static_cast<double>(edge.samples);
      edge.mean_mm = sum / n;
      edge.std_mm = std::sqrt(std::max(0.0, sum_sq / n - edge.mean_mm * edge.mean_mm));
      edge.cathode_edge_spread_mm = cmax - cmin;
      edge.anode_edge_spread_mm = amax - amin;
      edge.anode_edge_mm = anode_sum / n;
      edge.cathode_edge_mm = cathode_sum / n;
    } else {
      edge.mean_mm = edge.std_mm = kNaN;
      edge.anode_edge_mm = edge.cathode_edge_mm = kNaN;
    }
    result.min_overhang_mm = nanMin(result.min_overhang_mm, edge.min_mm);
  }
}

std::string edgeName(int axis, int side) {
  return std::string(1, "xyz"[axis]) + (side == 0 ? " min" : " max");
}

// ---------------------------------------------------------------------------------------------
// Overview: bounding box, cell type and axis from a coarse level.

struct Overview {
  Index3 lo{};  // level-0 voxels, inclusive
  Index3 hi{};  // exclusive
  CellType type = CellType::kCylindrical;
  int axis = 2;
};

Overview overview(const Dataset& dataset, const BatteryOptions& options,
                  std::vector<std::string>& warnings) {
  const DatasetInfo& info = dataset.info();
  // A level of at least 4³ voxels per voxel, so that noise in the air does not reach the
  // threshold, and at most 256 voxels along any axis.
  int level = 0;
  for (const LevelInfo& l : info.levels) {
    level = l.level;
    if (l.level >= 2 && *std::max_element(l.dims.begin(), l.dims.end()) <= 256) {
      break;
    }
  }
  const Index3 dims = dataset.level(level).dims;
  std::vector<float> values(at(dims[0] * dims[1] * dims[2]));
  dataset.readRegion(level, Box{{0, 0, 0}, dims}, values, info.air_level);
  const auto idx = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
    return at(x + dims[0] * (y + dims[1] * z));
  };
  // Bounding box from the material per slice along each axis, ignoring slices with a few
  // stray voxels.
  std::array<std::vector<std::int64_t>, 3> counts;
  for (std::size_t a = 0; a < 3; ++a) {
    counts[a].assign(at(dims[a]), 0);
  }
  for (std::int64_t z = 0; z < dims[2]; ++z) {
    for (std::int64_t y = 0; y < dims[1]; ++y) {
      for (std::int64_t x = 0; x < dims[0]; ++x) {
        if (values[idx(x, y, z)] >= info.threshold) {
          ++counts[0][at(x)];
          ++counts[1][at(y)];
          ++counts[2][at(z)];
        }
      }
    }
  }
  Index3 lo{dims};
  Index3 hi{0, 0, 0};
  for (std::size_t a = 0; a < 3; ++a) {
    const std::int64_t most = *std::max_element(counts[a].begin(), counts[a].end());
    const std::int64_t enough = std::max<std::int64_t>(2, most / 50);
    for (std::int64_t i = 0; i < dims[a]; ++i) {
      if (counts[a][at(i)] >= enough) {
        lo[a] = std::min(lo[a], i);
        hi[a] = std::max(hi[a], i + 1);
      }
    }
  }
  if (hi[0] <= lo[0]) {
    throw std::invalid_argument("The dataset holds no material above the air threshold");
  }
  const std::int64_t scale = std::int64_t{1} << static_cast<unsigned>(level);
  Overview o;
  for (std::size_t a = 0; a < 3; ++a) {
    o.lo[a] = std::max<std::int64_t>(0, lo[a] * scale - scale);
    o.hi[a] = std::min(info.dims[a], hi[a] * scale + scale);
  }
  // Round cross-section: the filled share of the bounding box of the middle slice is pi / 4.
  int round_axis = -1;
  double round_error = 1.0;
  for (int c = 0; c < 3; ++c) {
    const auto [u, v] = planeAxes(c);
    const std::int64_t mid = (lo[at(c)] + hi[at(c)]) / 2;
    std::int64_t area = 0;
    for (std::int64_t j = lo[at(v)]; j < hi[at(v)]; ++j) {
      std::int64_t first = -1;
      std::int64_t last = -1;
      for (std::int64_t i = lo[at(u)]; i < hi[at(u)]; ++i) {
        Index3 p{};
        p[at(c)] = mid;
        p[at(u)] = i;
        p[at(v)] = j;
        Index3 q = p;
        q[at(u)] = std::min(i + 1, hi[at(u)] - 1);
        if (values[idx(p[0], p[1], p[2])] >= info.threshold &&
            values[idx(q[0], q[1], q[2])] >= info.threshold) {
          first = first < 0 ? i : first;
          last = i + 1;
        }
      }
      area += first < 0 ? 0 : last - first + 1;
    }
    const auto wu = static_cast<double>(hi[at(u)] - lo[at(u)]) * info.voxel_size[at(u)];
    const auto wv = static_cast<double>(hi[at(v)] - lo[at(v)]) * info.voxel_size[at(v)];
    const auto fill = static_cast<double>(area) /
                      static_cast<double>((hi[at(u)] - lo[at(u)]) * (hi[at(v)] - lo[at(v)]));
    const double aspect = wu / wv;
    const double error = std::abs(fill - kPi / 4.0);
    if (aspect > 0.8 && aspect < 1.25 && error < 0.09 && error < round_error) {
      round_error = error;
      round_axis = c;
    }
  }
  o.type =
      options.cell_type.value_or(round_axis >= 0 ? CellType::kCylindrical : CellType::kStacked);
  if (options.axis) {
    o.axis = *options.axis;
  } else if (o.type == CellType::kCylindrical) {
    if (round_axis < 0) {
      // Given as cylindrical but not round: the longest extent.
      warnings.emplace_back("No round cross-section found; the cell axis is the longest extent");
    }
    o.axis = round_axis;
  } else {
    o.axis = -1;
  }
  if (o.axis < 0) {
    // Cylindrical without a round section: the longest extent; stacked: the thinnest.
    double best = o.type == CellType::kStacked ? std::numeric_limits<double>::infinity() : 0.0;
    for (int a = 0; a < 3; ++a) {
      const auto extent = static_cast<double>(o.hi[at(a)] - o.lo[at(a)]) * info.voxel_size[at(a)];
      if (o.type == CellType::kStacked ? extent < best : extent > best) {
        best = extent;
        o.axis = a;
      }
    }
  }
  if (o.axis < 0 || o.axis > 2) {
    throw std::invalid_argument("The cell axis must be 0, 1 or 2");
  }
  return o;
}

// ---------------------------------------------------------------------------------------------
// Particles and voids.

/// Whether a voxel lies where particles (`bright`) or voids are looked for.
using InRegion = std::function<bool(const Index3&, bool bright)>;

std::vector<CellFinding> components(std::vector<std::pair<std::uint64_t, float>> voxels,
                                    const Index3& dims, const VoxelSize& pitch,
                                    std::int64_t min_voxels, bool bright) {
  std::sort(voxels.begin(), voxels.end());
  const auto key = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
    return static_cast<std::uint64_t>(x + dims[0] * (y + dims[1] * z));
  };
  std::vector<std::size_t> parent(voxels.size());
  std::iota(parent.begin(), parent.end(), 0);
  const auto find = [&](std::size_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  const auto lookup = [&](std::uint64_t k) -> std::size_t {
    const auto it =
        std::lower_bound(voxels.begin(), voxels.end(), std::pair{k, -1e30F},
                         [](const auto& a, const auto& b) { return a.first < b.first; });
    return it != voxels.end() && it->first == k ? static_cast<std::size_t>(it - voxels.begin())
                                                : voxels.size();
  };
  for (std::size_t i = 0; i < voxels.size(); ++i) {
    const auto k = static_cast<std::int64_t>(voxels[i].first);
    const std::int64_t x = k % dims[0];
    const std::int64_t y = (k / dims[0]) % dims[1];
    const std::int64_t z = k / (dims[0] * dims[1]);
    // Neighbours with a larger key: +x, +y, +z.
    const std::array<std::array<std::int64_t, 3>, 3> next{
        {{x + 1, y, z}, {x, y + 1, z}, {x, y, z + 1}}};
    for (const auto& n : next) {
      if (n[0] >= dims[0] || n[1] >= dims[1] || n[2] >= dims[2]) {
        continue;
      }
      const std::size_t j = lookup(key(n[0], n[1], n[2]));
      if (j < voxels.size()) {
        parent[find(i)] = find(j);
      }
    }
  }
  struct Acc {
    std::int64_t count = 0;
    Vec3 sum{};
    Index3 lo{std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::max(),
              std::numeric_limits<std::int64_t>::max()};
    Index3 hi{std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::min(),
              std::numeric_limits<std::int64_t>::min()};
    double extreme = 0.0;
  };
  std::vector<Acc> acc(voxels.size());
  for (std::size_t i = 0; i < voxels.size(); ++i) {
    Acc& a = acc[find(i)];
    const auto k = static_cast<std::int64_t>(voxels[i].first);
    const Index3 c{k % dims[0], (k / dims[0]) % dims[1], k / (dims[0] * dims[1])};
    if (a.count == 0) {
      a.extreme = voxels[i].second;
    }
    ++a.count;
    for (std::size_t d = 0; d < 3; ++d) {
      a.sum[d] += static_cast<double>(c[d]);
      a.lo[d] = std::min(a.lo[d], c[d]);
      a.hi[d] = std::max(a.hi[d], c[d]);
    }
    a.extreme = bright ? std::max(a.extreme, static_cast<double>(voxels[i].second))
                       : std::min(a.extreme, static_cast<double>(voxels[i].second));
  }
  std::vector<CellFinding> findings;
  for (std::size_t i = 0; i < voxels.size(); ++i) {
    const Acc& a = acc[i];
    if (parent[i] != i || a.count < min_voxels) {
      continue;
    }
    CellFinding f;
    f.voxels = a.count;
    f.volume_mm3 = static_cast<double>(a.count) * pitch.volumeMm3();
    f.equivalent_diameter_mm = std::cbrt(6.0 * f.volume_mm3 / kPi);
    for (std::size_t d = 0; d < 3; ++d) {
      f.center_mm[d] = a.sum[d] / static_cast<double>(a.count) * pitch[d];
      f.size_mm = std::max(f.size_mm, static_cast<double>(a.hi[d] - a.lo[d] + 1) * pitch[d]);
    }
    f.extreme_value = a.extreme;
    findings.push_back(f);
  }
  std::sort(findings.begin(), findings.end(),
            [](const CellFinding& a, const CellFinding& b) { return a.voxels > b.voxels; });
  for (std::size_t i = 0; i < findings.size(); ++i) {
    findings[i].id = static_cast<int>(i + 1);
  }
  return findings;
}

/// Moves bright components too large for particles out of the particles: tabs, foil ends and
/// welds reach into the electrode stack. Numbers the rest from 1.
void separateStructures(BatteryResult& result, const BatteryOptions& options) {
  const auto large = [&](const CellFinding& f) { return f.size_mm > options.max_particle_size_mm; };
  const auto n = std::count_if(result.particles.begin(), result.particles.end(), large);
  result.bright_structures += n;
  result.particles.erase(std::remove_if(result.particles.begin(), result.particles.end(), large),
                         result.particles.end());
  for (std::size_t i = 0; i < result.particles.size(); ++i) {
    result.particles[i].id = static_cast<int>(i + 1);
  }
  if (n > 0) {
    result.warnings.push_back(std::to_string(n) +
                              " bright structures longer than the largest particle (tabs, foil "
                              "ends, welds) are not counted as particles");
  }
}

void findParticlesAndVoids(const Dataset& dataset, const InRegion& in_region, BatteryResult& result,
                           const BatteryOptions& options) {
  const TelemetryPhase phase("particles and voids");
  const DatasetInfo& info = dataset.info();
  using Found = std::vector<std::pair<std::uint64_t, float>>;
  tbb::combinable<Found> bright;
  tbb::combinable<Found> dark;
  const auto particle = static_cast<float>(result.particle_threshold);
  const auto gas = static_cast<float>(result.void_threshold);
  const Index3 dims = info.dims;
  std::atomic<bool> too_many{false};
  dataset.forEachGrid(0, [&](const Index3& /*brick*/, const auto& grid) {
    Found& b = bright.local();
    Found& d = dark.local();
    for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
      for (auto it = leaf->cbeginValueOn(); it; ++it) {
        const auto value = static_cast<float>(*it);
        if (value < particle && value > gas) {
          continue;
        }
        const auto c = it.getCoord();
        const Index3 v{c.x(), c.y(), c.z()};
        if (!in_region(v, value >= particle)) {
          continue;
        }
        const auto key = static_cast<std::uint64_t>(v[0] + dims[0] * (v[1] + dims[1] * v[2]));
        (value >= particle ? b : d).emplace_back(key, value);
      }
      if (b.size() + d.size() > kMaxFindingVoxels) {
        too_many = true;
        return;
      }
    }
  });
  if (too_many) {
    throw std::runtime_error(
        "Too many voxels above the particle threshold or below the void threshold in the "
        "electrode stack; set the thresholds by hand");
  }
  Found all_bright;
  Found all_dark;
  bright.combine_each(
      [&](const Found& f) { all_bright.insert(all_bright.end(), f.begin(), f.end()); });
  dark.combine_each([&](const Found& f) { all_dark.insert(all_dark.end(), f.begin(), f.end()); });
  if (all_bright.size() + all_dark.size() > kMaxFindingVoxels) {
    throw std::runtime_error(
        "Too many voxels above the particle threshold or below the void threshold in the "
        "electrode stack; set the thresholds by hand");
  }
  result.particles =
      components(std::move(all_bright), dims, info.voxel_size, options.min_particle_voxels, true);
  result.voids =
      components(std::move(all_dark), dims, info.voxel_size, options.min_void_voxels, false);
  separateStructures(result, options);
}

void setThresholds(BatteryResult& result, const BatteryOptions& options) {
  const BatteryLevels& l = result.levels;
  const double brightest = std::max(l.cathode, l.anode);
  result.particle_threshold = options.particle_threshold
                                  ? static_cast<double>(*options.particle_threshold)
                                  : brightest + (brightest - l.gap);
  result.void_threshold = options.void_threshold ? static_cast<double>(*options.void_threshold)
                                                 : l.air + 0.5 * (l.gap - l.air);
}

void summarizeSections(BatteryResult& result) {
  std::vector<int> cathodes;
  std::vector<int> anodes;
  std::vector<double> pitches;
  double outer_sum = 0.0;
  result.outer_min_size_mm = kNaN;
  result.outer_max_size_mm = kNaN;
  result.min_wall_mm = kNaN;
  for (const BatterySection& s : result.sections) {
    cathodes.push_back(s.cathode_layers);
    anodes.push_back(s.anode_layers);
    pitches.push_back(s.pitch_mm);
    outer_sum += s.outer_mm;
    result.outer_min_size_mm = nanMin(result.outer_min_size_mm, s.outer_mm);
    result.outer_max_size_mm = nanMax(result.outer_max_size_mm, s.outer_mm);
    result.max_out_of_roundness_mm =
        std::max(result.max_out_of_roundness_mm, s.out_of_roundness_mm);
    result.max_dent_mm = std::max(result.max_dent_mm, s.dent_mm);
    result.min_wall_mm = nanMin(result.min_wall_mm, s.min_wall_mm);
    result.max_core_out_of_roundness_mm =
        std::max(result.max_core_out_of_roundness_mm, s.core_out_of_roundness_mm);
  }
  result.cathode_layers = medianInt(cathodes);
  result.anode_layers = medianInt(anodes);
  result.pitch_mm = median(pitches);
  result.outer_mean_mm =
      result.sections.empty() ? kNaN : outer_sum / static_cast<double>(result.sections.size());
}

/// Counts, pitch and gaps of the layers of one profile.
struct ProfileLayers {
  int cathodes = 0;
  int anodes = 0;
  std::vector<double> cathode_spacing;  // samples
  std::vector<double> spacing;          // between neighbouring layers, samples
  double first = kNaN;                  // position of the first and last layer, samples
  double last = kNaN;
};

ProfileLayers countLayers(const std::vector<Peak>& peaks) {
  ProfileLayers p;
  double last_cathode = kNaN;
  for (std::size_t i = 0; i < peaks.size(); ++i) {
    if (peaks[i].cathode) {
      ++p.cathodes;
      if (!std::isnan(last_cathode)) {
        p.cathode_spacing.push_back(peaks[i].position - last_cathode);
      }
      last_cathode = peaks[i].position;
    } else {
      ++p.anodes;
    }
    if (i > 0) {
      p.spacing.push_back(peaks[i].position - peaks[i - 1].position);
    }
  }
  if (!peaks.empty()) {
    p.first = peaks.front().position;
    p.last = peaks.back().position;
  }
  return p;
}

/// Whether the space between two neighbouring layers is a gap: much wider than usual and dark
/// in its middle, not a layer the peak search missed.
bool isGap(std::span<const double> profile, const Peak& a, const Peak& b, double usual,
           double factor, const BatteryLevels& levels) {
  const double spacing = b.position - a.position;
  if (!(spacing > factor * usual)) {
    return false;
  }
  const auto from = static_cast<std::size_t>(std::ceil(a.position + 0.25 * spacing));
  const auto to = static_cast<std::size_t>(std::floor(b.position - 0.25 * spacing));
  double sum = 0.0;
  double count = 0.0;
  for (std::size_t i = from; i <= to && i < profile.size(); ++i) {
    sum += profile[i];
    count += 1.0;
  }
  const double dark = levels.gap + 0.5 * (std::min(levels.anode, levels.cathode) - levels.gap);
  return count > 0.0 && sum / count < dark;
}

// ---------------------------------------------------------------------------------------------
// Cylindrical cells.

/// Layer positions of the middle section, for the image.
struct LayerPoint {
  std::array<double, 2> mm{};
  bool cathode = false;
};

struct CylinderData {
  int axis = 2;
  int u = 0;
  int v = 1;
  Overview box;
  double step = 0.0;
  std::int64_t ray_samples = 0;
  /// Per section and angle: core radius and inner radius of the can wall.
  std::vector<std::vector<double>> core;
  std::vector<std::vector<double>> inner;
  /// Per section and angle: radius of the innermost and outermost layer, NaN without layers.
  std::vector<std::vector<double>> first_layer;
  std::vector<std::vector<double>> last_layer;
  double pitch = 0.0;               // between neighbouring layers, mm
  std::vector<double> section_pos;  // mm along the axis
  /// Centre line: centre = c0 + c1 * position along the axis.
  std::array<double, 2> c0{};
  std::array<double, 2> c1{};
  std::vector<LayerPoint> points;
  /// Along the axis: the ends of the anodes (particles are looked for up to them) and of the
  /// cathodes (voids).
  double electrode_low = kNaN;
  double electrode_high = kNaN;
  double cathode_low = kNaN;
  double cathode_high = kNaN;

  [[nodiscard]] std::array<double, 2> centre(double pos) const {
    return {c0[0] + c1[0] * pos, c0[1] + c1[1] * pos};
  }
};

void fitCentreLine(CylinderData& data, const std::vector<BatterySection>& sections) {
  const auto n = static_cast<double>(sections.size());
  double sp = 0.0;
  double spp = 0.0;
  std::array<double, 2> sc{};
  std::array<double, 2> spc{};
  for (const BatterySection& s : sections) {
    const double p = s.position_mm[0];
    sp += p;
    spp += p * p;
    for (std::size_t k = 0; k < 2; ++k) {
      sc[k] += s.center_mm[k];
      spc[k] += p * s.center_mm[k];
    }
  }
  const double det = n * spp - sp * sp;
  for (std::size_t k = 0; k < 2; ++k) {
    if (sections.size() < 2 || std::abs(det) < 1e-12) {
      data.c1[k] = 0.0;
      data.c0[k] = sc[k] / n;
    } else {
      data.c1[k] = (n * spc[k] - sp * sc[k]) / det;
      data.c0[k] = (sc[k] - data.c1[k] * sp) / n;
    }
  }
}

void cylinderSections(const VolumeView& volume, const BatteryOptions& options, CylinderData& data,
                      BatteryResult& result) {
  const TelemetryPhase phase("cross-sections");
  const VolumeInfo& info = volume.info();
  const VoxelSize& pitch = info.voxel_size;
  const int a = data.axis;
  const auto lo = data.box.lo;
  const auto hi = data.box.hi;
  data.step = std::min(pitch[at(data.u)], pitch[at(data.v)]);
  const double half_u =
      0.5 * static_cast<double>(hi[at(data.u)] - lo[at(data.u)]) * pitch[at(data.u)];
  const double half_v =
      0.5 * static_cast<double>(hi[at(data.v)] - lo[at(data.v)]) * pitch[at(data.v)];
  data.ray_samples =
      static_cast<std::int64_t>(std::ceil(1.25 * std::hypot(half_u, half_v) / data.step));
  const int n_angles = std::max(8, options.angles);
  const int n_sections = std::max(1, options.sections);
  const auto air = static_cast<double>(info.air_level);
  const auto threshold = static_cast<double>(info.threshold);
  const auto wall_window =
      static_cast<std::size_t>(std::max(4.0, std::ceil(0.5 / data.step)));  // 0.5 mm

  struct Ray {
    std::vector<double> profile;
    Band wall;
    double outer = kNaN;  // samples
    double core = 0.0;    // samples
    double inner = kNaN;  // samples
    double noise = 0.0;
    std::vector<Peak> peaks;
  };
  std::vector<std::vector<Ray>> rays(at(n_sections));
  std::vector<Plane> planes(at(n_sections));
  result.sections.resize(at(n_sections));
  data.core.assign(at(n_sections), std::vector<double>(at(n_angles), kNaN));
  data.inner.assign(at(n_sections), std::vector<double>(at(n_angles), kNaN));
  tbb::parallel_for(0, n_sections, [&](int k) {
    const double fraction = n_sections == 1 ? 0.5 : 0.2 + 0.6 * k / (n_sections - 1);
    auto index = static_cast<std::int64_t>(
        std::lround(static_cast<double>(lo[at(a)]) +
                    fraction * static_cast<double>(hi[at(a)] - lo[at(a)] - 1)));
    if (a == 2 && !info.planes.empty()) {
      index = nearestPlane(info.planes, index);
    }
    planes[at(k)] = readPlane(volume, a, index, lo, hi, info.air_level);
    const Plane& plane = planes[at(k)];
    BatterySection& section = result.sections[at(k)];
    section.position_mm[0] = static_cast<double>(index) * pitch[at(a)];
    // Centre of the material in the slice, then of the circle through the outer surface.
    double sx = 0.0;
    double sy = 0.0;
    double count = 0.0;
    for (std::int64_t j = 0; j < plane.nv; ++j) {
      for (std::int64_t i = 0; i < plane.nu; ++i) {
        if (plane.voxel(i, j) >= info.threshold) {
          sx += static_cast<double>(plane.u0 + i) * plane.pu;
          sy += static_cast<double>(plane.v0 + j) * plane.pv;
          count += 1.0;
        }
      }
    }
    std::array<double, 2> centre{sx / std::max(1.0, count), sy / std::max(1.0, count)};
    std::vector<Ray>& section_rays = rays[at(k)];
    section_rays.assign(at(n_angles), Ray{});
    double radius = kNaN;
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<std::array<double, 2>> points;
      for (int j = 0; j < n_angles; ++j) {
        const double angle = 2.0 * kPi * j / n_angles;
        Ray& ray = section_rays[at(j)];
        ray.profile = rayProfile(plane, centre, angle, data.step, data.ray_samples,
                                 options.average_voxels, ray.noise);
        std::vector<double> reversed(ray.profile.rbegin(), ray.profile.rend());
        ray.wall = firstBand(reversed, threshold, air, wall_window);
        const auto last = static_cast<double>(ray.profile.size() - 1);
        ray.outer = last - ray.wall.outer;
        ray.inner = std::isnan(ray.wall.inner) ? kNaN : last - ray.wall.inner;
        if (!std::isnan(ray.outer)) {
          points.push_back({centre[0] + ray.outer * data.step * std::cos(angle),
                            centre[1] + ray.outer * data.step * std::sin(angle)});
        }
      }
      if (points.size() < 3) {
        throw std::runtime_error("No outer surface of the cell found");
      }
      const auto [fitted, r] = fitCircle(points);
      if (std::isnan(r)) {
        throw std::runtime_error("No outer surface of the cell found");
      }
      if (pass == 0) {
        centre = fitted;
      }
      radius = r;
    }
    section.center_mm = centre;
    section.outer_mm = 2.0 * radius;
    double rmin = kNaN;
    double rmax = kNaN;
    double wall_min = kNaN;
    double wall_sum = 0.0;
    double wall_count = 0.0;
    double core_sum = 0.0;
    double core_count = 0.0;
    double cmin = kNaN;
    double cmax = kNaN;
    std::vector<std::array<double, 2>> core_points;
    section.outer_radius_mm.assign(at(n_angles), kNaN);
    section.core_radius_mm.assign(at(n_angles), 0.0);
    for (int j = 0; j < n_angles; ++j) {
      Ray& ray = section_rays[at(j)];
      const double angle = 2.0 * kPi * j / n_angles;
      const double r_out = ray.outer * data.step;
      section.outer_radius_mm[at(j)] = r_out;
      rmin = nanMin(rmin, r_out);
      rmax = nanMax(rmax, r_out);
      if (!std::isnan(ray.inner)) {
        const double wall = (ray.outer - ray.inner) * data.step;
        wall_min = nanMin(wall_min, wall);
        wall_sum += wall;
        wall_count += 1.0;
      }
      // Hollow core: from the centre to the first material.
      const std::vector<double>& p = ray.profile;
      std::size_t c = 0;
      if (p.size() > 3 && (p[0] + p[1] + p[2]) / 3.0 < threshold) {
        while (c + 1 < p.size() && p[c + 1] < threshold) {
          ++c;
        }
        ray.core = static_cast<double>(c) + (threshold - p[c]) / std::max(1e-9, p[c + 1] - p[c]);
        core_points.push_back({centre[0] + ray.core * data.step * std::cos(angle),
                               centre[1] + ray.core * data.step * std::sin(angle)});
      }
      section.core_radius_mm[at(j)] = ray.core * data.step;
      data.core[at(k)][at(j)] = ray.core * data.step;
      core_sum += ray.core * data.step;
      core_count += 1.0;
      cmin = nanMin(cmin, ray.core * data.step);
      cmax = nanMax(cmax, ray.core * data.step);
      const double inner =
          std::isnan(ray.inner) ? ray.outer - 2.0 * static_cast<double>(wall_window) : ray.inner;
      data.inner[at(k)][at(j)] = inner * data.step;
    }
    section.out_of_roundness_mm = rmax - rmin;
    section.dent_mm = std::max(0.0, radius - rmin);
    section.min_wall_mm = wall_min;
    section.mean_wall_mm = wall_count > 0.0 ? wall_sum / wall_count : kNaN;
    section.core_diameter_mm = 2.0 * core_sum / std::max(1.0, core_count);
    section.core_out_of_roundness_mm = cmax - cmin;
    if (core_points.size() >= 3) {
      const auto [core_centre, core_r] = fitCircle(core_points);
      if (!std::isnan(core_r)) {
        section.core_offset_mm = std::hypot(core_centre[0] - centre[0], core_centre[1] - centre[1]);
      }
    }
  });

  // Layers: peaks between the core and the can wall; classes over all sections.
  std::vector<double> heights;
  std::vector<double> valleys;
  for (int k = 0; k < n_sections; ++k) {
    for (int j = 0; j < n_angles; ++j) {
      auto& ray = rays[at(k)][at(j)];
      const auto from = static_cast<std::size_t>(std::ceil(ray.core)) + 1;
      const double inner = data.inner[at(k)][at(j)] / data.step - 1.0;
      const auto to = static_cast<std::size_t>(std::max(0.0, std::floor(inner)));
      if (to <= from + 3) {
        continue;
      }
      const std::span<const double> segment(ray.profile.data() + from, to - from);
      ray.peaks = findPeaks(ray.profile, from, to, minProminence(segment, ray.noise));
      for (const Peak& peak : ray.peaks) {
        heights.push_back(peak.value);
      }
      collectValleys(ray.profile, ray.peaks, valleys);
    }
  }
  const Classes classes = classify(heights);
  result.levels.electrodes_distinct = classes.distinct;
  result.levels.cathode = options.cathode_brighter ? classes.high : classes.low;
  result.levels.anode = options.cathode_brighter ? classes.low : classes.high;
  result.levels.gap = median(valleys);
  std::vector<double> wall_peaks;
  for (const auto& section_rays : rays) {
    for (const auto& ray : section_rays) {
      wall_peaks.push_back(ray.wall.peak);
    }
  }
  result.levels.housing = median(wall_peaks);
  if (!classes.distinct) {
    result.warnings.emplace_back(
        "Anode and cathode layers do not differ in brightness: the layers found are counted as "
        "cathode layers, and the anode is looked for between them beyond the cathode ends");
  }

  std::vector<double> all_spacing;
  for (int k = 0; k < n_sections; ++k) {
    for (auto& ray : rays[at(k)]) {
      label(ray.peaks, classes, options.cathode_brighter);
      const ProfileLayers layers = countLayers(ray.peaks);
      all_spacing.insert(all_spacing.end(), layers.spacing.begin(), layers.spacing.end());
    }
  }
  const double spacing_median = median(all_spacing);
  data.pitch = spacing_median * data.step;
  data.first_layer.assign(at(n_sections), std::vector<double>(at(n_angles), kNaN));
  data.last_layer.assign(at(n_sections), std::vector<double>(at(n_angles), kNaN));
  for (int k = 0; k < n_sections; ++k) {
    BatterySection& section = result.sections[at(k)];
    std::vector<int> cathodes;
    std::vector<int> anodes;
    std::vector<double> pitches;
    std::vector<double> outermost;
    for (int j = 0; j < n_angles; ++j) {
      const auto& ray = rays[at(k)][at(j)];
      const double angle = 2.0 * kPi * j / n_angles;
      const ProfileLayers layers = countLayers(ray.peaks);
      cathodes.push_back(layers.cathodes);
      anodes.push_back(layers.anodes);
      data.first_layer[at(k)][at(j)] = layers.first * data.step;
      data.last_layer[at(k)][at(j)] = layers.last * data.step;
      pitches.insert(pitches.end(), layers.cathode_spacing.begin(), layers.cathode_spacing.end());
      if (!std::isnan(layers.last)) {
        outermost.push_back(layers.last * data.step);
      }
      // Not next to the innermost and outermost layer, where the electrode strips start and
      // end and a layer is missing over part of the turn.
      for (std::size_t i = 2; i + 1 < ray.peaks.size(); ++i) {
        const double spacing = ray.peaks[i].position - ray.peaks[i - 1].position;
        if (isGap(ray.profile, ray.peaks[i - 1], ray.peaks[i], spacing_median, options.gap_factor,
                  result.levels)) {
          const double r = 0.5 * (ray.peaks[i].position + ray.peaks[i - 1].position) * data.step;
          LayerGap gap;
          gap.position_mm[at(a)] = section.position_mm[0];
          gap.position_mm[at(data.u)] = section.center_mm[0] + r * std::cos(angle);
          gap.position_mm[at(data.v)] = section.center_mm[1] + r * std::sin(angle);
          gap.size_mm = spacing * data.step;
          gap.expected_mm = spacing_median * data.step;
          gap.section = k;
          result.gaps.push_back(gap);
        }
      }
      if (k == n_sections / 2) {
        for (const Peak& peak : ray.peaks) {
          const double r = peak.position * data.step;
          data.points.push_back({{section.center_mm[0] + r * std::cos(angle),
                                  section.center_mm[1] + r * std::sin(angle)},
                                 peak.cathode});
        }
      }
    }
    section.cathode_layers = medianInt(cathodes);
    section.anode_layers = medianInt(anodes);
    section.pitch_mm = median(pitches) * data.step;
    section.stack_mm = 2.0 * median(outermost);
  }
  data.section_pos.clear();
  for (const BatterySection& s : result.sections) {
    data.section_pos.push_back(s.position_mm[0]);
  }
  fitCentreLine(data, result.sections);
}

/// Overhang samples of the sections across one edge of a cylindrical cell, one per angle (in
/// degrees); keeps the section of the smallest overhang in `worst`.
void edgeOverhang(std::vector<SectionImage>& images, const std::vector<double>& angles, int edge,
                  const BatteryOptions& options, BatteryResult& result, bool& unfinished,
                  std::vector<int>& hidden_edges, std::vector<SectionImage>& worst) {
  const auto n = static_cast<int>(images.size());
  std::vector<std::vector<OverhangSample>> per_angle(at(n));
  std::vector<char> per_unfinished(at(n), 0);
  std::vector<char> per_hidden(at(n), 0);
  tbb::parallel_for(0, n, [&](int j) {
    bool open = false;
    bool hide = false;
    sectionOverhang(images[at(j)], result.levels, options, edge, angles[at(j)], per_angle[at(j)],
                    open, hide);
    per_unfinished[at(j)] = open ? 1 : 0;
    per_hidden[at(j)] = hide ? 1 : 0;
  });
  warnStrayEnds(result, edge, dropStrayEnds(per_angle, options.max_end_deviation_mm),
                options.max_end_deviation_mm);
  // Fewer layer ends than sections: what is left are stray ends, not the edge.
  std::size_t kept = 0;
  for (const auto& samples : per_angle) {
    kept += samples.size();
  }
  if (kept > 0 && kept < per_angle.size()) {
    for (auto& samples : per_angle) {
      samples.clear();
    }
    result.warnings.push_back("At " + result.edges[at(edge)].name + " only " +
                              std::to_string(kept) +
                              " layer ends were found, fewer than one per section; the overhang "
                              "there is not measured");
  }
  int hidden_sections = 0;
  for (int j = 0; j < n; ++j) {
    result.overhang.insert(result.overhang.end(), per_angle[at(j)].begin(), per_angle[at(j)].end());
    unfinished = unfinished || per_unfinished[at(j)] != 0;
    hidden_sections += per_hidden[at(j)];
  }
  if (2 * hidden_sections > n) {
    hidden_edges.push_back(edge);
  }
  std::int64_t worst_index = -1;
  double worst_value = std::numeric_limits<double>::infinity();
  for (const OverhangSample& s : result.overhang) {
    if (s.edge == edge && s.overhang_mm < worst_value) {
      worst_value = s.overhang_mm;
      worst_index = std::find(angles.begin(), angles.end(), s.section) - angles.begin();
    }
  }
  worst.push_back(worst_index >= 0 && worst_index < n ? std::move(images[at(worst_index)])
                                                      : SectionImage{});
}

/// Sections across both ends of a cylindrical cell, one per angle, from a quarter of the length
/// inside the cell outwards to the end of the can.
void cylinderOverhang(const VolumeView& volume, const BatteryOptions& options, CylinderData& data,
                      BatteryResult& result, std::vector<SectionImage>& worst) {
  const TelemetryPhase phase("overhang");
  const VolumeInfo& info = volume.info();
  const VoxelSize& pitch = info.voxel_size;
  const int a = data.axis;
  const int n_angles = std::max(1, options.overhang_sections);
  const auto lo = data.box.lo;
  const auto hi = data.box.hi;
  // Across: from inside the core to the wall, the same samples for every angle.
  double core_min = kNaN;
  double inner_max = kNaN;
  for (std::size_t k = 0; k < data.core.size(); ++k) {
    for (std::size_t j = 0; j < data.core[k].size(); ++j) {
      core_min = nanMin(core_min, data.core[k][j]);
      inner_max = nanMax(inner_max, data.inner[k][j]);
    }
  }
  const double across_from = std::max(0.0, core_min - 2.0 * data.step);
  const auto across = static_cast<std::int64_t>(std::ceil((inner_max - across_from) / data.step));
  const std::int64_t length = hi[at(a)] - lo[at(a)];
  const std::int64_t depth = std::max<std::int64_t>(8, length / 4);
  bool unfinished = false;
  std::vector<int> hidden_edges;
  for (int side = 0; side < 2; ++side) {
    const int edge = static_cast<int>(result.edges.size());
    result.edges.push_back({a, side, edgeName(a, side)});
    const std::int64_t start = side == 0 ? lo[at(a)] + depth : hi[at(a)] - 1 - depth;
    const std::int64_t rows = depth + 1;
    std::vector<SectionImage> images(at(n_angles));
    for (int j = 0; j < n_angles; ++j) {
      SectionImage& image = images[at(j)];
      image.across = across;
      image.along = rows;
      image.across_from_mm = across_from;
      image.across_step_mm = data.step;
      image.along_from_mm = static_cast<double>(start) * pitch[at(a)];
      image.along_step_mm = (side == 0 ? -1.0 : 1.0) * pitch[at(a)];
      image.values.assign(at(across * rows), info.air_level);
      // Valid up to the can wall at this angle (smallest over the sections).
      const double angle_deg = 360.0 * j / n_angles;
      const auto bin = static_cast<std::size_t>(std::lround(
                           angle_deg / 360.0 * static_cast<double>(data.inner.front().size()))) %
                       data.inner.front().size();
      double wall = kNaN;
      for (const auto& inner : data.inner) {
        wall = nanMin(wall, inner[bin]);
      }
      image.valid_across = std::clamp<std::int64_t>(
          static_cast<std::int64_t>(std::floor((wall - across_from) / data.step)) - 1, 0, across);
    }
    tbb::parallel_for(
        tbb::blocked_range<std::int64_t>(0, rows),
        [&](const tbb::blocked_range<std::int64_t>& range) {
          for (std::int64_t s = range.begin(); s != range.end(); ++s) {
            const std::int64_t index = start + (side == 0 ? -s : s);
            const Plane plane = readPlane(volume, a, index, lo, hi, info.air_level);
            const auto centre = data.centre(static_cast<double>(index) * pitch[at(a)]);
            for (int j = 0; j < n_angles; ++j) {
              const double angle = 2.0 * kPi * j / n_angles;
              const double c = std::cos(angle);
              const double sn = std::sin(angle);
              SectionImage& image = images[at(j)];
              for (std::int64_t i = 0; i < across; ++i) {
                const double r = across_from + static_cast<double>(i) * data.step;
                double sum = 0.0;
                for (int t = -options.average_voxels; t <= options.average_voxels; ++t) {
                  const double off = t * data.step;
                  sum += plane.sample(centre[0] + r * c - off * sn, centre[1] + r * sn + off * c);
                }
                image.values[at(i + across * s)] =
                    static_cast<float>(sum / (2 * options.average_voxels + 1));
              }
            }
          }
        });
    std::vector<double> angles(at(n_angles));
    for (int j = 0; j < n_angles; ++j) {
      angles[at(j)] = 360.0 * j / n_angles;
    }
    edgeOverhang(images, angles, edge, options, result, unfinished, hidden_edges, worst);
  }
  warnOverhang(result, unfinished, hidden_edges);
}

/// Takes the measured ends of the electrodes along the axis, or well inside the can, clear of
/// the tabs and the cap, where they were not measured.
void cylinderEnds(CylinderData& data, const BatteryResult& result) {
  for (const BatteryEdge& edge : result.edges) {
    if (edge.samples > 0) {
      (edge.side == 0 ? data.electrode_low : data.electrode_high) = edge.anode_edge_mm;
      // Voids inside the shortest cathode, clear of the electrolyte beyond the cathode ends.
      (edge.side == 0 ? data.cathode_low : data.cathode_high) =
          edge.cathode_edge_mm + (edge.side == 0 ? 1.0 : -1.0) * edge.cathode_edge_spread_mm;
    }
  }
  const double low = result.bounds_min_mm[at(data.axis)] + 0.1 * result.length_mm;
  const double high = result.bounds_max_mm[at(data.axis)] - 0.1 * result.length_mm;
  data.cathode_low = std::isnan(data.cathode_low) ? low : data.cathode_low;
  data.cathode_high = std::isnan(data.cathode_high) ? high : data.cathode_high;
  data.electrode_low = std::isnan(data.electrode_low) ? data.cathode_low : data.electrode_low;
  data.electrode_high = std::isnan(data.electrode_high) ? data.cathode_high : data.electrode_high;
}

/// Where particles and voids are looked for in a cylindrical cell: in the winding, between the
/// electrode ends along the axis.
InRegion cylinderRegion(const CylinderData& data, const VoxelSize& pitch) {
  const double margin = 2.0 * pitch[at(data.axis)];
  const auto n_angles = static_cast<double>(data.core.front().size());
  // The innermost turn of each section: the strips start somewhere on it, and the turns inside
  // and next to it hold folded separator and electrolyte that look like gas.
  std::vector<double> first_turn;
  for (const std::vector<double>& radii : data.first_layer) {
    double r = kNaN;
    for (const double first : radii) {
      r = nanMax(r, first);
    }
    first_turn.push_back(r);
  }
  return [data, margin, pitch, n_angles, first_turn](const Index3& v, bool bright) {
    const auto pos = static_cast<double>(v[at(data.axis)]) * pitch[at(data.axis)];
    const double from = bright ? data.electrode_low : data.cathode_low;
    const double to = bright ? data.electrode_high : data.cathode_high;
    if (pos < from + margin || pos > to - margin) {
      return false;
    }
    const auto c = data.centre(pos);
    const auto du = static_cast<double>(v[at(data.u)]) * pitch[at(data.u)] - c[0];
    const auto dv = static_cast<double>(v[at(data.v)]) * pitch[at(data.v)] - c[1];
    const double r = std::hypot(du, dv);
    double angle = std::atan2(dv, du);
    if (angle < 0.0) {
      angle += 2.0 * kPi;
    }
    const auto bins = static_cast<std::size_t>(n_angles);
    const auto bin = static_cast<std::size_t>(std::lround(angle / (2.0 * kPi) * n_angles)) % bins;
    std::size_t k = 0;
    for (std::size_t i = 1; i < data.section_pos.size(); ++i) {
      if (std::abs(data.section_pos[i] - pos) < std::abs(data.section_pos[k] - pos)) {
        k = i;
      }
    }
    // Between the innermost and the outermost layer at this angle, where there are layers; a
    // particle may sit half a layer pitch outside them. Voids not in the first and last turn,
    // where the electrolyte at the start and end of the strips looks like gas.
    const double first =
        bright || std::isnan(first_turn[k]) ? data.first_layer[k][bin] : first_turn[k];
    const double last = data.last_layer[k][bin];
    const double slack = bright ? 0.5 * data.pitch : -data.pitch;
    const double lo = std::isnan(first) ? data.core[k][bin] + 2.0 * data.step : first - slack;
    const double hi = std::isnan(last) ? data.inner[k][bin] - 2.0 * data.step : last + slack;
    return r > lo && r < hi && r < data.inner[k][bin] - 2.0 * data.step;
  };
}

/// Radius and angle (cylindrical) or position along the normal (stacked) of the findings.
void placeFindings(BatteryResult& result) {
  for (auto* list : {&result.particles, &result.voids}) {
    for (CellFinding& f : *list) {
      if (result.type == CellType::kCylindrical) {
        const auto [u, v] = planeAxes(result.axis);
        double best = std::numeric_limits<double>::infinity();
        std::array<double, 2> c{};
        for (const BatterySection& s : result.sections) {
          const double d = std::abs(s.position_mm[0] - f.center_mm[at(result.axis)]);
          if (d < best) {
            best = d;
            c = s.center_mm;
          }
        }
        const double du = f.center_mm[at(u)] - c[0];
        const double dv = f.center_mm[at(v)] - c[1];
        f.radius_mm = std::hypot(du, dv);
        f.angle_deg = std::atan2(dv, du) * 180.0 / kPi;
        if (f.angle_deg < 0.0) {
          f.angle_deg += 360.0;
        }
      } else {
        f.radius_mm = f.center_mm[at(result.axis)];
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Stacked cells.

struct StackData {
  int n = 2;  // normal
  int u = 0;  // longer in-plane axis
  int v = 1;
  Overview box;
  double first_layer = kNaN;  // mm along the normal
  double last_layer = kNaN;
  double half_pitch = 0.0;
  std::vector<LayerPoint> points;  // in the plane (u, n) through the middle of v
};

/// Mean profile along the normal over a (2w+1)² column at (iu, iv); `noise` receives its noise
/// from the spread within the column.
std::vector<double> stackProfile(const VolumeView& volume, const StackData& data, std::int64_t iu,
                                 std::int64_t iv, int w, double& noise) {
  const VolumeInfo& info = volume.info();
  Box box;
  box.min = data.box.lo;
  box.max = data.box.hi;
  box.min[at(data.u)] = std::max<std::int64_t>(0, iu - w);
  box.max[at(data.u)] = std::min(info.dims[at(data.u)], iu + w + 1);
  box.min[at(data.v)] = std::max<std::int64_t>(0, iv - w);
  box.max[at(data.v)] = std::min(info.dims[at(data.v)], iv + w + 1);
  std::vector<float> values(at(box.voxelCount()));
  volume.readRegion(box, values, info.air_level);
  const std::int64_t len = box.size(at(data.n));
  std::vector<double> profile(at(len), 0.0);
  std::vector<double> sum_sq(at(len), 0.0);
  const Index3 size{box.size(0), box.size(1), box.size(2)};
  const auto count = static_cast<double>(box.voxelCount()) / static_cast<double>(len);
  for (std::int64_t z = 0; z < size[2]; ++z) {
    for (std::int64_t y = 0; y < size[1]; ++y) {
      for (std::int64_t x = 0; x < size[0]; ++x) {
        const Index3 c{x, y, z};
        const double v = values[at(x + size[0] * (y + size[1] * z))];
        profile[at(c[at(data.n)])] += v / count;
        sum_sq[at(c[at(data.n)])] += v * v / count;
      }
    }
  }
  std::vector<double> spread(at(len));
  for (std::size_t i = 0; i < spread.size(); ++i) {
    spread[i] = std::sqrt(std::max(0.0, sum_sq[i] - profile[i] * profile[i]));
  }
  noise = count > 1.0 ? median(spread) * std::sqrt(count / (count - 1.0)) / std::sqrt(count) : 0.0;
  return profile;
}

void stackSections(const VolumeView& volume, const BatteryOptions& options, StackData& data,
                   BatteryResult& result) {
  const TelemetryPhase phase("profiles");
  const VolumeInfo& info = volume.info();
  const VoxelSize& pitch = info.voxel_size;
  const auto air = static_cast<double>(info.air_level);
  const auto threshold = static_cast<double>(info.threshold);
  const int per_axis = std::max(1, options.sections);
  const double step = pitch[at(data.n)];
  const auto wall_window = static_cast<std::size_t>(std::max(4.0, std::ceil(0.5 / step)));
  const auto lo = data.box.lo;
  const auto hi = data.box.hi;
  struct Profile {
    std::vector<double> values;
    double noise = 0.0;
    Band low;
    Band high;
    std::vector<Peak> peaks;
  };
  std::vector<Profile> profiles(at(static_cast<std::int64_t>(per_axis) * per_axis));
  result.sections.resize(profiles.size());
  tbb::parallel_for(0, per_axis * per_axis, [&](int k) {
    const int i = k % per_axis;
    const int j = k / per_axis;
    const double fu = per_axis == 1 ? 0.5 : 0.2 + 0.6 * i / (per_axis - 1);
    const double fv = per_axis == 1 ? 0.5 : 0.2 + 0.6 * j / (per_axis - 1);
    const auto iu = static_cast<std::int64_t>(
        std::lround(static_cast<double>(lo[at(data.u)]) +
                    fu * static_cast<double>(hi[at(data.u)] - lo[at(data.u)] - 1)));
    const auto iv = static_cast<std::int64_t>(
        std::lround(static_cast<double>(lo[at(data.v)]) +
                    fv * static_cast<double>(hi[at(data.v)] - lo[at(data.v)] - 1)));
    Profile& profile = profiles[at(k)];
    profile.values = stackProfile(volume, data, iu, iv, options.average_voxels, profile.noise);
    profile.low = firstBand(profile.values, threshold, air, wall_window);
    std::vector<double> reversed(profile.values.rbegin(), profile.values.rend());
    profile.high = firstBand(reversed, threshold, air, wall_window);
    BatterySection& section = result.sections[at(k)];
    section.position_mm = {static_cast<double>(iu) * pitch[at(data.u)],
                           static_cast<double>(iv) * pitch[at(data.v)]};
    const auto last = static_cast<double>(profile.values.size() - 1);
    section.outer_mm = (last - profile.high.outer - profile.low.outer) * step;
    double wall_min = kNaN;
    double wall_sum = 0.0;
    double walls = 0.0;
    for (const Band* band : {&profile.low, &profile.high}) {
      if (!std::isnan(band->inner)) {
        const double wall = (band->inner - band->outer) * step;
        wall_min = nanMin(wall_min, wall);
        wall_sum += wall;
        walls += 1.0;
      }
    }
    section.min_wall_mm = wall_min;
    section.mean_wall_mm = walls > 0.0 ? wall_sum / walls : kNaN;
  });
  std::vector<double> heights;
  std::vector<double> valleys;
  for (Profile& profile : profiles) {
    const auto last = static_cast<double>(profile.values.size() - 1);
    const double from_f = std::isnan(profile.low.inner)
                              ? profile.low.outer + 2.0 * static_cast<double>(wall_window)
                              : profile.low.inner + 1.0;
    const double to_f = std::isnan(profile.high.inner)
                            ? last - profile.high.outer - 2.0 * static_cast<double>(wall_window)
                            : last - profile.high.inner - 1.0;
    if (to_f <= from_f + 3.0) {
      continue;
    }
    const auto from = static_cast<std::size_t>(std::ceil(from_f));
    const auto to = static_cast<std::size_t>(std::floor(to_f));
    const std::span<const double> segment(profile.values.data() + from, to - from);
    profile.peaks = findPeaks(profile.values, from, to, minProminence(segment, profile.noise));
    for (const Peak& peak : profile.peaks) {
      heights.push_back(peak.value);
    }
    collectValleys(profile.values, profile.peaks, valleys);
  }
  const Classes classes = classify(heights);
  result.levels.electrodes_distinct = classes.distinct;
  result.levels.cathode = options.cathode_brighter ? classes.high : classes.low;
  result.levels.anode = options.cathode_brighter ? classes.low : classes.high;
  result.levels.gap = median(valleys);
  std::vector<double> wall_peaks;
  for (const Profile& profile : profiles) {
    wall_peaks.push_back(profile.low.peak);
    wall_peaks.push_back(profile.high.peak);
  }
  result.levels.housing = median(wall_peaks);
  if (!classes.distinct) {
    result.warnings.emplace_back(
        "Anode and cathode layers do not differ in brightness: the layers found are counted as "
        "cathode layers, and the anode is looked for between them beyond the cathode ends");
  }
  std::vector<double> all_spacing;
  for (Profile& profile : profiles) {
    label(profile.peaks, classes, options.cathode_brighter);
    const ProfileLayers layers = countLayers(profile.peaks);
    all_spacing.insert(all_spacing.end(), layers.spacing.begin(), layers.spacing.end());
  }
  const double spacing_median = median(all_spacing);
  data.half_pitch = 0.5 * spacing_median * step;
  const auto n0 = static_cast<double>(lo[at(data.n)]) * step;
  for (std::size_t k = 0; k < profiles.size(); ++k) {
    const Profile& profile = profiles[k];
    BatterySection& section = result.sections[k];
    const ProfileLayers layers = countLayers(profile.peaks);
    section.cathode_layers = layers.cathodes;
    section.anode_layers = layers.anodes;
    section.pitch_mm = median(layers.cathode_spacing) * step;
    if (!std::isnan(layers.first)) {
      section.stack_mm = (layers.last - layers.first) * step;
      data.first_layer = nanMin(data.first_layer, n0 + layers.first * step);
      data.last_layer = nanMax(data.last_layer, n0 + layers.last * step);
    }
    for (std::size_t i = 1; i < profile.peaks.size(); ++i) {
      const double spacing = profile.peaks[i].position - profile.peaks[i - 1].position;
      if (isGap(profile.values, profile.peaks[i - 1], profile.peaks[i], spacing_median,
                options.gap_factor, result.levels)) {
        LayerGap gap;
        gap.position_mm[at(data.u)] = section.position_mm[0];
        gap.position_mm[at(data.v)] = section.position_mm[1];
        gap.position_mm[at(data.n)] =
            n0 + 0.5 * (profile.peaks[i].position + profile.peaks[i - 1].position) * step;
        gap.size_mm = spacing * step;
        gap.expected_mm = spacing_median * step;
        gap.section = static_cast<int>(k);
        result.gaps.push_back(gap);
      }
    }
  }
}

/// Sections across the four edges of a stack, from the middle of the cell outwards.
void stackOverhang(const VolumeView& volume, const BatteryOptions& options, StackData& data,
                   BatteryResult& result, std::vector<SectionImage>& worst) {
  if (std::isnan(data.first_layer)) {
    return;
  }
  const TelemetryPhase phase("overhang");
  const VolumeInfo& info = volume.info();
  const VoxelSize& pitch = info.voxel_size;
  const int per_edge = std::max(3, options.overhang_sections / 4);
  const double step_n = pitch[at(data.n)];
  const double margin = std::max(2.0 * data.half_pitch, 2.0 * step_n);
  const auto n_lo = std::max<std::int64_t>(
      0, static_cast<std::int64_t>(std::floor((data.first_layer - margin) / step_n)));
  const auto n_hi = std::min<std::int64_t>(
      info.dims[at(data.n)],
      static_cast<std::int64_t>(std::ceil((data.last_layer + margin) / step_n)) + 1);
  bool unfinished = false;
  std::vector<int> hidden_edges;
  for (const int axis : {data.u, data.v}) {
    const int other = axis == data.u ? data.v : data.u;
    for (int side = 0; side < 2; ++side) {
      const int edge = static_cast<int>(result.edges.size());
      result.edges.push_back({axis, side, edgeName(axis, side)});
      const std::int64_t lo = data.box.lo[at(axis)];
      const std::int64_t hi = data.box.hi[at(axis)];
      const std::int64_t mid = (lo + hi) / 2;
      const std::int64_t rows = side == 0 ? mid - lo + 1 : hi - mid;
      std::vector<SectionImage> images(at(per_edge));
      std::vector<double> positions(at(per_edge));
      tbb::parallel_for(0, per_edge, [&](int j) {
        const double f = 0.1 + 0.8 * j / (per_edge - 1);
        const std::int64_t olo = data.box.lo[at(other)];
        const std::int64_t ohi = data.box.hi[at(other)];
        const auto io = static_cast<std::int64_t>(
            std::lround(static_cast<double>(olo) + f * static_cast<double>(ohi - olo - 1)));
        positions[at(j)] = static_cast<double>(io) * pitch[at(other)];
        Box box;
        box.min[at(data.n)] = n_lo;
        box.max[at(data.n)] = n_hi;
        box.min[at(axis)] = side == 0 ? lo : mid;
        box.max[at(axis)] = side == 0 ? mid + 1 : hi;
        box.min[at(other)] = std::max<std::int64_t>(0, io - options.average_voxels);
        box.max[at(other)] = std::min(info.dims[at(other)], io + options.average_voxels + 1);
        std::vector<float> values(at(box.voxelCount()));
        volume.readRegion(box, values, info.air_level);
        SectionImage& image = images[at(j)];
        image.across = n_hi - n_lo;
        image.along = rows;
        image.valid_across = image.across;
        image.across_from_mm = static_cast<double>(n_lo) * step_n;
        image.across_step_mm = step_n;
        image.along_from_mm = static_cast<double>(mid) * pitch[at(axis)];
        image.along_step_mm = (side == 0 ? -1.0 : 1.0) * pitch[at(axis)];
        image.values.assign(at(image.across * rows), 0.0F);
        const Index3 size{box.size(0), box.size(1), box.size(2)};
        const auto count = static_cast<float>(box.size(at(other)));
        for (std::int64_t z = 0; z < size[2]; ++z) {
          for (std::int64_t y = 0; y < size[1]; ++y) {
            for (std::int64_t x = 0; x < size[0]; ++x) {
              const Index3 c{x, y, z};
              const std::int64_t a = c[at(data.n)];
              const std::int64_t s = side == 0 ? (mid - lo) - c[at(axis)] : c[at(axis)];
              image.values[at(a + image.across * s)] +=
                  values[at(x + size[0] * (y + size[1] * z))] / count;
            }
          }
        }
      });
      std::vector<std::vector<OverhangSample>> per_section(at(per_edge));
      std::vector<char> open(at(per_edge), 0);
      std::vector<char> hidden(at(per_edge), 0);
      tbb::parallel_for(0, per_edge, [&](int j) {
        bool o = false;
        bool h = false;
        sectionOverhang(images[at(j)], result.levels, options, edge, positions[at(j)],
                        per_section[at(j)], o, h);
        open[at(j)] = o ? 1 : 0;
        hidden[at(j)] = h ? 1 : 0;
      });
      if (2 * std::count(hidden.begin(), hidden.end(), 1) > per_edge) {
        hidden_edges.push_back(edge);
      }
      warnStrayEnds(result, edge, dropStrayEnds(per_section, options.max_end_deviation_mm),
                    options.max_end_deviation_mm);
      std::int64_t worst_index = -1;
      double worst_value = std::numeric_limits<double>::infinity();
      for (int j = 0; j < per_edge; ++j) {
        for (const OverhangSample& s : per_section[at(j)]) {
          if (s.overhang_mm < worst_value) {
            worst_value = s.overhang_mm;
            worst_index = j;
          }
        }
        result.overhang.insert(result.overhang.end(), per_section[at(j)].begin(),
                               per_section[at(j)].end());
        unfinished = unfinished || open[at(j)] != 0;
      }
      worst.push_back(worst_index >= 0 ? std::move(images[at(worst_index)]) : SectionImage{});
    }
  }
  warnOverhang(result, unfinished, hidden_edges);
}

/// Outer size of the cell along `axis` on a line through `through` (level-0 voxel), averaged
/// over a small column: between the outer half-height edges of the housing at both ends.
double outerExtentMm(const VolumeView& volume, const Overview& box, int axis, const Index3& through,
                     int w) {
  const VolumeInfo& info = volume.info();
  Box line;
  for (std::size_t a = 0; a < 3; ++a) {
    if (static_cast<int>(a) == axis) {
      line.min[a] = box.lo[a];
      line.max[a] = box.hi[a];
    } else {
      line.min[a] = std::clamp<std::int64_t>(through[a] - w, 0, info.dims[a] - 1);
      line.max[a] = std::clamp<std::int64_t>(through[a] + w + 1, line.min[a] + 1, info.dims[a]);
    }
  }
  std::vector<float> values(at(line.voxelCount()));
  volume.readRegion(line, values, info.air_level);
  const std::int64_t len = line.size(at(axis));
  std::vector<double> profile(at(len), 0.0);
  const Index3 size{line.size(0), line.size(1), line.size(2)};
  const auto count = static_cast<double>(line.voxelCount()) / static_cast<double>(len);
  for (std::int64_t z = 0; z < size[2]; ++z) {
    for (std::int64_t y = 0; y < size[1]; ++y) {
      for (std::int64_t x = 0; x < size[0]; ++x) {
        const Index3 c{x, y, z};
        profile[at(c[at(axis)])] += values[at(x + size[0] * (y + size[1] * z))] / count;
      }
    }
  }
  const double pitch = info.voxel_size[at(axis)];
  const auto window = static_cast<std::size_t>(std::max(4.0, std::ceil(0.5 / pitch)));
  const auto threshold = static_cast<double>(info.threshold);
  const auto air = static_cast<double>(info.air_level);
  const Band low = firstBand(profile, threshold, air, window);
  std::vector<double> reversed(profile.rbegin(), profile.rend());
  const Band high = firstBand(reversed, threshold, air, window);
  const double extent = (static_cast<double>(len - 1) - high.outer - low.outer) * pitch;
  return std::isnan(extent) ? static_cast<double>(len) * pitch : extent;
}

/// Outer size of a cylindrical cell across (from `cross`) and along the axis through the
/// winding, between the core and the wall (from `along`).
void cylinderOuterSize(const VolumeView& cross, const VolumeView& along, const CylinderData& data,
                       BatteryResult& result) {
  const VoxelSize& pitch = cross.info().voxel_size;
  const int a = data.axis;
  const BatterySection& mid = result.sections[result.sections.size() / 2];
  Index3 through{};
  through[at(a)] = static_cast<std::int64_t>(std::lround(mid.position_mm[0] / pitch[at(a)]));
  through[at(data.u)] =
      static_cast<std::int64_t>(std::lround(mid.center_mm[0] / pitch[at(data.u)]));
  through[at(data.v)] =
      static_cast<std::int64_t>(std::lround(mid.center_mm[1] / pitch[at(data.v)]));
  result.outer_size_mm[at(data.u)] = outerExtentMm(cross, data.box, data.u, through, 1);
  result.outer_size_mm[at(data.v)] = outerExtentMm(cross, data.box, data.v, through, 1);
  const double r = 0.5 * (data.core[result.sections.size() / 2].front() +
                          data.inner[result.sections.size() / 2].front());
  through[at(data.u)] =
      static_cast<std::int64_t>(std::lround((mid.center_mm[0] + r) / pitch[at(data.u)]));
  result.outer_size_mm[at(a)] = outerExtentMm(along, data.box, a, through, 1);
  result.length_mm = result.outer_size_mm[at(a)];
}

// ---------------------------------------------------------------------------------------------
// Images.

std::array<std::uint8_t, 3> greyPixel(double value, double lo, double hi) {
  const double t = std::clamp((value - lo) / std::max(1e-9, hi - lo), 0.0, 1.0);
  const auto g = static_cast<std::uint8_t>(std::lround(255.0 * t));
  return {g, g, g};
}

/// Drawing on a BatteryImage.
struct Canvas {
  BatteryImage& image;

  void set(std::int64_t x, std::int64_t y, const std::array<std::uint8_t, 3>& c) const {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
      return;
    }
    for (std::size_t k = 0; k < 3; ++k) {
      image.rgb[at(3 * (x + image.width * y)) + k] = c[k];
    }
  }
  void dot(double x, double y, int radius, const std::array<std::uint8_t, 3>& c) const {
    const auto cx = static_cast<std::int64_t>(std::lround(x));
    const auto cy = static_cast<std::int64_t>(std::lround(y));
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        if (dx * dx + dy * dy <= radius * radius) {
          set(cx + dx, cy + dy, c);
        }
      }
    }
  }
  void ring(double x, double y, double radius, const std::array<std::uint8_t, 3>& c) const {
    const int steps = std::max(16, static_cast<int>(radius * 8.0));
    for (int k = 0; k < steps; ++k) {
      const double t = 2.0 * kPi * k / steps;
      set(std::lround(x + radius * std::cos(t)), std::lround(y + radius * std::sin(t)), c);
    }
  }
};

constexpr std::array<std::uint8_t, 3> kCathodeColor{230, 60, 50};
constexpr std::array<std::uint8_t, 3> kAnodeColor{60, 140, 240};
constexpr std::array<std::uint8_t, 3> kParticleColor{255, 210, 0};
constexpr std::array<std::uint8_t, 3> kVoidColor{230, 40, 230};
/// Longest side of an image of a plane; larger planes are shown every n-th voxel.
constexpr std::int64_t kMaxImageSide = 2048;

double displayHigh(const BatteryLevels& l) {
  return std::max({l.cathode, l.anode, std::isnan(l.housing) ? 0.0 : l.housing});
}

/// A plane of the dataset as an image, u to the right, v downwards, every `stride`-th voxel.
BatteryImage planeImage(const std::string& name, const Plane& plane, const BatteryLevels& levels,
                        std::int64_t& stride) {
  stride =
      std::max<std::int64_t>(1, (std::max(plane.nu, plane.nv) + kMaxImageSide - 1) / kMaxImageSide);
  BatteryImage image;
  image.name = name;
  image.width = (plane.nu + stride - 1) / stride;
  image.height = (plane.nv + stride - 1) / stride;
  image.rgb.assign(at(3 * image.width * image.height), 0);
  image.pixel_width_mm = plane.pu * static_cast<double>(stride);
  image.pixel_height_mm = plane.pv * static_cast<double>(stride);
  const double high = displayHigh(levels);
  const Canvas canvas{image};
  for (std::int64_t j = 0; j < image.height; ++j) {
    for (std::int64_t i = 0; i < image.width; ++i) {
      canvas.set(i, j, greyPixel(plane.voxel(i * stride, j * stride), levels.air, high));
    }
  }
  return image;
}

/// Pixel of a position in mm in an image of `plane`.
std::array<double, 2> planePixel(const Plane& plane, std::int64_t stride, double u_mm,
                                 double v_mm) {
  return {(u_mm / plane.pu - static_cast<double>(plane.u0)) / static_cast<double>(stride),
          (v_mm / plane.pv - static_cast<double>(plane.v0)) / static_cast<double>(stride)};
}

void markFindings(BatteryImage& image, const BatteryResult& result, const Plane& plane,
                  std::int64_t stride) {
  const Canvas canvas{image};
  for (const auto* list : {&result.particles, &result.voids}) {
    const auto color = list == &result.particles ? kParticleColor : kVoidColor;
    for (const CellFinding& f : *list) {
      const auto p =
          planePixel(plane, stride, f.center_mm[at(plane.u_axis)], f.center_mm[at(plane.v_axis)]);
      const double r = std::max(4.0, 0.5 * f.size_mm / image.pixel_width_mm + 3.0);
      canvas.ring(p[0], p[1], r, color);
      canvas.ring(p[0], p[1], r + 1.0, color);
    }
  }
}

/// Image of a section across an edge: across to the right, the edge at the top, with the ends of
/// the electrode layers of that section.
BatteryImage sectionImage(const SectionImage& section, const BatteryResult& result, int edge) {
  BatteryImage image;
  const BatteryEdge& e = result.edges[at(edge)];
  image.name = "overhang_" + std::string(1, "xyz"[e.axis]) + (e.side == 0 ? "_min" : "_max");
  image.width = section.across;
  image.height = section.along;
  image.rgb.assign(at(3 * section.across * section.along), 0);
  image.pixel_width_mm = section.across_step_mm;
  image.pixel_height_mm = std::abs(section.along_step_mm);
  const Canvas canvas{image};
  const double high = displayHigh(result.levels);
  for (std::int64_t s = 0; s < section.along; ++s) {
    for (std::int64_t a = 0; a < section.across; ++a) {
      canvas.set(a, section.along - 1 - s, greyPixel(section.value(a, s), result.levels.air, high));
    }
  }
  const OverhangSample& worst = result.overhang[at(e.worst)];
  for (const OverhangSample& sample : result.overhang) {
    if (sample.edge != edge || sample.section != worst.section) {
      continue;
    }
    const double x = (sample.across_mm - section.across_from_mm) / section.across_step_mm;
    const double xa = (sample.anode_across_mm - section.across_from_mm) / section.across_step_mm;
    const auto yc = static_cast<double>(section.along - 1) -
                    (sample.cathode_end_mm - section.along_from_mm) / section.along_step_mm;
    const auto ya = static_cast<double>(section.along - 1) -
                    (sample.anode_end_mm - section.along_from_mm) / section.along_step_mm;
    const int r = sample.layer == worst.layer ? 2 : 1;
    canvas.dot(x, yc, r, kCathodeColor);
    canvas.dot(xa, ya, r, kAnodeColor);
  }
  return image;
}

/// What the analysis keeps for the images.
struct Extras {
  std::vector<LayerPoint> points;
  std::vector<SectionImage> worst;
  int cross_axis = 2;  // the cross-section is normal to this axis
  std::int64_t cross_index = 0;
  int long_axis = 0;  // the longitudinal section is normal to this one
  std::int64_t long_index = 0;
  Overview box;
};

/// Renders the images: the cross-section from `cross`, the longitudinal section from `along`.
void renderImages(const VolumeView& cross_view, const VolumeView& along, const Extras& extras,
                  BatteryResult& result) {
  const TelemetryPhase phase("images");
  const float air = cross_view.info().air_level;
  std::int64_t stride = 1;
  const Plane cross = readPlane(cross_view, extras.cross_axis, extras.cross_index, extras.box.lo,
                                extras.box.hi, air);
  BatteryImage image = planeImage("cross_section", cross, result.levels, stride);
  {
    const Canvas canvas{image};
    for (const LayerPoint& point : extras.points) {
      const auto p = planePixel(cross, stride, point.mm[0], point.mm[1]);
      canvas.set(std::lround(p[0]), std::lround(p[1]), point.cathode ? kCathodeColor : kAnodeColor);
    }
  }
  markFindings(image, result, cross, stride);
  result.images.push_back(std::move(image));
  const Plane longitudinal =
      readPlane(along, extras.long_axis, extras.long_index, extras.box.lo, extras.box.hi, air);
  image = planeImage("longitudinal", longitudinal, result.levels, stride);
  markFindings(image, result, longitudinal, stride);
  result.images.push_back(std::move(image));
  for (std::size_t e = 0; e < extras.worst.size() && e < result.edges.size(); ++e) {
    if (!extras.worst[e].values.empty() && result.edges[e].worst >= 0) {
      result.images.push_back(sectionImage(extras.worst[e], result, static_cast<int>(e)));
    }
  }
}

BatteryResult analyze(const Dataset& dataset, const BatteryOptions& options) {
  const DatasetInfo& info = dataset.info();
  const VolumeView volume = viewOf(dataset);
  BatteryResult result;
  result.dims = info.dims;
  result.voxel_size = info.voxel_size;
  result.levels.air = info.air_level;
  result.min_overhang_mm = kNaN;
  const VoxelSize& pitch = info.voxel_size;
  Extras extras;
  Overview box;
  {
    const TelemetryPhase phase("overview");
    box = overview(dataset, options, result.warnings);
  }
  extras.box = box;
  result.type = box.type;
  result.axis = box.axis;
  for (std::size_t a = 0; a < 3; ++a) {
    result.bounds_min_mm[a] = static_cast<double>(box.lo[a]) * pitch[a];
    result.bounds_max_mm[a] = static_cast<double>(box.hi[a] - 1) * pitch[a];
  }
  // Until measured: the length of the bounding box.
  result.length_mm =
      static_cast<double>(box.hi[at(box.axis)] - box.lo[at(box.axis)]) * pitch[at(box.axis)];
  InRegion in_region;
  if (box.type == CellType::kCylindrical) {
    CylinderData data;
    data.axis = box.axis;
    std::tie(data.u, data.v) = planeAxes(box.axis);
    data.box = box;
    cylinderSections(volume, options, data, result);
    cylinderOuterSize(volume, volume, data, result);
    cylinderOverhang(volume, options, data, result, extras.worst);
    summarizeEdges(result);
    cylinderEnds(data, result);
    extras.points = data.points;
    extras.cross_axis = box.axis;
    extras.cross_index = static_cast<std::int64_t>(std::lround(
        result.sections[result.sections.size() / 2].position_mm[0] / pitch[at(box.axis)]));
    // Longitudinal: normal to the first in-plane axis, through the centre.
    extras.long_axis = data.u;
    const double centre_pos =
        0.5 * (result.bounds_min_mm[at(box.axis)] + result.bounds_max_mm[at(box.axis)]);
    extras.long_index =
        static_cast<std::int64_t>(std::lround(data.centre(centre_pos)[0] / pitch[at(data.u)]));
    in_region = cylinderRegion(data, pitch);
  } else {
    StackData data;
    data.n = box.axis;
    const auto [p, q] = planeAxes(box.axis);
    const auto ep = static_cast<double>(box.hi[at(p)] - box.lo[at(p)]) * pitch[at(p)];
    const auto eq = static_cast<double>(box.hi[at(q)] - box.lo[at(q)]) * pitch[at(q)];
    data.u = ep >= eq ? p : q;
    data.v = ep >= eq ? q : p;
    data.box = box;
    stackSections(volume, options, data, result);
    {
      const Index3 through{(box.lo[0] + box.hi[0]) / 2, (box.lo[1] + box.hi[1]) / 2,
                           (box.lo[2] + box.hi[2]) / 2};
      for (int a = 0; a < 3; ++a) {
        result.outer_size_mm[at(a)] = outerExtentMm(volume, box, a, through, 1);
      }
      result.length_mm = result.outer_size_mm[at(box.axis)];
    }
    stackOverhang(volume, options, data, result, extras.worst);
    summarizeEdges(result);
    // The electrode stack: between the outermost layers, inside the anode edges.
    std::array<double, 6> limits{};
    for (std::size_t a = 0; a < 3; ++a) {
      const double inset = 0.05 * (result.bounds_max_mm[a] - result.bounds_min_mm[a]);
      limits[2 * a] = result.bounds_min_mm[a] + inset;
      limits[2 * a + 1] = result.bounds_max_mm[a] - inset;
    }
    std::array<double, 6> void_limits = limits;
    for (const BatteryEdge& edge : result.edges) {
      if (edge.samples > 0) {
        const double inwards = (edge.side == 0 ? 2.0 : -2.0) * pitch[at(edge.axis)];
        limits[at(2 * edge.axis + edge.side)] = edge.anode_edge_mm + inwards;
        void_limits[at(2 * edge.axis + edge.side)] = edge.cathode_edge_mm + inwards;
      }
    }
    if (!std::isnan(data.first_layer)) {
      limits[2 * at(data.n)] = data.first_layer - data.half_pitch;
      limits[at(2 * data.n + 1)] = data.last_layer + data.half_pitch;
      void_limits[2 * at(data.n)] = data.first_layer;
      void_limits[at(2 * data.n + 1)] = data.last_layer;
    }
    in_region = [limits, void_limits, pitch](const Index3& v, bool bright) {
      const std::array<double, 6>& l = bright ? limits : void_limits;
      for (std::size_t a = 0; a < 3; ++a) {
        const auto mm = static_cast<double>(v[a]) * pitch[a];
        if (mm < l[2 * a] || mm > l[2 * a + 1]) {
          return false;
        }
      }
      return true;
    };
    extras.cross_axis = data.v;
    extras.cross_index = (box.lo[at(data.v)] + box.hi[at(data.v)]) / 2;
    extras.long_axis = data.u;
    extras.long_index = (box.lo[at(data.u)] + box.hi[at(data.u)]) / 2;
  }
  summarizeSections(result);
  setThresholds(result, options);
  findParticlesAndVoids(dataset, in_region, result, options);
  placeFindings(result);
  renderImages(volume, volume, extras, result);
  return result;
}

// ---------------------------------------------------------------------------------------------
// Cylindrical cells given as section images.

/// The section images of a cell placed in a volume: x and y as in the cross-sections, z along
/// the rows of the longitudinal sections from the bottom, all with the pixel size.
struct ImageCell {
  std::vector<const CellImage*> along;    // longitudinal sections, ascending angle
  std::vector<const CellImage*> cross;    // cross-sections, ascending position
  std::vector<std::int64_t> cross_index;  // z index of each cross-section
  std::vector<std::size_t> nearest;       // per z index: the nearest cross-section
  std::vector<double> axis_column;        // per longitudinal section: column of the axis
  double pixel = 1.0;
  Index3 dims{};
  float air = 0.0F;
  float threshold = 0.0F;

  [[nodiscard]] std::int64_t height() const { return dims[2]; }
};

/// Column of the axis in a longitudinal section: the middle between the outer surfaces over the
/// middle third of its rows.
double axisColumn(const CellImage& image, float threshold) {
  std::vector<double> centres;
  const auto above = [&](std::int64_t row, std::int64_t col) {
    return static_cast<float>(image.values[at(col + image.width * row)]) >= threshold;
  };
  for (std::int64_t row = image.height / 3; row < 2 * image.height / 3; row += 8) {
    std::int64_t first = -1;
    std::int64_t last = -1;
    for (std::int64_t col = 0; col + 1 < image.width; ++col) {
      if (above(row, col) && above(row, col + 1)) {
        first = first < 0 ? col : first;
        last = col + 1;
      }
    }
    if (first >= 0) {
      centres.push_back(0.5 * static_cast<double>(first + last));
    }
  }
  if (centres.empty()) {
    throw std::runtime_error("No cell found in the longitudinal section at " +
                             std::to_string(image.position) + " degrees");
  }
  return median(centres);
}

ImageCell placeImages(const CellImages& images, const BatteryOptions& options,
                      std::vector<std::string>& warnings) {
  if (images.longitudinal.empty() || images.cross.size() < 2) {
    throw std::invalid_argument(
        "A cell given as images needs longitudinal sections and at least two cross-sections");
  }
  ImageCell cell;
  for (const CellImage& image : images.longitudinal) {
    cell.along.push_back(&image);
  }
  for (const CellImage& image : images.cross) {
    cell.cross.push_back(&image);
  }
  std::sort(cell.along.begin(), cell.along.end(),
            [](const CellImage* a, const CellImage* b) { return a->position < b->position; });
  std::sort(cell.cross.begin(), cell.cross.end(),
            [](const CellImage* a, const CellImage* b) { return a->position < b->position; });
  const CellImage& first_along = *cell.along.front();
  const CellImage& first_cross = *cell.cross.front();
  for (const CellImage* image : cell.along) {
    if (image->width != first_along.width || image->height != first_along.height) {
      throw std::invalid_argument("The longitudinal sections differ in size");
    }
  }
  for (const CellImage* image : cell.cross) {
    if (image->width != first_cross.width || image->height != first_cross.height) {
      throw std::invalid_argument("The cross-sections differ in size");
    }
  }
  if (first_along.height < 16 || first_cross.width < 16 || first_cross.height < 16) {
    throw std::invalid_argument("The section images are too small");
  }
  cell.dims = {first_cross.width, first_cross.height, first_along.height};
  const double span = cell.cross.back()->position - cell.cross.front()->position;
  if (options.image_pixel_mm) {
    cell.pixel = *options.image_pixel_mm;
  } else {
    cell.pixel = span / static_cast<double>(cell.height() - 1);
    std::ostringstream text;
    text << "The pixel size, " << std::setprecision(4) << cell.pixel * 1000.0
         << " µm, is taken from the positions of the cross-sections, which are assumed to span "
            "the height of the longitudinal sections";
    warnings.push_back(text.str());
  }
  if (!(cell.pixel > 0.0) || !std::isfinite(cell.pixel)) {
    throw std::invalid_argument("The pixel size of the section images must be positive");
  }
  for (const CellImage* image : cell.cross) {
    cell.cross_index.push_back(std::clamp<std::int64_t>(
        std::lround((image->position - cell.cross.front()->position) / cell.pixel), 0,
        cell.height() - 1));
  }
  cell.nearest.resize(at(cell.height()));
  std::size_t c = 0;
  for (std::int64_t k = 0; k < cell.height(); ++k) {
    while (c + 1 < cell.cross_index.size() &&
           std::abs(cell.cross_index[c + 1] - k) <= std::abs(cell.cross_index[c] - k)) {
      ++c;
    }
    cell.nearest[at(k)] = c;
  }
  // Air and threshold from the middle cross-section.
  const CellImage& middle = *cell.cross[cell.cross.size() / 2];
  detail::Histogram histogram(detail::kHistogramBins, 0);
  for (const std::uint16_t value : middle.values) {
    ++histogram[value];
  }
  const detail::ThresholdResult split = detail::airThreshold(histogram);
  cell.air = split.air_level;
  cell.threshold = split.threshold;
  for (const CellImage* image : cell.along) {
    cell.axis_column.push_back(axisColumn(*image, cell.threshold));
  }
  return cell;
}

VolumeView crossView(const ImageCell& cell) {
  std::vector<std::int64_t> planes = cell.cross_index;
  planes.erase(std::unique(planes.begin(), planes.end()), planes.end());
  return VolumeView({cell.dims, VoxelSize(cell.pixel), cell.air, cell.threshold, planes},
                    [&cell](const Box& box, std::span<float> out, float fill) {
                      std::size_t o = 0;
                      for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
                        const CellImage* image =
                            z < 0 || z >= cell.height() ? nullptr : cell.cross[cell.nearest[at(z)]];
                        for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
                          for (std::int64_t x = box.min[0]; x < box.max[0]; ++x) {
                            const bool inside = image != nullptr && x >= 0 && y >= 0 &&
                                                x < image->width && y < image->height;
                            out[o++] =
                                inside ? static_cast<float>(image->values[at(x + image->width * y)])
                                       : fill;
                          }
                        }
                      }
                    });
}

/// The longitudinal section at the first angle as a volume: the same in every plane normal to y,
/// its axis at the centre `centre_x` (mm) of the cross-sections.
VolumeView alongView(const ImageCell& cell, double centre_x) {
  const CellImage& image = *cell.along.front();
  const auto shift =
      static_cast<std::int64_t>(std::lround(cell.axis_column.front() - centre_x / cell.pixel));
  return VolumeView(
      {cell.dims, VoxelSize(cell.pixel), cell.air, cell.threshold, {}},
      [&image, shift](const Box& box, std::span<float> out, float fill) {
        std::size_t o = 0;
        for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
          const std::int64_t row = image.height - 1 - z;
          for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
            for (std::int64_t x = box.min[0]; x < box.max[0]; ++x) {
              const std::int64_t col = x + shift;
              const bool inside = row >= 0 && row < image.height && col >= 0 && col < image.width;
              out[o++] =
                  inside ? static_cast<float>(image.values[at(col + image.width * row)]) : fill;
            }
          }
        }
      });
}

/// Bounding box: across from the middle cross-section, along the axis from the longitudinal
/// sections.
Overview imageOverview(const ImageCell& cell) {
  const auto bounds = [](const std::vector<std::int64_t>& counts) {
    const std::int64_t most = *std::max_element(counts.begin(), counts.end());
    const std::int64_t enough = std::max<std::int64_t>(2, most / 50);
    std::int64_t lo = -1;
    std::int64_t hi = -1;
    for (std::size_t i = 0; i < counts.size(); ++i) {
      if (counts[i] >= enough) {
        lo = lo < 0 ? static_cast<std::int64_t>(i) : lo;
        hi = static_cast<std::int64_t>(i) + 1;
      }
    }
    if (lo < 0) {
      throw std::invalid_argument("The section images show no material above the air threshold");
    }
    return std::pair{lo, hi};
  };
  const CellImage& middle = *cell.cross[cell.cross.size() / 2];
  std::vector<std::int64_t> cols(at(middle.width), 0);
  std::vector<std::int64_t> rows(at(middle.height), 0);
  for (std::int64_t y = 0; y < middle.height; ++y) {
    for (std::int64_t x = 0; x < middle.width; ++x) {
      if (static_cast<float>(middle.values[at(x + middle.width * y)]) >= cell.threshold) {
        ++cols[at(x)];
        ++rows[at(y)];
      }
    }
  }
  std::vector<std::int64_t> levels(at(cell.height()), 0);
  for (const CellImage* image : cell.along) {
    for (std::int64_t row = 0; row < image->height; ++row) {
      for (std::int64_t col = 0; col < image->width; ++col) {
        if (static_cast<float>(image->values[at(col + image->width * row)]) >= cell.threshold) {
          ++levels[at(image->height - 1 - row)];
        }
      }
    }
  }
  Overview o;
  const std::array<std::pair<std::int64_t, std::int64_t>, 3> b{bounds(cols), bounds(rows),
                                                               bounds(levels)};
  for (std::size_t a = 0; a < 3; ++a) {
    o.lo[a] = std::max<std::int64_t>(0, b[a].first - 2);
    o.hi[a] = std::min(cell.dims[a], b[a].second + 2);
  }
  o.type = CellType::kCylindrical;
  o.axis = 2;
  return o;
}

/// Overhang at both ends from the longitudinal sections, both halves of each.
void imageOverhang(const ImageCell& cell, const BatteryOptions& options, const CylinderData& data,
                   BatteryResult& result, std::vector<SectionImage>& worst) {
  const TelemetryPhase phase("overhang");
  double core_min = kNaN;
  double inner_max = kNaN;
  for (std::size_t k = 0; k < data.core.size(); ++k) {
    for (std::size_t j = 0; j < data.core[k].size(); ++j) {
      core_min = nanMin(core_min, data.core[k][j]);
      inner_max = nanMax(inner_max, data.inner[k][j]);
    }
  }
  const double across_from = std::max(0.0, core_min - 2.0 * data.step);
  const auto across = static_cast<std::int64_t>(std::ceil((inner_max - across_from) / data.step));
  const std::int64_t length = data.box.hi[2] - data.box.lo[2];
  const std::int64_t depth = std::max<std::int64_t>(8, length / 4);
  // Directions: the right half of each section at its angle, the left half opposite.
  struct Direction {
    std::size_t image = 0;
    double sign = 1.0;
    double angle = 0.0;
  };
  std::vector<Direction> directions;
  for (std::size_t i = 0; i < cell.along.size(); ++i) {
    for (int half = 0; half < 2; ++half) {
      double angle = std::fmod(cell.along[i]->position + 180.0 * half, 360.0);
      angle = angle < 0.0 ? angle + 360.0 : angle;
      directions.push_back({i, half == 0 ? 1.0 : -1.0, angle});
    }
  }
  std::vector<double> angles;
  angles.reserve(directions.size());
  for (const Direction& d : directions) {
    angles.push_back(d.angle);
  }
  const std::size_t bins = data.inner.front().size();
  bool unfinished = false;
  std::vector<int> hidden_edges;
  for (int side = 0; side < 2; ++side) {
    const int edge = static_cast<int>(result.edges.size());
    result.edges.push_back({2, side, edgeName(2, side)});
    const std::int64_t start = side == 0 ? data.box.lo[2] + depth : data.box.hi[2] - 1 - depth;
    const std::int64_t rows = depth + 1;
    std::vector<SectionImage> images(directions.size());
    tbb::parallel_for(std::size_t{0}, directions.size(), [&](std::size_t j) {
      const Direction& d = directions[j];
      const CellImage& source = *cell.along[d.image];
      const double axis = cell.axis_column[d.image];
      SectionImage& image = images[j];
      image.across = across;
      image.along = rows;
      image.across_from_mm = across_from;
      image.across_step_mm = data.step;
      image.along_from_mm = static_cast<double>(start) * cell.pixel;
      image.along_step_mm = (side == 0 ? -1.0 : 1.0) * cell.pixel;
      image.values.assign(at(across * rows), cell.air);
      const auto bin =
          static_cast<std::size_t>(std::lround(d.angle / 360.0 * static_cast<double>(bins))) % bins;
      double wall = kNaN;
      for (const auto& inner : data.inner) {
        wall = nanMin(wall, inner[bin]);
      }
      image.valid_across = std::clamp<std::int64_t>(
          static_cast<std::int64_t>(std::floor((wall - across_from) / data.step)) - 1, 0, across);
      for (std::int64_t s = 0; s < rows; ++s) {
        const std::int64_t row = source.height - 1 - (start + (side == 0 ? -s : s));
        if (row < 0 || row >= source.height) {
          continue;
        }
        const std::uint16_t* line = source.values.data() + at(source.width * row);
        for (std::int64_t i = 0; i < across; ++i) {
          const double r = (across_from + static_cast<double>(i) * data.step) / cell.pixel;
          const double col = axis + d.sign * r;
          const double base = std::floor(col);
          const auto c0 = static_cast<std::int64_t>(base);
          if (c0 < 0 || c0 + 1 >= source.width) {
            continue;
          }
          const double t = col - base;
          image.values[at(i + across * s)] = static_cast<float>(
              (1.0 - t) * static_cast<double>(line[c0]) + t * static_cast<double>(line[c0 + 1]));
        }
      }
    });
    edgeOverhang(images, angles, edge, options, result, unfinished, hidden_edges, worst);
  }
  warnOverhang(result, unfinished, hidden_edges);
}

/// The pixels at or below `gas` in columns [from, to) connected to the seed pixels (4-connected):
/// the hollow core, not a void.
std::vector<char> darkFrom(const CellImage& image, std::int64_t from, std::int64_t to, float gas,
                           const std::vector<std::int64_t>& seeds) {
  std::vector<char> mask(at(image.width * image.height), 0);
  const auto dark = [&](std::int64_t i) { return static_cast<float>(image.values[at(i)]) <= gas; };
  std::vector<std::int64_t> stack;
  for (const std::int64_t seed : seeds) {
    if (dark(seed) && mask[at(seed)] == 0) {
      mask[at(seed)] = 1;
      stack.push_back(seed);
    }
  }
  while (!stack.empty()) {
    const std::int64_t i = stack.back();
    stack.pop_back();
    const std::int64_t x = i % image.width;
    const std::int64_t y = i / image.width;
    const std::array<std::array<std::int64_t, 2>, 4> next{
        {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}}};
    for (const auto& n : next) {
      if (n[0] < from || n[0] >= to || n[1] < 0 || n[1] >= image.height) {
        continue;
      }
      const std::int64_t j = n[0] + image.width * n[1];
      if (mask[at(j)] == 0 && dark(j)) {
        mask[at(j)] = 1;
        stack.push_back(j);
      }
    }
  }
  return mask;
}

/// Particles and voids as their sections in the images; one finding seen in several images is
/// kept once, from the image where it is largest.
void imageFindings(const ImageCell& cell, const InRegion& in_region, const CylinderData& data,
                   BatteryResult& result, const BatteryOptions& options) {
  const TelemetryPhase phase("particles and voids");
  const auto particle = static_cast<float>(result.particle_threshold);
  const auto gas = static_cast<float>(result.void_threshold);
  const VoxelSize pitch(cell.pixel);
  using Found = std::vector<std::pair<std::uint64_t, float>>;
  // Jobs: every cross-section, and each half of every longitudinal section.
  const std::size_t n_cross = cell.cross.size();
  const std::size_t n_jobs = n_cross + 2 * cell.along.size();
  std::vector<std::vector<CellFinding>> bright(n_jobs);
  std::vector<std::vector<CellFinding>> dark(n_jobs);
  std::atomic<bool> too_many{false};
  const auto finish = [&](std::size_t job, Found& b, Found& d, std::int64_t width,
                          std::int64_t height, const auto& place) {
    if (b.size() + d.size() > kMaxFindingVoxels / 16) {
      too_many = true;
      return;
    }
    const Index3 dims{width, height, 1};
    for (int kind = 0; kind < 2; ++kind) {
      std::vector<CellFinding> found =
          components(std::move(kind == 0 ? b : d), dims, pitch,
                     kind == 0 ? options.min_particle_voxels : options.min_void_voxels, kind == 0);
      for (CellFinding& f : found) {
        const double area = static_cast<double>(f.voxels) * cell.pixel * cell.pixel;
        f.volume_mm3 = kNaN;
        f.equivalent_diameter_mm = 2.0 * std::sqrt(area / kPi);
        f.center_mm = place(f.center_mm[0] / cell.pixel, f.center_mm[1] / cell.pixel);
      }
      (kind == 0 ? bright : dark)[job] = std::move(found);
    }
  };
  tbb::parallel_for(std::size_t{0}, n_jobs, [&](std::size_t job) {
    Found b;
    Found d;
    if (job < n_cross) {
      const CellImage& image = *cell.cross[job];
      const std::int64_t k = cell.cross_index[job];
      const auto centre = data.centre(static_cast<double>(k) * cell.pixel);
      const std::vector<char> core = darkFrom(image, 0, image.width, gas,
                                              {std::lround(centre[0] / cell.pixel) +
                                               image.width * std::lround(centre[1] / cell.pixel)});
      for (std::int64_t y = 0; y < image.height; ++y) {
        for (std::int64_t x = 0; x < image.width; ++x) {
          const auto value = static_cast<float>(image.values[at(x + image.width * y)]);
          if ((value < particle && value > gas) || core[at(x + image.width * y)] != 0 ||
              !in_region({x, y, k}, value >= particle)) {
            continue;
          }
          (value >= particle ? b : d)
              .emplace_back(static_cast<std::uint64_t>(x + image.width * y), value);
        }
      }
      finish(job, b, d, image.width, image.height, [&](double x, double y) {
        return Vec3{x * cell.pixel, y * cell.pixel, static_cast<double>(k) * cell.pixel};
      });
      return;
    }
    const std::size_t i = (job - n_cross) / 2;
    const double sign = (job - n_cross) % 2 == 0 ? 1.0 : -1.0;
    const CellImage& image = *cell.along[i];
    const double axis = cell.axis_column[i];
    const double angle = (image.position + (sign > 0.0 ? 0.0 : 180.0)) * kPi / 180.0;
    const double cos_a = std::cos(angle);
    const double sin_a = std::sin(angle);
    const auto to3d = [&](double col, double row) {
      const double r = sign * (col - axis) * cell.pixel;
      const double z = (static_cast<double>(image.height - 1) - row) * cell.pixel;
      const auto c = data.centre(z);
      return Vec3{c[0] + r * cos_a, c[1] + r * sin_a, z};
    };
    const auto from = static_cast<std::int64_t>(sign > 0.0 ? std::ceil(axis) : 0.0);
    const auto to =
        static_cast<std::int64_t>(sign > 0.0 ? static_cast<double>(image.width) : std::ceil(axis));
    std::vector<std::int64_t> seeds;
    const std::int64_t seed_col = sign > 0.0 ? from : to - 1;
    if (seed_col >= 0 && seed_col < image.width) {
      for (std::int64_t row = 0; row < image.height; ++row) {
        seeds.push_back(seed_col + image.width * row);
      }
    }
    const std::vector<char> core = darkFrom(image, from, to, gas, seeds);
    for (std::int64_t row = 0; row < image.height; ++row) {
      for (std::int64_t col = from; col < to; ++col) {
        const auto value = static_cast<float>(image.values[at(col + image.width * row)]);
        if ((value < particle && value > gas) || core[at(col + image.width * row)] != 0) {
          continue;
        }
        const Vec3 p = to3d(static_cast<double>(col), static_cast<double>(row));
        const Index3 v{std::lround(p[0] / cell.pixel), std::lround(p[1] / cell.pixel),
                       std::lround(p[2] / cell.pixel)};
        if (!in_region(v, value >= particle)) {
          continue;
        }
        (value >= particle ? b : d)
            .emplace_back(static_cast<std::uint64_t>(col + image.width * row), value);
      }
    }
    finish(job, b, d, image.width, image.height, to3d);
  });
  if (too_many) {
    throw std::runtime_error(
        "Too many pixels above the particle threshold or below the void threshold in the "
        "electrode stack; set the thresholds by hand");
  }
  const auto merge = [](std::vector<std::vector<CellFinding>>& per_job) {
    std::vector<CellFinding> all;
    for (auto& found : per_job) {
      all.insert(all.end(), found.begin(), found.end());
    }
    std::sort(all.begin(), all.end(),
              [](const CellFinding& a, const CellFinding& b) { return a.voxels > b.voxels; });
    std::vector<CellFinding> kept;
    for (const CellFinding& f : all) {
      const bool seen = std::any_of(kept.begin(), kept.end(), [&](const CellFinding& k) {
        const double d =
            std::hypot(f.center_mm[0] - k.center_mm[0], f.center_mm[1] - k.center_mm[1],
                       f.center_mm[2] - k.center_mm[2]);
        return d < 0.5 * (f.size_mm + k.size_mm);
      });
      if (!seen) {
        kept.push_back(f);
      }
    }
    for (std::size_t i = 0; i < kept.size(); ++i) {
      kept[i].id = static_cast<int>(i + 1);
    }
    return kept;
  };
  result.particles = merge(bright);
  result.voids = merge(dark);
  separateStructures(result, options);
}

BatteryResult analyzeImages(const CellImages& images, const BatteryOptions& options) {
  if (options.cell_type && *options.cell_type != CellType::kCylindrical) {
    throw std::invalid_argument("Cells given as section images must be cylindrical");
  }
  BatteryResult result;
  result.from_images = true;
  const ImageCell cell = placeImages(images, options, result.warnings);
  const VolumeView cross = crossView(cell);
  result.dims = cell.dims;
  result.voxel_size = VoxelSize(cell.pixel);
  result.levels.air = cell.air;
  result.min_overhang_mm = kNaN;
  result.type = CellType::kCylindrical;
  result.axis = 2;
  Extras extras;
  CylinderData data;
  data.axis = 2;
  data.u = 0;
  data.v = 1;
  data.box = imageOverview(cell);
  extras.box = data.box;
  for (std::size_t a = 0; a < 3; ++a) {
    result.bounds_min_mm[a] = static_cast<double>(data.box.lo[a]) * cell.pixel;
    result.bounds_max_mm[a] = static_cast<double>(data.box.hi[a] - 1) * cell.pixel;
  }
  result.length_mm = static_cast<double>(data.box.hi[2] - data.box.lo[2]) * cell.pixel;
  cylinderSections(cross, options, data, result);
  const double centre_z = 0.5 * (result.bounds_min_mm[2] + result.bounds_max_mm[2]);
  const VolumeView along = alongView(cell, data.centre(centre_z)[0]);
  cylinderOuterSize(cross, along, data, result);
  imageOverhang(cell, options, data, result, extras.worst);
  summarizeEdges(result);
  cylinderEnds(data, result);
  extras.points = data.points;
  extras.cross_axis = 2;
  extras.cross_index = static_cast<std::int64_t>(
      std::lround(result.sections[result.sections.size() / 2].position_mm[0] / cell.pixel));
  extras.long_axis = 1;
  extras.long_index = static_cast<std::int64_t>(std::lround(data.centre(centre_z)[1] / cell.pixel));
  summarizeSections(result);
  setThresholds(result, options);
  imageFindings(cell, cylinderRegion(data, result.voxel_size), data, result, options);
  placeFindings(result);
  renderImages(cross, along, extras, result);
  return result;
}

nlohmann::json findingJson(const CellFinding& f, CellType type) {
  nlohmann::json j = {{"id", f.id},
                      {"center_mm", f.center_mm},
                      {"voxels", f.voxels},
                      {"volume_mm3", f.volume_mm3},
                      {"size_mm", f.size_mm},
                      {"equivalent_diameter_mm", f.equivalent_diameter_mm},
                      {"grey_value", f.extreme_value}};
  if (type == CellType::kCylindrical) {
    j["radius_mm"] = f.radius_mm;
    j["angle_deg"] = f.angle_deg;
  } else {
    j["normal_mm"] = f.radius_mm;
  }
  return j;
}

/// NaN as null, so that the JSON stays valid.
nlohmann::json number(double value) {
  return std::isfinite(value) ? nlohmann::json(value) : nlohmann::json();
}

}  // namespace

BatteryResult analyzeBattery(const Dataset& dataset, const BatteryOptions& options) {
  return analyze(dataset, options);
}

BatteryResult analyzeBatteryImages(const CellImages& images, const BatteryOptions& options) {
  return analyzeImages(images, options);
}

nlohmann::json toJson(const BatteryResult& result) {
  const bool cylindrical = result.type == CellType::kCylindrical;
  nlohmann::json sections = nlohmann::json::array();
  for (const BatterySection& s : result.sections) {
    nlohmann::json j = {
        {"outer_mm", number(s.outer_mm)},         {"min_wall_mm", number(s.min_wall_mm)},
        {"mean_wall_mm", number(s.mean_wall_mm)}, {"stack_mm", number(s.stack_mm)},
        {"cathode_layers", s.cathode_layers},     {"anode_layers", s.anode_layers},
        {"pitch_mm", number(s.pitch_mm)}};
    if (cylindrical) {
      j["position_mm"] = s.position_mm[0];
      j["center_mm"] = s.center_mm;
      j["out_of_roundness_mm"] = number(s.out_of_roundness_mm);
      j["dent_mm"] = number(s.dent_mm);
      j["core_diameter_mm"] = number(s.core_diameter_mm);
      j["core_out_of_roundness_mm"] = number(s.core_out_of_roundness_mm);
      j["core_offset_mm"] = number(s.core_offset_mm);
      nlohmann::json outer = nlohmann::json::array();
      nlohmann::json core = nlohmann::json::array();
      for (std::size_t k = 0; k < s.outer_radius_mm.size(); ++k) {
        outer.push_back(number(s.outer_radius_mm[k]));
        core.push_back(number(s.core_radius_mm[k]));
      }
      j["outer_radius_mm"] = outer;
      j["core_radius_mm"] = core;
    } else {
      j["position_mm"] = s.position_mm;
    }
    sections.push_back(j);
  }
  nlohmann::json edges = nlohmann::json::array();
  for (const BatteryEdge& e : result.edges) {
    edges.push_back({{"axis", e.axis},
                     {"side", e.side == 0 ? "min" : "max"},
                     {"name", e.name},
                     {"samples", e.samples},
                     {"min_mm", number(e.min_mm)},
                     {"mean_mm", number(e.mean_mm)},
                     {"max_mm", number(e.max_mm)},
                     {"std_mm", number(e.std_mm)},
                     {"worst", e.worst},
                     {"cathode_edge_spread_mm", number(e.cathode_edge_spread_mm)},
                     {"anode_edge_spread_mm", number(e.anode_edge_spread_mm)},
                     {"anode_edge_mm", number(e.anode_edge_mm)},
                     {"cathode_edge_mm", number(e.cathode_edge_mm)}});
  }
  nlohmann::json overhang = nlohmann::json::array();
  for (const OverhangSample& s : result.overhang) {
    overhang.push_back({{"edge", s.edge},
                        {cylindrical ? "angle_deg" : "position_mm", s.section},
                        {"layer", s.layer},
                        {"across_mm", s.across_mm},
                        {"cathode_end_mm", s.cathode_end_mm},
                        {"anode_end_mm", s.anode_end_mm},
                        {"overhang_mm", s.overhang_mm}});
  }
  nlohmann::json gaps = nlohmann::json::array();
  for (const LayerGap& g : result.gaps) {
    gaps.push_back({{"position_mm", g.position_mm},
                    {"size_mm", g.size_mm},
                    {"expected_mm", g.expected_mm},
                    {"section", g.section}});
  }
  nlohmann::json particles = nlohmann::json::array();
  for (const CellFinding& f : result.particles) {
    particles.push_back(findingJson(f, result.type));
  }
  nlohmann::json voids = nlohmann::json::array();
  for (const CellFinding& f : result.voids) {
    voids.push_back(findingJson(f, result.type));
  }
  const BatteryLevels& l = result.levels;
  return {{"type", cylindrical ? "cylindrical" : "stacked"},
          {"axis", result.axis},
          {"dims", result.dims},
          {"voxel_size_mm", result.voxel_size},
          {"levels",
           {{"air", l.air},
            {"gap", number(l.gap)},
            {"anode", number(l.anode)},
            {"cathode", number(l.cathode)},
            {"housing", number(l.housing)},
            {"electrodes_distinct", l.electrodes_distinct}}},
          {"bounds_min_mm", result.bounds_min_mm},
          {"bounds_max_mm", result.bounds_max_mm},
          {"outer_size_mm", result.outer_size_mm},
          {"length_mm", result.length_mm},
          {"cathode_layers", result.cathode_layers},
          {"anode_layers", result.anode_layers},
          {"pitch_mm", number(result.pitch_mm)},
          {"outer_mean_mm", number(result.outer_mean_mm)},
          {"outer_min_size_mm", number(result.outer_min_size_mm)},
          {"outer_max_size_mm", number(result.outer_max_size_mm)},
          {"max_out_of_roundness_mm", number(result.max_out_of_roundness_mm)},
          {"max_dent_mm", number(result.max_dent_mm)},
          {"min_wall_mm", number(result.min_wall_mm)},
          {"max_core_out_of_roundness_mm", number(result.max_core_out_of_roundness_mm)},
          {"min_overhang_mm", number(result.min_overhang_mm)},
          {"particle_threshold", result.particle_threshold},
          {"void_threshold", result.void_threshold},
          {"sections", sections},
          {"edges", edges},
          {"overhang", overhang},
          {"gaps", gaps},
          {"particles", particles},
          {"voids", voids},
          {"bright_structures", result.bright_structures},
          {"from_images", result.from_images},
          {"warnings", result.warnings}};
}

void writeBatteryResult(const BatteryResult& result, const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
  writeJson(dir / "battery.json", toJson(result));
  for (const BatteryImage& image : result.images) {
    // Small sections are enlarged, pixel by pixel, so that the layers can be seen.
    const std::int64_t f = std::max<std::int64_t>(1, 640 / std::max(image.width, image.height));
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(3 * image.width * image.height * f * f));
    for (std::int64_t y = 0; y < image.height * f; ++y) {
      for (std::int64_t x = 0; x < image.width * f; ++x) {
        for (std::int64_t c = 0; c < 3; ++c) {
          rgb[static_cast<std::size_t>(3 * (x + image.width * f * y) + c)] =
              image.rgb[static_cast<std::size_t>(3 * (x / f + image.width * (y / f)) + c)];
        }
      }
    }
    detail::writeRgbPng(dir / (image.name + ".png"), static_cast<std::uint32_t>(image.width * f),
                        static_cast<std::uint32_t>(image.height * f), rgb, image.pixel_width_mm,
                        image.pixel_height_mm);
  }
}

}  // namespace voxelsieve
