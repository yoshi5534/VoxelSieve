#pragma once

// Beam hardening correction of reconstructed volumes (docs/adr/0033-beam-hardening-correction.md).
//
// A polychromatic X-ray beam gets harder on its way through the part: the measured projection
// grows less than linearly with the path length, and the reconstruction shows material darker far
// from the surface (cupping) and dark streaks between dense or thick regions. Without the
// projections, the correction works on the reconstructed volume: it projects the volume again,
// raises the path lengths to the powers 2, 3, ... and reconstructs them (empirical, image-based
// beam hardening correction), and adds the combination of those images that makes the material
// most uniform. A simpler method scales the grey values by the depth below the surface. Both
// estimate the correction on a coarse level of the dataset and write a new dataset with the bricks
// and active voxels of the input, so they work on datasets larger than RAM.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "voxelsieve/dataset.hpp"

namespace voxelsieve {

/// Two-dimensional parallel-beam projection and filtered back projection of an image of `nx` x
/// `ny` pixels (x fastest) with a pixel pitch in mm per axis, centred on the image centre. The
/// projection is a sinogram of `angles()` rows over 180 degrees with `bins()` detector bins of
/// `binMm()` each, wide enough for the image diagonal; its values are line integrals in mm times
/// the image value. `reconstruct` inverts `project` with a ramp (Ram-Lak) filter.
class ParallelBeam {
 public:
  /// `angles` 0: pi / 2 times the larger image edge, enough for the pixels at the rim.
  ParallelBeam(std::int64_t nx, std::int64_t ny, double pitch_x_mm, double pitch_y_mm,
               int angles = 0);

  [[nodiscard]] int angles() const { return angles_; }
  [[nodiscard]] int bins() const { return bins_; }
  [[nodiscard]] double binMm() const { return bin_mm_; }

  /// Line integrals of `image` (nx * ny values) along the rays of every angle and bin, angle
  /// slowest. The image is interpolated bilinearly and zero outside.
  [[nodiscard]] std::vector<float> project(std::span<const float> image) const;
  /// Filtered back projection of a sinogram of `angles() * bins()` values into nx * ny pixels.
  [[nodiscard]] std::vector<float> reconstruct(std::span<const float> sinogram) const;

 private:
  std::int64_t nx_;
  std::int64_t ny_;
  double pitch_x_;
  double pitch_y_;
  int angles_;
  int bins_;
  double bin_mm_;
  std::vector<double> cos_;
  std::vector<double> sin_;
  std::vector<double> ramp_;  // filter taps at bin offsets 0, 1, ..., bins - 1
};

/// Grey value of the material by depth below the surface, measured on a coarse level of a
/// dataset: the median of the material voxels (above the threshold) in each depth bin, one voxel
/// of that level wide. Voxels in the outermost bin are partial volume and left out.
struct CuppingProfile {
  /// Level of the dataset the profile is measured on.
  int level = 0;
  /// Centre of each bin in mm below the surface, and the material grey value there.
  std::vector<double> depth_mm;
  std::vector<double> level_grey;
  double air_level = 0.0;
  /// (surface level - core level) / (surface level - air): the share by which the material is
  /// darker in the core than at the surface. The surface level is that of the first bin, the core
  /// level the median of the levels in the deepest fifth of the depth range. Negative when the
  /// core is brighter; 0 without at least two bins.
  double cupping = 0.0;
};

enum class BeamHardeningMethod : std::uint8_t { kReprojection, kDepth };

/// "reprojection", "depth".
[[nodiscard]] std::string_view beamHardeningMethodName(BeamHardeningMethod method);
/// Inverse of beamHardeningMethodName; throws std::invalid_argument for other names.
[[nodiscard]] BeamHardeningMethod parseBeamHardeningMethod(std::string_view name);

struct BeamHardeningOptions {
  BeamHardeningMethod method = BeamHardeningMethod::kReprojection;
  /// Reprojection: the rotation axis of the scan (0 x, 1 y, 2 z). Each slice across it is
  /// projected as a parallel beam; cone-beam angles are neglected.
  int rotation_axis = 2;
  /// Reprojection: highest power of the path length in the correction, 2 to 4. 2 is enough for
  /// mild hardening; 3 also follows hardening that saturates on long paths through dense
  /// material.
  int order = 3;
  /// The correction is estimated on the finest level of the dataset whose voxels across the
  /// largest edge are at most this many (the coarsest level if none is), and interpolated to
  /// level 0. Beam hardening varies slowly, so a few hundred voxels suffice.
  std::int64_t resolution = 256;
  /// Stored value type and progress, as for writeDerivedDataset. Integer types round the
  /// corrected values and clamp them to the type's range.
  DerivedDatasetOptions output;
};

struct BeamHardeningInfo {
  DatasetInfo dataset;
  BeamHardeningMethod method = BeamHardeningMethod::kReprojection;
  /// Level of the input the correction was estimated on.
  int level = 0;
  /// Reprojection: the weight of the reconstructed path length to the power 2, 3, ...
  std::vector<double> coefficients;
  /// Material voxels the correction was fitted to, after outliers (other materials, pores) were
  /// left out, and the standard deviation of their grey values before and after the correction.
  std::int64_t fit_voxels = 0;
  double material_spread_before = 0.0;
  double material_spread_after = 0.0;
  /// The material by depth in the input and in the result, on the same level.
  CuppingProfile before;
  CuppingProfile after;
};

/// The level of `dataset` on which a correction with this resolution is estimated.
[[nodiscard]] int beamHardeningLevel(const Dataset& dataset, std::int64_t resolution);

/// Measures the material by depth below the surface on `level` of the dataset.
[[nodiscard]] CuppingProfile measureCupping(const Dataset& dataset, int level);

/// Corrects beam hardening in level 0 of `dataset` and writes the result as a new dataset to
/// `dir`, which must not exist or be empty, with the same bricks and active voxels; the coarser
/// levels are rebuilt from the corrected values.
///
/// Reprojection (default): every slice across the rotation axis of the coarse level is projected
/// as a parallel beam; the path lengths p (the projection of the grey values above air,
/// normalised to the largest) are raised to the powers 2 to `order` and reconstructed. The result
/// is the input plus the combination of those images whose weights make the material most uniform
/// (least squares over the material voxels below the surface, repeated with outliers such as a
/// second material or pores left out). This corrects cupping and the streaks between dense
/// regions alike, without knowing the spectrum.
///
/// Depth: the grey value above air is scaled by the ratio of the material level at the surface to
/// the material level at the voxel's depth (measureCupping). This corrects cupping of one material
/// without assumptions on the scan geometry, but no streaks.
BeamHardeningInfo correctBeamHardening(const Dataset& dataset, const std::filesystem::path& dir,
                                       const BeamHardeningOptions& options = {});

}  // namespace voxelsieve
