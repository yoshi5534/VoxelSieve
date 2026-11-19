#include "voxelsieve/beam_hardening.hpp"

#include <tbb/combinable.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {
namespace {

using Index3 = std::array<std::int64_t, 3>;

constexpr double kMadToSigma = 1.4826;
/// Voxels a depth bin needs for its median.
constexpr std::size_t kMinBinVoxels = 8;
/// Fit rounds; each one leaves out the voxels more than kOutlierSigmas from the fit before.
constexpr int kFitRounds = 4;
constexpr double kOutlierSigmas = 3.0;

double median(std::vector<float> values) {
  if (values.empty()) {
    return 0.0;
  }
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

/// Squared Euclidean distance transform along one line (Felzenszwalb and Huttenlocher): `f` holds
/// the squared distance in mm^2 to the nearest source along the axes done so far (0 at sources,
/// infinity without any), `out` gets it including this line. `step` is the pitch of the line in
/// mm; `hull` and `bounds` are scratch space.
void distanceLine(const std::vector<double>& f, double step, std::vector<double>& out,
                  std::vector<std::int64_t>& hull, std::vector<double>& bounds) {
  const auto n = static_cast<std::int64_t>(f.size());
  constexpr double kInf = std::numeric_limits<double>::infinity();
  hull.assign(static_cast<std::size_t>(n), 0);
  bounds.assign(static_cast<std::size_t>(n) + 1, 0.0);
  out.assign(static_cast<std::size_t>(n), kInf);
  const auto height = [&](std::int64_t i) {
    const double x = static_cast<double>(i) * step;
    return f[static_cast<std::size_t>(i)] + x * x;
  };
  // Where the parabolas of sources p < q intersect.
  const auto meet = [&](std::int64_t p, std::int64_t q) {
    return (height(q) - height(p)) / (2.0 * static_cast<double>(q - p) * step);
  };
  std::int64_t k = -1;
  for (std::int64_t q = 0; q < n; ++q) {
    if (f[static_cast<std::size_t>(q)] == kInf) {
      continue;
    }
    while (k >= 0 &&
           meet(hull[static_cast<std::size_t>(k)], q) <= bounds[static_cast<std::size_t>(k)]) {
      --k;
    }
    ++k;
    hull[static_cast<std::size_t>(k)] = q;
    bounds[static_cast<std::size_t>(k)] =
        k == 0 ? -kInf : meet(hull[static_cast<std::size_t>(k - 1)], q);
    bounds[static_cast<std::size_t>(k) + 1] = kInf;
  }
  if (k < 0) {
    return;  // no source on this line
  }
  std::size_t j = 0;
  for (std::int64_t q = 0; q < n; ++q) {
    const double xq = static_cast<double>(q) * step;
    while (bounds[j + 1] < xq) {
      ++j;
    }
    const std::int64_t v = hull[j];
    const double d = xq - static_cast<double>(v) * step;
    out[static_cast<std::size_t>(q)] = d * d + f[static_cast<std::size_t>(v)];
  }
}

/// One level of a dataset in memory, with the outside air and the depth below the surface.
struct Coarse {
  int level = 0;
  Index3 dims{};
  VoxelSize pitch;
  /// Grey values, NaN where the sieve removed the voxels.
  std::vector<float> values;
  /// Outside air: removed voxels and the voxels below the threshold connected to them (to the
  /// volume border when nothing was removed).
  std::vector<std::uint8_t> outside;
  /// Distance in mm from the centre of each voxel to the centre of the nearest outside voxel; 0
  /// for outside voxels.
  std::vector<float> depth_mm;

  [[nodiscard]] std::size_t index(std::int64_t x, std::int64_t y, std::int64_t z) const {
    return static_cast<std::size_t>(x + dims[0] * (y + dims[1] * z));
  }
  [[nodiscard]] std::size_t size() const { return values.size(); }
  /// Width of a depth bin: the largest pitch of the level.
  [[nodiscard]] double binMm() const { return std::max({pitch[0], pitch[1], pitch[2]}); }
};

Coarse readCoarse(const Dataset& dataset, int level) {
  const TelemetryPhase phase("read level " + std::to_string(level));
  Coarse coarse;
  coarse.level = level;
  coarse.dims = dataset.level(level).dims;
  coarse.pitch = dataset.level(level).voxel_size;
  const Index3& d = coarse.dims;
  const auto count = static_cast<std::size_t>(d[0] * d[1] * d[2]);
  coarse.values.resize(count);
  dataset.readRegion(level, {{0, 0, 0}, d}, coarse.values, std::numeric_limits<float>::quiet_NaN());

  // Outside air: flood fill from removed voxels and the border through voxels below threshold.
  const float threshold = dataset.info().threshold;
  const auto open = [&](std::size_t i) {
    return std::isnan(coarse.values[i]) || coarse.values[i] < threshold;
  };
  coarse.outside.assign(count, 0);
  std::deque<Index3> queue;
  const auto seed = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
    const std::size_t i = coarse.index(x, y, z);
    if (coarse.outside[i] == 0 && open(i)) {
      coarse.outside[i] = 1;
      queue.push_back({x, y, z});
    }
  };
  // The sieve already found the outside air robustly, block by block, and removed it; seeding
  // from single noisy voxels below the threshold at the volume border would let the outside
  // reach into a part that the border cuts through. Only a dataset without removed voxels is
  // seeded from its border.
  const bool removed = std::ranges::any_of(coarse.values, [](float v) { return std::isnan(v); });
  for (std::int64_t z = 0; z < d[2]; ++z) {
    for (std::int64_t y = 0; y < d[1]; ++y) {
      for (std::int64_t x = 0; x < d[0]; ++x) {
        const bool border =
            x == 0 || y == 0 || z == 0 || x == d[0] - 1 || y == d[1] - 1 || z == d[2] - 1;
        if (removed ? std::isnan(coarse.values[coarse.index(x, y, z)]) : border) {
          seed(x, y, z);
        }
      }
    }
  }
  while (!queue.empty()) {
    const Index3 p = queue.front();
    queue.pop_front();
    for (std::size_t a = 0; a < 3; ++a) {
      for (const std::int64_t step : {-1, 1}) {
        Index3 n = p;
        n[a] += step;
        if (n[a] >= 0 && n[a] < d[a]) {
          seed(n[0], n[1], n[2]);
        }
      }
    }
  }

  // Depth: exact Euclidean distance transform in mm, one axis after the other.
  constexpr double kInf = std::numeric_limits<double>::infinity();
  std::vector<double> squared(count);
  for (std::size_t i = 0; i < count; ++i) {
    squared[i] = coarse.outside[i] != 0 ? 0.0 : kInf;
  }
  const std::array<std::int64_t, 3> strides{1, d[0], d[0] * d[1]};
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const std::size_t a1 = axis == 0 ? 1 : 0;
    const std::size_t a2 = axis == 2 ? 1 : 2;
    tbb::parallel_for(std::int64_t{0}, d[a2], [&](std::int64_t j2) {
      std::vector<double> line(static_cast<std::size_t>(d[axis]));
      std::vector<double> out;
      std::vector<std::int64_t> hull;
      std::vector<double> bounds;
      for (std::int64_t j1 = 0; j1 < d[a1]; ++j1) {
        const std::int64_t base = j1 * strides[a1] + j2 * strides[a2];
        for (std::int64_t i = 0; i < d[axis]; ++i) {
          line[static_cast<std::size_t>(i)] =
              squared[static_cast<std::size_t>(base + i * strides[axis])];
        }
        distanceLine(line, coarse.pitch[axis], out, hull, bounds);
        for (std::int64_t i = 0; i < d[axis]; ++i) {
          squared[static_cast<std::size_t>(base + i * strides[axis])] =
              out[static_cast<std::size_t>(i)];
        }
      }
    });
  }
  coarse.depth_mm.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    coarse.depth_mm[i] = std::isfinite(squared[i]) ? static_cast<float>(std::sqrt(squared[i]))
                                                   : std::numeric_limits<float>::max();
  }
  return coarse;
}

