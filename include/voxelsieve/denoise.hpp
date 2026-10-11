#pragma once

// Denoising of datasets (docs/adr/0023-denoising.md): non-local means, which averages voxels
// whose surrounding patches look alike instead of voxels that lie close together, so edges, thin
// walls and small pores keep their contrast; or a learned model (.vsm, model.hpp) trained on the
// noisy scan itself. Both run tile by tile with a halo and write a new dataset with the bricks and
// active voxels of the input, so they work on datasets larger than RAM; the input stays as it is.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/model.hpp"
#include "voxelsieve/source.hpp"

namespace voxelsieve {

struct NonLocalMeansOptions {
  /// Half edge of the compared patches in voxels: 1 compares 3x3x3 patches.
  int patch_radius = 1;
  /// Half edge of the search window in voxels: 3 averages over up to 7x7x7 voxels.
  int search_radius = 3;
  /// Filter strength beta: a voxel of the window gets the weight exp(-r / beta), with r the mean
  /// squared difference of the two patches as a multiple of what noise alone gives at their
  /// offset (NoiseModel). Patches of the same structure have r = 1 on average, so 1 averages them
  /// with weight exp(-1); larger values smooth more.
  double strength = 1.0;
  /// Patches with r above this limit count as other structure and get no weight. Without it, the
  /// many voxels of the material around a pore of a few voxels outweigh the few that look like
  /// it, however small their weights, and fill the pore in. r of two patches of the same
  /// structure scatters around 1 by the noise of the patch distance, so values near 1 also drop
  /// some of those and smooth less.
  double similarity_limit = 1.3;
};

/// The noise of a scan as non-local means needs it: how much two voxels at a given offset differ
/// in homogeneous material, the mean of (v(p) - v(p + d))^2. Uncorrelated noise gives 2 sigma^2 at
/// every offset. The reconstruction filter of a real scan correlates neighbouring voxels, which
/// then differ less; non-local means that took 2 sigma^2 for them would find neighbours similar
/// whatever the structure and blur small pores like a box filter.
struct NoiseModel {
  /// Standard deviation of the noise in grey values.
  double sigma = 0.0;
  /// Largest offset per axis in voxels.
  int radius = 0;
  /// Mean squared difference per offset (dx, dy, dz) in [-radius, radius]^3, dx fastest.
  std::vector<double> pair_variance;

  /// Uncorrelated noise.
  [[nodiscard]] static NoiseModel white(double sigma, int radius);
  [[nodiscard]] double pairVariance(int dx, int dy, int dz) const;
  /// Correlation of two voxels at an offset: 1 - pair variance / (2 sigma^2).
  [[nodiscard]] double correlation(int dx, int dy, int dz) const;
};

/// Measures the noise of level 0 of a dataset from its 8x8x8 blocks that are fully active and
/// whose mean is above the threshold: sigma is the square root of the median block variance and
/// each pair variance the median over the blocks of the mean squared difference of their voxels
/// at that offset (from a sample of a few thousand blocks). Blocks across an edge vary more, so
/// the medians are those of homogeneous material. Zero when no block qualifies.
[[nodiscard]] NoiseModel estimateNoise(const Dataset& dataset, int radius = 3);

/// Non-local means of a dense volume (x fastest). Every voxel is compared with the voxels of its
/// search window inside the volume, so voxels within `search_radius + patch_radius` of the border
/// see fewer neighbours: give the volume that much context around the voxels whose result is
/// used. The noise model must reach the search radius; patch distances at offset d are measured
/// against its pair variance at d.
[[nodiscard]] std::vector<float> nonLocalMeans(std::span<const float> grey,
                                               const std::array<std::int64_t, 3>& dims,
                                               const NoiseModel& noise,
                                               const NonLocalMeansOptions& options = {});

enum class DenoiseMethod : std::uint8_t { kNonLocalMeans, kModel };

/// "non_local_means", "model".
[[nodiscard]] std::string_view denoiseMethodName(DenoiseMethod method);

struct DenoiseOptions {
  DenoiseMethod method = DenoiseMethod::kNonLocalMeans;
  NonLocalMeansOptions non_local_means;
  /// Noise sigma for non-local means in grey values, taken as uncorrelated; unset: measured
  /// with estimateNoise, correlation included.
  std::optional<double> noise_sigma;
  /// The model for `kModel`, a denoising model (ModelTask::kDenoising).
  const Model* model = nullptr;
  /// Edge of the tiles in voxels; 0: 64 for non-local means (tiles run in parallel), 128 for a
  /// model (its layers run in parallel).
  std::int64_t tile = 0;
  /// Stored value type and progress, as for writeDerivedDataset. Integer types round the
  /// denoised values to the nearest integer.
  DerivedDatasetOptions output;
};

struct DenoiseInfo {
  DatasetInfo dataset;
  DenoiseMethod method = DenoiseMethod::kNonLocalMeans;
  /// The noise of the input: given or measured. For non-local means, the model it used.
  NoiseModel noise;
  /// Name of the model, empty for non-local means.
  std::string model;
};

/// Denoises level 0 of `dataset` and writes the result as a new dataset to `dir`, which must not
/// exist or be empty, with the same bricks and active voxels; the coarser levels are rebuilt from
/// the denoised values. Removed air around the part counts as the dataset's air level.
DenoiseInfo denoiseDataset(const Dataset& dataset, const std::filesystem::path& dir,
                           const DenoiseOptions& options = {});

/// How close a denoised dataset comes to the noise-free scan, for benchmarks on synthetic scans
/// (ADR 0023). Root mean square differences in grey values over the voxels active in the
/// dataset: all of them, and split into edges (voxels where the reference differs from a face
/// neighbour by more than 5 % of the contrast, 2 * (threshold - air level)), homogeneous
/// material (above the threshold) and homogeneous air inside the kept region.
struct DenoiseScore {
  double rmse = 0.0;
  double rmse_edges = 0.0;
  double rmse_material = 0.0;
  double rmse_air = 0.0;
  std::int64_t voxels = 0;
  std::int64_t edge_voxels = 0;
  std::int64_t material_voxels = 0;
  std::int64_t air_voxels = 0;
};

/// Compares level 0 of `dataset` with `reference`, a volume of the same dimensions, such as the
/// scan without noise.
[[nodiscard]] DenoiseScore scoreDenoising(const Dataset& dataset, const VolumeSource& reference);

}  // namespace voxelsieve
