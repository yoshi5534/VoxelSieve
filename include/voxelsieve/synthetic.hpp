#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>

#include "voxelsieve/mesh.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// Void sphere in mm, in the coordinates of the mesh.
struct Sphere {
  std::array<double, 3> center_mm{0.0, 0.0, 0.0};
  double radius_mm = 0.0;
};

enum class DefectType : std::uint8_t {
  /// Shrinkage cavity: an irregular void made of a few overlapping spheres.
  kLunker,
  /// Loosened microstructure ("Gefügeauflockerung"): a zone with many small pores near or below
  /// the voxel size, which lowers the grey value more than it shows single pores.
  kLoosening,
};

/// Ground truth of one artificial defect.
struct Defect {
  DefectType type = DefectType::kLunker;
  std::array<double, 3> center_mm{0.0, 0.0, 0.0};
  /// Radius of the sphere that encloses the defect (the zone radius for loosening).
  double radius_mm = 0.0;
  std::vector<Sphere> spheres;
  /// Void volume; estimated by Monte Carlo for overlapping lunker spheres, exact for loosening
  /// (its pores do not overlap).
  double void_volume_mm3 = 0.0;
};

/// Settings for a synthetic CT scan of a mesh. Sizes are in mm; zero counts or strengths disable
/// the respective feature.
struct SyntheticSpec {
  VoxelSize voxel_size{0.1};
  /// Air around the mesh bounding box on every side.
  double padding_mm = 1.0;
  std::uint16_t air_value = 1000;
  std::uint16_t material_value = 20000;
  std::uint64_t seed = 42;

  int lunker_count = 0;
  /// Enclosing radius of a lunker; 0 picks 5 % of the smallest mesh extent.
  double lunker_radius_mm = 0.0;

  int loosening_count = 0;
  /// Zone radius; 0 picks 10 % of the smallest mesh extent.
  double loosening_radius_mm = 0.0;
  /// Void fraction inside a loosening zone.
  double loosening_porosity = 0.05;
  /// Pore radius inside a loosening zone; 0 picks 0.6 voxels.
  double loosening_pore_radius_mm = 0.0;

  /// Standard deviation of Gaussian noise in grey values.
  double noise_sigma = 0.0;
  /// Beam hardening: relative darkening of material far from the surface (cupping), for example
  /// 0.1 for 10 %.
  double cupping = 0.0;
  /// Depth below the surface over which cupping reaches 63 % of its strength; 0 picks 20 % of the
  /// smallest mesh extent.
  double cupping_depth_mm = 0.0;
  /// Unsharpness of the imaging chain (focal spot, detector, reconstruction filter): standard
  /// deviation of a Gaussian point spread function in mm applied to the grey values before rings
  /// and noise, for example 0.7 voxels. 0 keeps the sharp partial-volume edges.
  double blur_sigma_mm = 0.0;
  /// Ring artefacts around the z axis through the volume centre (the rotation axis).
  int ring_count = 0;
  /// Standard deviation of the ring amplitudes in grey values.
  double ring_strength = 0.0;
};

/// Synthetic CT scan of a closed mesh with artificial defects, noise and artefacts.
///
/// Voxel (0, 0, 0) is centred at mesh bounds minus padding plus half a voxel. Grey values follow
/// the same linear partial-volume model as the box phantom; voxels touched by a defect are
/// supersampled so that small pores lower the grey value by their actual volume. Voxels are
/// computed on demand, so scans larger than memory can be streamed through `readRegion`.
class SyntheticScan final : public VolumeSource {
 public:
  SyntheticScan(const Mesh& mesh, const SyntheticSpec& spec);
  ~SyntheticScan() override;

  [[nodiscard]] std::array<std::int64_t, 3> dims() const override;
  [[nodiscard]] VoxelSize voxelSize() const override;
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

  [[nodiscard]] const SyntheticSpec& spec() const;
  [[nodiscard]] const std::vector<Defect>& defects() const;
  /// Mesh coordinates of the centre of voxel (0, 0, 0).
  [[nodiscard]] std::array<double, 3> originMm() const;
  [[nodiscard]] double meshVolumeMm3() const;
  /// Warnings about defects that could not be placed, for example in thin parts.
  [[nodiscard]] const std::vector<std::string>& warnings() const;

  /// Signed distance to the material surface including defects in mm, negative in material.
  [[nodiscard]] double signedDistanceMm(const std::array<double, 3>& point_mm) const;

  /// Raw-file sidecar with the format, the scan settings and the ground truth.
  [[nodiscard]] nlohmann::json toJson() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string toString(DefectType type);

}  // namespace voxelsieve