/// Whether voxel `i` is material below the partial-volume shell of the surface.
bool isInnerMaterial(const Coarse& coarse, std::size_t i, float threshold) {
  return coarse.outside[i] == 0 && !std::isnan(coarse.values[i]) && coarse.values[i] >= threshold &&
         coarse.depth_mm[i] >= 1.5 * coarse.binMm();
}

CuppingProfile profileOf(const Coarse& coarse, std::span<const float> values,
                         const DatasetInfo& info) {
  CuppingProfile profile;
  profile.level = coarse.level;
  profile.air_level = info.air_level;
  const double bin = coarse.binMm();
  std::vector<std::vector<float>> bins;
  for (std::size_t i = 0; i < coarse.size(); ++i) {
    if (!isInnerMaterial(coarse, i, info.threshold) || std::isnan(values[i]) ||
        values[i] < info.threshold) {
      continue;
    }
    const auto b = static_cast<std::size_t>(std::lround(coarse.depth_mm[i] / bin));
    if (b >= bins.size()) {
      bins.resize(b + 1);
    }
    bins[b].push_back(values[i]);
  }
  for (std::size_t b = 0; b < bins.size(); ++b) {
    if (bins[b].size() >= kMinBinVoxels) {
      profile.depth_mm.push_back(static_cast<double>(b) * bin);
      profile.level_grey.push_back(median(std::move(bins[b])));
    }
  }
  if (profile.level_grey.size() >= 2) {
    // The core: the median of the levels in the deepest fifth of the depth range, as the
    // deepest bins hold few voxels.
    const double surface = profile.level_grey.front();
    const double deepest = profile.depth_mm.back();
    const double from = deepest - 0.2 * (deepest - profile.depth_mm.front());
    std::vector<float> core;
    for (std::size_t b = 1; b < profile.depth_mm.size(); ++b) {
      if (profile.depth_mm[b] >= from) {
        core.push_back(static_cast<float>(profile.level_grey[b]));
      }
    }
    if (surface > profile.air_level) {
      profile.cupping = (surface - median(core)) / (surface - profile.air_level);
    }
  }
  return profile;
}

