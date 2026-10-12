#include "voxelsieve/denoise.hpp"

#include <tbb/combinable.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

#include "detail/noise.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {
namespace {

using Index3 = std::array<std::int64_t, 3>;

/// Standard deviations of blocks are collected in bins of this width, in grey values.
constexpr double kSigmaBin = 0.5;
constexpr std::size_t kSigmaBins = 1U << 17U;  // up to 65536 grey values

/// Blocks sampled for the pair variances of the noise model.
constexpr std::size_t kNoiseSampleBlocks = 4000;
constexpr std::uint64_t kNoiseSampleSeed = 0x6e6f697365ULL;
/// Inner blocks needed to measure the noise from them alone.
constexpr std::uint64_t kMinInnerBlocks = 32;
/// Offsets the measured noise model covers at least.
constexpr int kNoiseModelRadius = 3;

/// Sums of `values` (dims, x fastest) over a box of radius `radius` around every voxel, with
/// zeros outside the volume. Separable running sums, in place. In double, sums of squared
/// differences of integer grey values are exact, so every voxel gets the same sum wherever its
/// tile starts.
void boxSum(std::vector<double>& values, const Index3& dims, int radius) {
  if (radius == 0) {
    return;
  }
  const Index3 stride{1, dims[0], dims[0] * dims[1]};
  std::vector<double> line;
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const std::size_t u = axis == 0 ? 1 : 0;
    const std::size_t v = 3 - axis - u;
    const std::int64_t n = dims[axis];
    line.resize(static_cast<std::size_t>(n));
    for (std::int64_t b = 0; b < dims[v]; ++b) {
      for (std::int64_t a = 0; a < dims[u]; ++a) {
        double* start = values.data() + a * stride[u] + b * stride[v];
        for (std::int64_t i = 0; i < n; ++i) {
          line[static_cast<std::size_t>(i)] = start[i * stride[axis]];
        }
        // Running sum over [i - radius, i + radius].
        double sum = 0.0;
        for (std::int64_t i = 0; i < std::min<std::int64_t>(radius, n); ++i) {
          sum += line[static_cast<std::size_t>(i)];
        }
        for (std::int64_t i = 0; i < n; ++i) {
          if (i + radius < n) {
            sum += line[static_cast<std::size_t>(i + radius)];
          }
          if (i - radius - 1 >= 0) {
            sum -= line[static_cast<std::size_t>(i - radius - 1)];
          }
          start[i * stride[axis]] = sum;
        }
      }
    }
  }
}

/// e^x for x <= 0, within 2e-6 relative. Unlike std::exp without -ffast-math, the compiler
/// vectorises it: 2^(x log2 e) split into an integer power, set in the exponent bits, and a
/// polynomial for the fraction.
inline float fastExp(float x) {
  const float t = std::max(x, -87.0F) * std::numbers::log2e_v<float>;
  const auto whole = static_cast<std::int32_t>(t);  // towards zero: t - whole in (-1, 0]
  const float f = (t - static_cast<float>(whole)) * std::numbers::ln2_v<float>;
  // e^f for f in (-0.7, 0], Taylor to the 7th power.
  const float poly =
      1.0F + f * (1.0F + f * (0.5F + f * (1.0F / 6.0F +
                                          f * (1.0F / 24.0F +
                                               f * (1.0F / 120.0F +
                                                    f * (1.0F / 720.0F + f * (1.0F / 5040.0F)))))));
  return poly * std::bit_cast<float>((whole + 127) << 23);
}

void checkOptions(const NonLocalMeansOptions& options, double noise_sigma) {
  if (options.patch_radius < 0 || options.search_radius < 1 || options.strength <= 0.0 ||
      options.similarity_limit <= 0.0) {
    throw std::invalid_argument(
        "Non-local means needs patch radius >= 0, search radius >= 1, strength > 0 and "
        "similarity limit > 0");
  }
  if (noise_sigma < 0.0) {
    throw std::invalid_argument("The noise sigma must not be negative");
  }
}

/// Index of offset (dx, dy, dz) in a NoiseModel's pair variances, dx fastest.
std::size_t offsetIndex(int dx, int dy, int dz, int radius) {
  const auto edge = 2 * static_cast<std::size_t>(radius) + 1;
  return static_cast<std::size_t>(dx + radius) +
         edge *
             (static_cast<std::size_t>(dy + radius) + edge * static_cast<std::size_t>(dz + radius));
}