/// Material level of a profile at `depth_mm`, interpolated linearly; the outermost level above
/// it and the deepest below it.
double levelAt(const CuppingProfile& profile, double depth_mm) {
  const auto& depth = profile.depth_mm;
  const auto& level = profile.level_grey;
  if (depth_mm <= depth.front()) {
    return level.front();
  }
  if (depth_mm >= depth.back()) {
    return level.back();
  }
  const auto upper = static_cast<std::size_t>(
      std::upper_bound(depth.begin(), depth.end(), depth_mm) - depth.begin());
  const double t = (depth_mm - depth[upper - 1]) / (depth[upper] - depth[upper - 1]);
  return level[upper - 1] + t * (level[upper] - level[upper - 1]);
}

/// Solves the symmetric positive definite system `a x = b` (n x n, row major) by Gaussian
/// elimination with partial pivoting.
std::vector<double> solve(std::vector<double> a, std::vector<double> b) {
  const std::size_t n = b.size();
  for (std::size_t col = 0; col < n; ++col) {
    std::size_t pivot = col;
    for (std::size_t r = col + 1; r < n; ++r) {
      if (std::abs(a[r * n + col]) > std::abs(a[pivot * n + col])) {
        pivot = r;
      }
    }
    if (std::abs(a[pivot * n + col]) < 1e-300) {
      throw std::runtime_error("Beam hardening fit is singular");
    }
    for (std::size_t c = 0; c < n; ++c) {
      std::swap(a[col * n + c], a[pivot * n + c]);
    }
    std::swap(b[col], b[pivot]);
    for (std::size_t r = col + 1; r < n; ++r) {
      const double f = a[r * n + col] / a[col * n + col];
      for (std::size_t c = col; c < n; ++c) {
        a[r * n + c] -= f * a[col * n + c];
      }
      b[r] -= f * b[col];
    }
  }
  std::vector<double> x(n);
  for (std::size_t r = n; r-- > 0;) {
    double sum = b[r];
    for (std::size_t c = r + 1; c < n; ++c) {
      sum -= a[r * n + c] * x[c];
    }
    x[r] = sum / a[r * n + r];
  }
  return x;
}

double standardDeviation(const std::vector<double>& values) {
  if (values.size() < 2) {
    return 0.0;
  }
  double mean = 0.0;
  for (const double v : values) {
    mean += v;
  }
  mean /= static_cast<double>(values.size());
  double sum = 0.0;
  for (const double v : values) {
    sum += (v - mean) * (v - mean);
  }
  return std::sqrt(sum / static_cast<double>(values.size() - 1));
}

struct Correction {
  /// Per coarse voxel: the grey value to add (reprojection) or the factor of the grey value
  /// above air (depth).
  std::vector<float> field;
  std::vector<double> coefficients;
  std::int64_t fit_voxels = 0;
  double spread_before = 0.0;
  double spread_after = 0.0;
};

/// Reconstructed powers of the path lengths through the slices across the rotation axis, and
/// the weights that flatten the material.
Correction reprojectionCorrection(const Coarse& coarse, const DatasetInfo& info,
                                  const BeamHardeningOptions& options) {
  const auto axis = static_cast<std::size_t>(options.rotation_axis);
  const std::size_t u = axis == 0 ? 1 : 0;
  const std::size_t v = axis == 2 ? 1 : 2;
  const Index3& d = coarse.dims;
  const std::array<std::int64_t, 3> strides{1, d[0], d[0] * d[1]};
  const ParallelBeam beam(d[u], d[v], coarse.pitch[u], coarse.pitch[v]);
  const auto powers = static_cast<std::size_t>(options.order - 1);  // p^2 ... p^order
  const double air = info.air_level;
  // Path lengths are divided by a fixed scale first (grey value range times the slice diagonal)
  // to keep the powers in float range, then by their true maximum below.
  const double scale = std::max(1.0, 2.0 * (info.threshold - air)) *
                       std::hypot(static_cast<double>(d[u]) * coarse.pitch[u],
                                  static_cast<double>(d[v]) * coarse.pitch[v]);
  std::vector<std::vector<float>> basis(powers, std::vector<float>(coarse.size(), 0.0F));
  std::atomic<double> largest{0.0};
  {
    const TelemetryPhase phase("reprojection");
    tbb::parallel_for(std::int64_t{0}, d[axis], [&](std::int64_t slice) {
      const auto pixels = static_cast<std::size_t>(d[u] * d[v]);
      std::vector<float> image(pixels);
      const auto at = [&](std::int64_t iu, std::int64_t iv) {
        return static_cast<std::size_t>(slice * strides[axis] + iu * strides[u] + iv * strides[v]);
      };
      // The image to project is the mean of the two neighbouring slices, not the slice itself.
      // The reconstructed powers of its own projection would carry its noise, ramp filtered, and
      // the fit would use that to cancel the noise of the material instead of the beam
      // hardening. The noise of neighbouring slices is nearly independent, while the part and
      // its beam hardening change little from one slice to the next.
      std::vector<std::int64_t> sources;
      for (const std::int64_t n : {slice - 1, slice + 1}) {
        if (n >= 0 && n < d[axis]) {
          sources.push_back(n);
        }
      }
      if (sources.empty()) {
        sources.push_back(slice);
      }
      std::fill(image.begin(), image.end(), 0.0F);
      for (const std::int64_t source : sources) {
        for (std::int64_t iv = 0; iv < d[v]; ++iv) {
          for (std::int64_t iu = 0; iu < d[u]; ++iu) {
            const auto i = static_cast<std::size_t>(source * strides[axis] + iu * strides[u] +
                                                    iv * strides[v]);
            const float value = coarse.values[i];
            if (coarse.outside[i] == 0 && !std::isnan(value)) {
              image[static_cast<std::size_t>(iu + d[u] * iv)] += static_cast<float>(
                  std::max(0.0, (value - air) / scale) / static_cast<double>(sources.size()));
            }
          }
        }
      }
      std::vector<float> sinogram = beam.project(image);
      double local = 0.0;
      for (const float p : sinogram) {
        local = std::max(local, static_cast<double>(p));
      }
      double seen = largest.load();
      while (local > seen && !largest.compare_exchange_weak(seen, local)) {
      }
      std::vector<float> power(sinogram.size());
      for (std::size_t k = 0; k < powers; ++k) {
        const auto exponent = static_cast<double>(k + 2);
        for (std::size_t i = 0; i < sinogram.size(); ++i) {
          power[i] = static_cast<float>(std::pow(static_cast<double>(sinogram[i]), exponent));
        }
        const std::vector<float> image_k = beam.reconstruct(power);
        for (std::int64_t iv = 0; iv < d[v]; ++iv) {
          for (std::int64_t iu = 0; iu < d[u]; ++iu) {
            basis[k][at(iu, iv)] = image_k[static_cast<std::size_t>(iu + d[u] * iv)];
          }
        }
      }
    });
  }
  Correction correction;
  correction.field.assign(coarse.size(), 0.0F);
  const double p_max = largest.load();
  if (p_max <= 0.0) {
    correction.coefficients.assign(powers, 0.0);
    return correction;
  }
  for (std::size_t k = 0; k < powers; ++k) {
    const auto norm = static_cast<float>(1.0 / std::pow(p_max, static_cast<double>(k + 2)));
    for (float& b : basis[k]) {
      b *= norm;
    }
  }

  const TelemetryPhase phase("fit");
  std::vector<std::size_t> samples;
  for (std::size_t i = 0; i < coarse.size(); ++i) {
    if (isInnerMaterial(coarse, i, info.threshold)) {
      samples.push_back(i);
    }
  }
  if (samples.size() < 16) {
    throw std::runtime_error("Too little material below the surface to correct beam hardening");
  }
  // Least squares for (m, c_2, ..., c_K): grey + sum c_k b_k = m over the inliers.
  const std::size_t n = powers + 1;
  std::vector<double> c(powers, 0.0);
  double m = 0.0;
  const auto residual = [&](std::size_t i) {
    double r = coarse.values[i] - m;
    for (std::size_t k = 0; k < powers; ++k) {
      r += c[k] * basis[k][i];
    }
    return r;
  };
  {
    std::vector<float> grey;
    grey.reserve(samples.size());
    for (const std::size_t i : samples) {
      grey.push_back(coarse.values[i]);
    }
    m = median(grey);
  }
  std::vector<std::size_t> inliers;
  for (int round = 0; round < kFitRounds; ++round) {
    std::vector<float> deviations;
    deviations.reserve(samples.size());
    for (const std::size_t i : samples) {
      deviations.push_back(static_cast<float>(std::abs(residual(i))));
    }
    const double sigma = std::max(kMadToSigma * median(deviations), 1e-6);
    inliers.clear();
    for (const std::size_t i : samples) {
      if (std::abs(residual(i)) <= kOutlierSigmas * sigma) {
        inliers.push_back(i);
      }
    }
    if (inliers.size() < n + 1) {
      break;
    }
    // Normal equations of grey = m - sum c_k b_k, unknowns (m, c_2, ...).
    std::vector<double> ata(n * n, 0.0);
    std::vector<double> atb(n, 0.0);
    std::vector<double> row(n);
    for (const std::size_t i : inliers) {
      row[0] = 1.0;
      for (std::size_t k = 0; k < powers; ++k) {
        row[k + 1] = -static_cast<double>(basis[k][i]);
      }
      for (std::size_t r = 0; r < n; ++r) {
        for (std::size_t s = 0; s < n; ++s) {
          ata[r * n + s] += row[r] * row[s];
        }
        atb[r] += row[r] * coarse.values[i];
      }
    }
    // Scale the columns to unit diagonal; the powers of p differ by orders of magnitude.
    std::vector<double> norm(n);
    for (std::size_t r = 0; r < n; ++r) {
      norm[r] = ata[r * n + r] > 0.0 ? 1.0 / std::sqrt(ata[r * n + r]) : 1.0;
    }
    for (std::size_t r = 0; r < n; ++r) {
      for (std::size_t s = 0; s < n; ++s) {
        ata[r * n + s] *= norm[r] * norm[s];
      }
      atb[r] *= norm[r];
      ata[r * n + r] += 1e-12;
    }
    const std::vector<double> x = solve(ata, atb);

    m = x[0] * norm[0];
    for (std::size_t k = 0; k < powers; ++k) {
      c[k] = x[k + 1] * norm[k + 1];
    }
  }
  std::vector<double> before;
  std::vector<double> after;
  before.reserve(inliers.size());
  after.reserve(inliers.size());
  for (const std::size_t i : inliers) {
    before.push_back(coarse.values[i]);
    after.push_back(residual(i) + m);
  }
  correction.coefficients = c;
  correction.fit_voxels = static_cast<std::int64_t>(inliers.size());
  correction.spread_before = standardDeviation(before);
  correction.spread_after = standardDeviation(after);
  for (std::size_t i = 0; i < coarse.size(); ++i) {
    double add = 0.0;
    for (std::size_t k = 0; k < powers; ++k) {
      add += c[k] * basis[k][i];
    }
    correction.field[i] = static_cast<float>(add);
  }
  return correction;
}