/// Offset of voxel (x, y, z) in an 8x8x8 leaf.
std::size_t leafIndex(int x, int y, int z) {
  return (static_cast<std::size_t>(x) << 6U) + (static_cast<std::size_t>(y) << 3U) +
         static_cast<std::size_t>(z);
}

}  // namespace

std::vector<float> nonLocalMeans(std::span<const float> grey,
                                 const std::array<std::int64_t, 3>& dims, const NoiseModel& noise,
                                 const NonLocalMeansOptions& options) {
  checkOptions(options, noise.sigma);
  if (static_cast<std::int64_t>(grey.size()) != dims[0] * dims[1] * dims[2]) {
    throw std::invalid_argument("Volume size does not match its dimensions");
  }
  if (noise.radius < options.search_radius ||
      noise.pair_variance.size() != static_cast<std::size_t>(std::pow(2 * noise.radius + 1, 3))) {
    throw std::invalid_argument("The noise model does not reach the search radius");
  }
  if (noise.sigma == 0.0 || grey.empty()) {
    return {grey.begin(), grey.end()};
  }
  const std::size_t n = grey.size();
  std::vector<float> weight_sum(n, 0.0F);
  std::vector<float> value_sum(n, 0.0F);
  std::vector<float> weight_max(n, 0.0F);
  std::vector<double> distance;
  std::vector<float> weights;
  const int patch = 2 * options.patch_radius + 1;
  const int s = options.search_radius;
  // Each pair of voxels is visited once, for the offset with the larger index; its weight goes to
  // both voxels.
  for (int dz = 0; dz <= s; ++dz) {
    for (int dy = -s; dy <= s; ++dy) {
      for (int dx = -s; dx <= s; ++dx) {
        if (dz == 0 && (dy < 0 || (dy == 0 && dx <= 0))) {
          continue;
        }
        const Index3 d{dx, dy, dz};
        // Voxels p whose partner p + d lies inside the volume.
        Index3 lo{};
        Index3 size{};
        for (std::size_t a = 0; a < 3; ++a) {
          lo[a] = std::max<std::int64_t>(0, -d[a]);
          size[a] = std::min(dims[a], dims[a] - d[a]) - lo[a];
        }
        if (size[0] <= 0 || size[1] <= 0 || size[2] <= 0) {
          continue;
        }
        // exp(-mean squared patch difference / (beta * the difference noise alone gives)).
        const double expected = noise.pairVariance(dx, dy, dz);
        if (expected <= 0.0) {
          continue;
        }
        // The patch sum as a multiple of what noise alone gives.
        const double factor = 1.0 / (expected * patch * patch * patch);
        const auto limit = static_cast<float>(options.similarity_limit);
        const auto inverse_strength = static_cast<float>(1.0 / options.strength);
        const std::int64_t shift = dx + dims[0] * (dy + dims[1] * dz);
        distance.resize(static_cast<std::size_t>(size[0] * size[1] * size[2]));
        for (std::int64_t z = 0; z < size[2]; ++z) {
          for (std::int64_t y = 0; y < size[1]; ++y) {
            const float* a = grey.data() + lo[0] + dims[0] * ((y + lo[1]) + dims[1] * (z + lo[2]));
            const float* b = a + shift;
            double* out = distance.data() + size[0] * (y + size[1] * z);
            for (std::int64_t x = 0; x < size[0]; ++x) {
              const double diff = static_cast<double>(a[x]) - static_cast<double>(b[x]);
              out[x] = diff * diff;
            }
          }
        }
        boxSum(distance, size, options.patch_radius);
        weights.resize(static_cast<std::size_t>(size[0]));
        for (std::int64_t z = 0; z < size[2]; ++z) {
          for (std::int64_t y = 0; y < size[1]; ++y) {
            const std::int64_t p = lo[0] + dims[0] * ((y + lo[1]) + dims[1] * (z + lo[2]));
            const double* dist = distance.data() + size[0] * (y + size[1] * z);
            float* __restrict w = weights.data();
            for (std::int64_t x = 0; x < size[0]; ++x) {
              const auto ratio = static_cast<float>(dist[x] * factor);
              const float weight = fastExp(-ratio * inverse_strength);
              w[x] = ratio <= limit ? weight : 0.0F;
            }
            // The weight goes to both voxels of each pair: first to p, then to its partner.
            for (const auto& [self, other] : {std::pair{p, p + shift}, std::pair{p + shift, p}}) {
              float* __restrict weight_row = weight_sum.data() + self;
              float* __restrict value_row = value_sum.data() + self;
              float* __restrict max_row = weight_max.data() + self;
              const float* __restrict partner = grey.data() + other;
              for (std::int64_t x = 0; x < size[0]; ++x) {
                weight_row[x] += w[x];
                value_row[x] += w[x] * partner[x];
                max_row[x] = std::max(max_row[x], w[x]);
              }
            }
          }
        }
      }
    }
  }
  // The voxel itself counts with the largest weight of its neighbours, so it does not dominate
  // where similar patches exist and keeps its value where none does.
  std::vector<float> result(n);
  for (std::size_t i = 0; i < n; ++i) {
    const float self = weight_max[i] > 0.0F ? weight_max[i] : 1.0F;
    result[i] = (value_sum[i] + self * grey[i]) / (weight_sum[i] + self);
  }
  return result;
}