/// Factor of the grey value above air per coarse voxel: material level at the surface over the
/// material level at the voxel's depth.
Correction depthCorrection(const Coarse& coarse, const DatasetInfo& info,
                           const CuppingProfile& profile) {
  Correction correction;
  correction.field.assign(coarse.size(), 1.0F);
  if (profile.level_grey.size() < 2) {
    return correction;
  }
  const double air = info.air_level;
  const double surface = profile.level_grey.front() - air;
  std::vector<double> before;
  std::vector<double> after;
  for (std::size_t i = 0; i < coarse.size(); ++i) {
    if (coarse.outside[i] != 0) {
      continue;
    }
    const double level = std::max(levelAt(profile, coarse.depth_mm[i]) - air, 0.1 * surface);
    correction.field[i] = static_cast<float>(surface / level);
    if (isInnerMaterial(coarse, i, info.threshold)) {
      before.push_back(coarse.values[i]);
      after.push_back(air + (coarse.values[i] - air) * correction.field[i]);
    }
  }
  // The spread of the main material: other materials and pores left out as for the fit.
  std::vector<float> corrected;
  corrected.reserve(after.size());
  for (const double value : after) {
    corrected.push_back(static_cast<float>(value));
  }
  const double centre = median(corrected);
  for (float& value : corrected) {
    value = static_cast<float>(std::abs(value - centre));
  }
  const double sigma = kMadToSigma * median(corrected);
  std::vector<double> kept_before;
  std::vector<double> kept_after;
  for (std::size_t i = 0; i < after.size(); ++i) {
    if (std::abs(after[i] - centre) <= kOutlierSigmas * sigma) {
      kept_before.push_back(before[i]);
      kept_after.push_back(after[i]);
    }
  }
  correction.fit_voxels = static_cast<std::int64_t>(kept_after.size());
  correction.spread_before = standardDeviation(kept_before);
  correction.spread_after = standardDeviation(kept_after);
  return correction;
}