NoiseModel NoiseModel::white(double sigma, int radius) {
  if (sigma < 0.0 || radius < 0) {
    throw std::invalid_argument("The noise sigma and radius must not be negative");
  }
  const auto edge = 2 * static_cast<std::size_t>(radius) + 1;
  NoiseModel noise{sigma, radius, std::vector<double>(edge * edge * edge, 2.0 * sigma * sigma)};
  noise.pair_variance[noise.pair_variance.size() / 2] = 0.0;  // a voxel and itself
  return noise;
}

double NoiseModel::pairVariance(int dx, int dy, int dz) const {
  if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) > radius) {
    throw std::out_of_range("Offset outside the noise model");
  }
  return pair_variance[offsetIndex(dx, dy, dz, radius)];
}

double NoiseModel::correlation(int dx, int dy, int dz) const {
  return sigma > 0.0 ? 1.0 - pairVariance(dx, dy, dz) / (2.0 * sigma * sigma) : 0.0;
}

NoiseModel estimateNoise(const Dataset& dataset, int radius) {
  if (radius < 0 || radius >= 8) {
    throw std::invalid_argument("The noise model reaches 0 to 7 voxels");
  }
  const float threshold = dataset.info().threshold;
  // A material block: fully active and its mean above the threshold. Gives its variance.
  const auto material = [&](const auto* leaf, double* variance) {
    if (leaf == nullptr || !leaf->getValueMask().isOn()) {
      return false;
    }
    double sum = 0.0;
    double sum_sq = 0.0;
    for (auto it = leaf->cbeginValueOn(); it; ++it) {
      const auto value = static_cast<double>(*it);
      sum += value;
      sum_sq += value * value;
    }
    const auto count = static_cast<double>(leaf->onVoxelCount());
    const double mean = sum / count;
    if (variance != nullptr) {
      *variance = std::max(0.0, sum_sq / count - mean * mean);
    }
    return mean >= threshold;
  };
  // An inner block: a material block whose six neighbours are material blocks too, so no surface
  // crosses it. In small or thin parts most material blocks lie at the surface, whose edge would
  // raise the median.
  const std::int64_t brick_size = dataset.info().brick_size;
  const auto inner = [&](const auto& tree, const auto& leaf) {
    const openvdb::Coord origin = leaf.origin();
    const Index3 brick{origin.x() / brick_size, origin.y() / brick_size, origin.z() / brick_size};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      for (const int step : {-8, 8}) {
        openvdb::Coord next = origin;
        next[axis] += step;
        if (next[axis] < 0) {
          return false;
        }
        const Index3 next_brick{next.x() / brick_size, next.y() / brick_size,
                                next.z() / brick_size};
        if (next_brick == brick) {
          if (!material(tree.probeConstLeaf(next), nullptr)) {
            return false;
          }
          continue;
        }
        const Dataset::BrickPtr other = dataset.brick(0, next_brick);
        if (!other || !other->visit([&](const auto& grid) {
              return material(grid.tree().probeConstLeaf(next), nullptr);
            })) {
          return false;
        }
      }
    }
    return true;
  };

  // Sigma from all blocks, binned: of the inner blocks if there are enough, else of all material
  // blocks.
  using Histogram = std::vector<std::uint64_t>;
  tbb::combinable<std::array<Histogram, 2>> histograms(
      [] { return std::array<Histogram, 2>{Histogram(kSigmaBins, 0), Histogram(kSigmaBins, 0)}; });
  dataset.forEachGrid(0, [&](const auto& /*brick*/, const auto& grid) {
    std::array<Histogram, 2>& local = histograms.local();
    for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
      double variance = 0.0;
      if (material(&*leaf, &variance)) {
        const auto bin =
            std::min(static_cast<std::size_t>(std::sqrt(variance) / kSigmaBin), kSigmaBins - 1);
        ++local[0][bin];
        if (inner(grid.tree(), *leaf)) {
          ++local[1][bin];
        }
      }
    }
  });
  std::array<Histogram, 2> histogram{Histogram(kSigmaBins, 0), Histogram(kSigmaBins, 0)};
  histograms.combine_each([&](const std::array<Histogram, 2>& local) {
    for (std::size_t h = 0; h < 2; ++h) {
      for (std::size_t i = 0; i < kSigmaBins; ++i) {
        histogram[h][i] += local[h][i];
      }
    }
  });
  std::array<std::uint64_t, 2> totals{};
  for (std::size_t h = 0; h < 2; ++h) {
    for (const std::uint64_t count : histogram[h]) {
      totals[h] += count;
    }
  }
  const bool use_inner = totals[1] >= kMinInnerBlocks;
  const std::uint64_t total = totals[use_inner ? 1 : 0];
  NoiseModel noise = NoiseModel::white(0.0, radius);
  if (total == 0) {
    return noise;
  }
  // The median of the variances is the square of the median of the standard deviations.
  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < kSigmaBins; ++i) {
    seen += histogram[use_inner ? 1 : 0][i];
    if (2 * seen >= total) {
      noise.sigma = (static_cast<double>(i) + 0.5) * kSigmaBin;
      break;
    }
  }

  // Pair variances from a sample of the blocks, picked by a hash of their position.
  const double keep =
      std::min(1.0, static_cast<double>(kNoiseSampleBlocks) / static_cast<double>(total));
  const std::size_t offsets = noise.pair_variance.size();
  tbb::combinable<std::vector<std::vector<double>>> samples(
      [&] { return std::vector<std::vector<double>>(offsets); });
  dataset.forEachGrid(0, [&](const auto& /*brick*/, const auto& grid) {
    std::vector<std::vector<double>>& local = samples.local();
    std::array<double, 512> values{};
    for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
      const openvdb::Coord origin = leaf->origin();
      const std::uint64_t block = (static_cast<std::uint64_t>(origin.x() >> 3) & 0x1FFFFFU) |
                                  (static_cast<std::uint64_t>(origin.y() >> 3) & 0x1FFFFFU) << 21U |
                                  (static_cast<std::uint64_t>(origin.z() >> 3) & 0x1FFFFFU) << 42U;
      if (detail::uniformNoise(kNoiseSampleSeed, block) >= keep) {
        continue;
      }
      if (!material(&*leaf, nullptr) || (use_inner && !inner(grid.tree(), *leaf))) {
        continue;
      }
      for (openvdb::Index i = 0; i < 512; ++i) {
        values[i] = static_cast<double>(leaf->getValue(i));
      }
      // Voxel (x, y, z) of the leaf has offset (x << 6) + (y << 3) + z.
      for (int dz = -radius; dz <= radius; ++dz) {
        for (int dy = -radius; dy <= radius; ++dy) {
          for (int dx = -radius; dx <= radius; ++dx) {
            double sum = 0.0;
            int count = 0;
            for (int x = std::max(0, -dx); x < std::min(8, 8 - dx); ++x) {
              for (int y = std::max(0, -dy); y < std::min(8, 8 - dy); ++y) {
                for (int z = std::max(0, -dz); z < std::min(8, 8 - dz); ++z) {
                  const double diff =
                      values[leafIndex(x, y, z)] - values[leafIndex(x + dx, y + dy, z + dz)];
                  sum += diff * diff;
                  ++count;
                }
              }
            }
            local[offsetIndex(dx, dy, dz, radius)].push_back(sum / count);
          }
        }
      }
    }
  });
  std::vector<std::vector<double>> all(offsets);
  samples.combine_each([&](const std::vector<std::vector<double>>& local) {
    for (std::size_t o = 0; o < offsets; ++o) {
      all[o].insert(all[o].end(), local[o].begin(), local[o].end());
    }
  });
  for (std::size_t o = 0; o < offsets; ++o) {
    std::vector<double>& values = all[o];
    if (values.empty()) {
      noise.pair_variance[o] = o == offsets / 2 ? 0.0 : 2.0 * noise.sigma * noise.sigma;
      continue;
    }
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    noise.pair_variance[o] = *middle;
  }
  return noise;
}