/// Linear interpolation weights from level-0 voxels to the voxel centres of a coarse level, per
/// axis: level-0 voxel x lies at coarse coordinate (x + 0.5) / 2^level - 0.5.
struct AxisWeights {
  std::vector<std::int64_t> lower;
  std::vector<float> t;  // weight of lower + 1
};

AxisWeights axisWeights(std::int64_t begin, std::int64_t end, int level, std::int64_t coarse) {
  AxisWeights w;
  const double factor = std::ldexp(1.0, level);
  for (std::int64_t x = begin; x < end; ++x) {
    const double c = std::clamp((static_cast<double>(x) + 0.5) / factor - 0.5, 0.0,
                                static_cast<double>(coarse - 1));
    const auto lower = std::min(static_cast<std::int64_t>(std::floor(c)), coarse - 1);
    w.lower.push_back(lower);
    w.t.push_back(lower + 1 < coarse ? static_cast<float>(c - static_cast<double>(lower)) : 0.0F);
  }
  return w;
}

}  // namespace

ParallelBeam::ParallelBeam(std::int64_t nx, std::int64_t ny, double pitch_x_mm, double pitch_y_mm,
                           int angles)
    : nx_(nx), ny_(ny), pitch_x_(pitch_x_mm), pitch_y_(pitch_y_mm) {
  if (nx < 1 || ny < 1 || !(pitch_x_mm > 0.0) || !(pitch_y_mm > 0.0) || angles < 0) {
    throw std::invalid_argument("Invalid parallel-beam geometry");
  }
  angles_ = angles > 0
                ? angles
                : std::max(16, static_cast<int>(std::ceil(std::numbers::pi / 2.0 *
                                                          static_cast<double>(std::max(nx, ny)))));
  bin_mm_ = std::min(pitch_x_mm, pitch_y_mm);
  const double half_diagonal =
      0.5 * std::hypot(static_cast<double>(nx) * pitch_x_mm, static_cast<double>(ny) * pitch_y_mm);
  const int half = static_cast<int>(std::ceil(half_diagonal / bin_mm_)) + 1;
  bins_ = 2 * half + 1;
  for (int a = 0; a < angles_; ++a) {
    const double theta = std::numbers::pi * a / angles_;
    cos_.push_back(std::cos(theta));
    sin_.push_back(std::sin(theta));
  }
  // Ram-Lak filter in the spatial domain (Kak and Slaney, eq. 3.61).
  ramp_.assign(static_cast<std::size_t>(bins_), 0.0);
  ramp_[0] = 1.0 / (4.0 * bin_mm_ * bin_mm_);
  for (int n = 1; n < bins_; n += 2) {
    ramp_[static_cast<std::size_t>(n)] = -1.0 / (static_cast<double>(n) * n * std::numbers::pi *
                                                 std::numbers::pi * bin_mm_ * bin_mm_);
  }
}