std::string_view denoiseMethodName(DenoiseMethod method) {
  return method == DenoiseMethod::kModel ? "model" : "non_local_means";
}

DenoiseInfo denoiseDataset(const Dataset& dataset, const std::filesystem::path& dir,
                           const DenoiseOptions& options) {
  const bool use_model = options.method == DenoiseMethod::kModel;
  if (use_model &&
      (options.model == nullptr || options.model->spec().task != ModelTask::kDenoising)) {
    throw std::invalid_argument("Denoising with a model needs a denoising model");
  }
  if (options.tile < 0) {
    throw std::invalid_argument("The tile edge must not be negative");
  }
  const NonLocalMeansOptions& nlm = options.non_local_means;
  checkOptions(nlm, options.noise_sigma.value_or(0.0));
  const int radius = std::max(nlm.search_radius, kNoiseModelRadius);
  DenoiseInfo info;
  info.method = options.method;
  if (use_model) {
    info.model = options.model->spec().name;
  }
  if (options.noise_sigma) {
    info.noise = NoiseModel::white(*options.noise_sigma, radius);
  } else {
    const TelemetryPhase phase("noise estimate");
    info.noise = estimateNoise(dataset, std::min(radius, 7));
    if (info.noise.radius < nlm.search_radius) {
      throw std::invalid_argument("Measured noise reaches 7 voxels; give the noise sigma");
    }
  }

  const std::int64_t tile = options.tile > 0 ? options.tile : (use_model ? 128 : 64);
  const std::int64_t halo =
      use_model ? options.model->spec().halo : nlm.search_radius + nlm.patch_radius;
  const Index3 dims = dataset.info().dims;
  const float air = dataset.info().air_level;

  const BrickValues values = [&](const Index3& /*brick*/, const Box& box, std::vector<float>& out) {
    std::vector<Box> cores;
    for (std::int64_t z = box.min[2]; z < box.max[2]; z += tile) {
      for (std::int64_t y = box.min[1]; y < box.max[1]; y += tile) {
        for (std::int64_t x = box.min[0]; x < box.max[0]; x += tile) {
          cores.push_back({{x, y, z},
                           {std::min(x + tile, box.max[0]), std::min(y + tile, box.max[1]),
                            std::min(z + tile, box.max[2])}});
        }
      }
    }
    const auto denoise_tile = [&](const Box& core) {
      Box outer;
      for (std::size_t a = 0; a < 3; ++a) {
        outer.min[a] = std::max<std::int64_t>(core.min[a] - halo, 0);
        outer.max[a] = std::min(core.max[a] + halo, dims[a]);
      }
      const Index3 size{outer.size(0), outer.size(1), outer.size(2)};
      std::vector<float> grey(static_cast<std::size_t>(outer.voxelCount()));
      // Removed air reads as NaN first, so tiles without active voxels are skipped.
      dataset.readRegion(0, outer, grey, std::numeric_limits<float>::quiet_NaN());
      const auto at = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
        return static_cast<std::size_t>(
            (x - outer.min[0]) + size[0] * ((y - outer.min[1]) + size[1] * (z - outer.min[2])));
      };
      bool active = false;
      for (std::int64_t z = core.min[2]; z < core.max[2] && !active; ++z) {
        for (std::int64_t y = core.min[1]; y < core.max[1] && !active; ++y) {
          for (std::int64_t x = core.min[0]; x < core.max[0] && !active; ++x) {
            active = !std::isnan(grey[at(x, y, z)]);
          }
        }
      }
      if (!active) {
        return;
      }
      for (float& value : grey) {
        if (std::isnan(value)) {
          value = air;
        }
      }
      const std::vector<float> denoised = use_model ? options.model->denoise(grey, size, air)
                                                    : nonLocalMeans(grey, size, info.noise, nlm);
      for (std::int64_t z = core.min[2]; z < core.max[2]; ++z) {
        for (std::int64_t y = core.min[1]; y < core.max[1]; ++y) {
          for (std::int64_t x = core.min[0]; x < core.max[0]; ++x) {
            out[static_cast<std::size_t>((x - box.min[0]) +
                                         box.size(0) *
                                             ((y - box.min[1]) + box.size(1) * (z - box.min[2])))] =
                denoised[at(x, y, z)];
          }
        }
      }
    };
    if (use_model) {
      // The layers run in parallel inside a tile.
      std::for_each(cores.begin(), cores.end(), denoise_tile);
    } else {
      tbb::parallel_for(std::size_t{0}, cores.size(),
                        [&](std::size_t i) { denoise_tile(cores[i]); });
    }
  };
  const TelemetryPhase phase(use_model ? "denoise (model)" : "denoise (non-local means)");
  info.dataset = writeDerivedDataset(dataset, dir, values, options.output);
  return info;
}