std::vector<float> ParallelBeam::project(std::span<const float> image) const {
  if (image.size() != static_cast<std::size_t>(nx_ * ny_)) {
    throw std::invalid_argument("Image size does not match the geometry");
  }
  std::vector<float> sinogram(static_cast<std::size_t>(angles_) * static_cast<std::size_t>(bins_));
  const double cx = 0.5 * static_cast<double>(nx_ - 1);
  const double cy = 0.5 * static_cast<double>(ny_ - 1);
  // Bilinear interpolation reaches one pixel beyond the image.
  const double x_reach = 0.5 * static_cast<double>(nx_ + 1) * pitch_x_;
  const double y_reach = 0.5 * static_cast<double>(ny_ + 1) * pitch_y_;
  const double dt = bin_mm_;
  const int half = bins_ / 2;
  const auto sample = [&](double fx, double fy) {
    const double x0 = std::floor(fx);
    const double y0 = std::floor(fy);
    const double wx = fx - x0;
    const double wy = fy - y0;
    const auto ix = static_cast<std::int64_t>(x0);
    const auto iy = static_cast<std::int64_t>(y0);
    double sum = 0.0;
    for (int j = 0; j < 2; ++j) {
      const std::int64_t y = iy + j;
      if (y < 0 || y >= ny_) {
        continue;
      }
      const double w_y = j == 0 ? 1.0 - wy : wy;
      for (int i = 0; i < 2; ++i) {
        const std::int64_t x = ix + i;
        if (x < 0 || x >= nx_) {
          continue;
        }
        sum += w_y * (i == 0 ? 1.0 - wx : wx) * image[static_cast<std::size_t>(x + nx_ * y)];
      }
    }
    return sum;
  };
  for (int a = 0; a < angles_; ++a) {
    const double c = cos_[static_cast<std::size_t>(a)];
    const double s = sin_[static_cast<std::size_t>(a)];
    for (int b = 0; b < bins_; ++b) {
      const double det = (b - half) * bin_mm_;
      // Ray: (det c - t s, det s + t c); clip t to where it can touch the image.
      const double px = det * c;
      const double py = det * s;
      double t_min = -std::numeric_limits<double>::infinity();
      double t_max = std::numeric_limits<double>::infinity();
      const auto clip = [&](double origin, double direction, double reach) {
        if (std::abs(direction) < 1e-12) {
          if (std::abs(origin) > reach) {
            t_min = 1.0;
            t_max = 0.0;
          }
          return;
        }
        double t0 = (-reach - origin) / direction;
        double t1 = (reach - origin) / direction;
        if (t0 > t1) {
          std::swap(t0, t1);
        }
        t_min = std::max(t_min, t0);
        t_max = std::min(t_max, t1);
      };
      clip(px, -s, x_reach);
      clip(py, c, y_reach);
      double sum = 0.0;
      if (t_min < t_max) {
        for (double t = std::ceil(t_min / dt) * dt; t <= t_max; t += dt) {
          sum += sample((px - t * s) / pitch_x_ + cx, (py + t * c) / pitch_y_ + cy);
        }
      }
      sinogram[static_cast<std::size_t>(a) * static_cast<std::size_t>(bins_) +
               static_cast<std::size_t>(b)] = static_cast<float>(sum * dt);
    }
  }
  return sinogram;
}

std::vector<float> ParallelBeam::reconstruct(std::span<const float> sinogram) const {
  const auto bins = static_cast<std::size_t>(bins_);
  if (sinogram.size() != static_cast<std::size_t>(angles_) * bins) {
    throw std::invalid_argument("Sinogram size does not match the geometry");
  }
  std::vector<double> filtered(sinogram.size());
  for (std::size_t a = 0; a < static_cast<std::size_t>(angles_); ++a) {
    const float* row = sinogram.data() + a * bins;
    double* out = filtered.data() + a * bins;
    for (std::size_t j = 0; j < bins; ++j) {
      double sum = ramp_[0] * row[j];
      // Only odd offsets have taps.
      for (std::size_t n = 1; n <= j; n += 2) {
        sum += ramp_[n] * row[j - n];
      }
      for (std::size_t n = 1; j + n < bins; n += 2) {
        sum += ramp_[n] * row[j + n];
      }
      out[j] = sum * bin_mm_;
    }
  }
  std::vector<float> image(static_cast<std::size_t>(nx_ * ny_));
  const double cx = 0.5 * static_cast<double>(nx_ - 1);
  const double cy = 0.5 * static_cast<double>(ny_ - 1);
  const double half = 0.5 * static_cast<double>(bins_ - 1);  // bins_ is odd
  const double weight = std::numbers::pi / angles_;
  for (std::int64_t y = 0; y < ny_; ++y) {
    const double my = (static_cast<double>(y) - cy) * pitch_y_;
    for (std::int64_t x = 0; x < nx_; ++x) {
      const double mx = (static_cast<double>(x) - cx) * pitch_x_;
      double sum = 0.0;
      for (std::size_t a = 0; a < static_cast<std::size_t>(angles_); ++a) {
        const double pos = (mx * cos_[a] + my * sin_[a]) / bin_mm_ + half;
        const double lower = std::floor(pos);
        const auto j = static_cast<std::int64_t>(lower);
        if (j < 0 || j + 1 >= bins_) {
          continue;
        }
        const double t = pos - lower;
        const double* row = filtered.data() + a * bins;
        sum += (1.0 - t) * row[j] + t * row[j + 1];
      }
      image[static_cast<std::size_t>(x + nx_ * y)] = static_cast<float>(sum * weight);
    }
  }
  return image;
}

std::string_view beamHardeningMethodName(BeamHardeningMethod method) {
  return method == BeamHardeningMethod::kDepth ? "depth" : "reprojection";
}

BeamHardeningMethod parseBeamHardeningMethod(std::string_view name) {
  if (name == "reprojection") {
    return BeamHardeningMethod::kReprojection;
  }
  if (name == "depth") {
    return BeamHardeningMethod::kDepth;
  }
  throw std::invalid_argument("Unknown beam hardening method: " + std::string(name));
}

int beamHardeningLevel(const Dataset& dataset, std::int64_t resolution) {
  const auto& levels = dataset.info().levels;
  for (const LevelInfo& level : levels) {
    if (std::max({level.dims[0], level.dims[1], level.dims[2]}) <= resolution) {
      return level.level;
    }
  }
  return levels.back().level;
}

CuppingProfile measureCupping(const Dataset& dataset, int level) {
  const Coarse coarse = readCoarse(dataset, level);
  return profileOf(coarse, coarse.values, dataset.info());
}

BeamHardeningInfo correctBeamHardening(const Dataset& dataset, const std::filesystem::path& dir,
                                       const BeamHardeningOptions& options) {
  if (options.rotation_axis < 0 || options.rotation_axis > 2) {
    throw std::invalid_argument("The rotation axis must be 0 (x), 1 (y) or 2 (z)");
  }
  if (options.order < 2 || options.order > 4) {
    throw std::invalid_argument("The order of the beam hardening correction must be 2 to 4");
  }
  if (options.resolution < 8) {
    throw std::invalid_argument("The resolution of the beam hardening correction must be >= 8");
  }
  const TelemetryPhase whole("beam hardening");
  const DatasetInfo& input = dataset.info();
  BeamHardeningInfo info;
  info.method = options.method;
  info.level = beamHardeningLevel(dataset, options.resolution);
  const Coarse coarse = readCoarse(dataset, info.level);
  info.before = profileOf(coarse, coarse.values, input);
  const Correction correction = options.method == BeamHardeningMethod::kDepth
                                    ? depthCorrection(coarse, input, info.before)
                                    : reprojectionCorrection(coarse, input, options);
  info.coefficients = correction.coefficients;
  info.fit_voxels = correction.fit_voxels;
  info.material_spread_before = correction.spread_before;
  info.material_spread_after = correction.spread_after;

  const bool additive = options.method == BeamHardeningMethod::kReprojection;
  const auto air = static_cast<float>(input.air_level);
  const BrickValues values = [&](const Index3&, const Box& box, std::vector<float>& out) {
    dataset.readRegion(0, box, out, air);
    std::array<AxisWeights, 3> w;
    for (std::size_t a = 0; a < 3; ++a) {
      w[a] = axisWeights(box.min[a], box.max[a], coarse.level, coarse.dims[a]);
    }
    const Index3 size{box.size(0), box.size(1), box.size(2)};
    tbb::parallel_for(std::int64_t{0}, size[2], [&](std::int64_t z) {
      const std::int64_t z0 = w[2].lower[static_cast<std::size_t>(z)];
      const float tz = w[2].t[static_cast<std::size_t>(z)];
      const std::int64_t z1 = std::min(z0 + 1, coarse.dims[2] - 1);
      for (std::int64_t y = 0; y < size[1]; ++y) {
        const std::int64_t y0 = w[1].lower[static_cast<std::size_t>(y)];
        const float ty = w[1].t[static_cast<std::size_t>(y)];
        const std::int64_t y1 = std::min(y0 + 1, coarse.dims[1] - 1);
        for (std::int64_t x = 0; x < size[0]; ++x) {
          const std::int64_t x0 = w[0].lower[static_cast<std::size_t>(x)];
          const float tx = w[0].t[static_cast<std::size_t>(x)];
          const std::int64_t x1 = std::min(x0 + 1, coarse.dims[0] - 1);
          const auto f = [&](std::int64_t cx, std::int64_t cy, std::int64_t cz) {
            return correction.field[coarse.index(cx, cy, cz)];
          };
          const float lower = (1.0F - ty) * ((1.0F - tx) * f(x0, y0, z0) + tx * f(x1, y0, z0)) +
                              ty * ((1.0F - tx) * f(x0, y1, z0) + tx * f(x1, y1, z0));
          const float upper = (1.0F - ty) * ((1.0F - tx) * f(x0, y0, z1) + tx * f(x1, y0, z1)) +
                              ty * ((1.0F - tx) * f(x0, y1, z1) + tx * f(x1, y1, z1));
          const float field = (1.0F - tz) * lower + tz * upper;
          float& value = out[static_cast<std::size_t>(x + size[0] * (y + size[1] * z))];
          value = additive ? value + field : air + (value - air) * field;
        }
      }
    });
  };
  {
    const TelemetryPhase phase("apply");
    info.dataset = writeDerivedDataset(dataset, dir, values, options.output);
  }
  const Dataset result = Dataset::open(dir);
  info.after = measureCupping(
      result, std::min(info.level, static_cast<int>(result.info().levels.size()) - 1));
  return info;
}

}  // namespace voxelsieve