DenoiseScore scoreDenoising(const Dataset& dataset, const VolumeSource& reference) {
  const DatasetInfo& info = dataset.info();
  if (reference.dims() != info.dims) {
    throw std::invalid_argument("The reference has other dimensions than the dataset");
  }
  const double edge_step = 0.05 * 2.0 * (info.threshold - info.air_level);
  struct Sums {
    std::array<double, 4> squares{};  // all, edges, material, air
    std::array<std::int64_t, 4> counts{};
  };
  tbb::combinable<Sums> partial;
  dataset.forEachGrid(0, [&](const Index3& brick, const auto& grid) {
    const Box box = dataset.brickBox(0, brick);
    Box outer;
    for (std::size_t a = 0; a < 3; ++a) {
      outer.min[a] = std::max<std::int64_t>(box.min[a] - 1, 0);
      outer.max[a] = std::min(box.max[a] + 1, info.dims[a]);
    }
    std::vector<std::uint16_t> ref(static_cast<std::size_t>(outer.voxelCount()));
    reference.readRegion(outer, ref);
    const Index3 size{outer.size(0), outer.size(1), outer.size(2)};
    const Index3 stride{1, size[0], size[0] * size[1]};
    Sums& sums = partial.local();
    for (auto it = grid.cbeginValueOn(); it; ++it) {
      const openvdb::CoordBBox voxels = it.getBoundingBox();  // a voxel or an active tile
      for (auto c = voxels.begin(); c; ++c) {
        const Index3 p{(*c).x() - outer.min[0], (*c).y() - outer.min[1], (*c).z() - outer.min[2]};
        const std::int64_t i = p[0] + stride[1] * p[1] + stride[2] * p[2];
        const double truth = ref[static_cast<std::size_t>(i)];
        bool edge = false;
        for (std::size_t a = 0; a < 3 && !edge; ++a) {
          for (const std::int64_t step : {-1, 1}) {
            if (p[a] + step >= 0 && p[a] + step < size[a] &&
                std::abs(static_cast<double>(ref[static_cast<std::size_t>(i + step * stride[a])]) -
                         truth) > edge_step) {
              edge = true;
            }
          }
        }
        const double diff = static_cast<double>(*it) - truth;
        const std::size_t kind = edge ? 1 : (truth >= info.threshold ? 2 : 3);
        for (const std::size_t k : {std::size_t{0}, kind}) {
          sums.squares[k] += diff * diff;
          ++sums.counts[k];
        }
      }
    }
  });
  Sums total;
  partial.combine_each([&](const Sums& local) {
    for (std::size_t k = 0; k < 4; ++k) {
      total.squares[k] += local.squares[k];
      total.counts[k] += local.counts[k];
    }
  });
  const auto rms = [&](std::size_t k) {
    return total.counts[k] > 0 ? std::sqrt(total.squares[k] / static_cast<double>(total.counts[k]))
                               : 0.0;
  };
  DenoiseScore score;
  score.rmse = rms(0);
  score.rmse_edges = rms(1);
  score.rmse_material = rms(2);
  score.rmse_air = rms(3);
  score.voxels = total.counts[0];
  score.edge_voxels = total.counts[1];
  score.material_voxels = total.counts[2];
  score.air_voxels = total.counts[3];
  return score;
}

}  // namespace voxelsieve
