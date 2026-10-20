#pragma once

// Synthetic CT scans of battery cells with known geometry (ADR 0029): a cylindrical cell with a
// wound electrode stack (jelly roll) in a steel can, and a stacked cell (pouch or prismatic) with
// flat electrode sheets in a housing. Cathode, anode and separator layers, the anode overhang at
// every edge, foreign particles and gas voids are known exactly, so the battery inspection is
// tested against analytic ground truth.

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <vector>

#include "voxelsieve/source.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

enum class CellType : std::uint8_t {
  /// Electrodes wound around the cell axis in a round can (18650, 21700, 4680 and the like).
  kCylindrical,
  /// Flat electrode sheets stacked along a normal, in a pouch or a prismatic can; also the flat
  /// part of a flat-wound prismatic cell.
  kStacked,
};

/// A sphere in mm relative to the volume centre: a foreign particle or a gas void.
struct CellSphere {
  std::array<double, 3> center_mm{0.0, 0.0, 0.0};
  double radius_mm = 0.0;
};

/// A cathode edge that reaches beyond its nominal position, which shortens the anode overhang
/// there, down to a cathode that sticks out beyond the anode.
struct CathodeExtension {
  /// Cathode layer from 0: the turn of the winding counted from the core, or the cathode sheet
  /// of the stack counted from the low end of the stack normal.
  int layer = 0;
  /// Cylindrical: the end of the cell, 1 at the high end of the axis (z), 0 at the low end.
  /// Stacked: the edge, 0 low x, 1 high x, 2 low y, 3 high y.
  int edge = 1;
  /// Cylindrical: the angular range of the extension around the axis in degrees, measured from
  /// the x axis towards y. Ignored for stacked cells, where the whole edge moves.
  double from_deg = 0.0;
  double to_deg = 360.0;
  double length_mm = 0.0;
};

/// Settings of a synthetic cell. The cylindrical cell's axis and the stack normal are z; the
/// cell is centred in the volume. Lengths in mm.
struct CellSpec {
  CellType type = CellType::kCylindrical;
  std::array<std::int64_t, 3> dims{256, 256, 160};
  VoxelSize voxel_size{0.025};
  std::uint16_t air_value = 1000;
  /// Electrolyte in the separator and every gap inside the housing.
  std::uint16_t electrolyte_value = 6000;
  /// Separator with electrolyte between the electrodes, where the cathodes face the anodes;
  /// unset: the electrolyte value. A value near the anode's hides the anode in the winding, as
  /// hard carbon or graphite anodes are in many scans, while it still stands out beyond the
  /// cathode ends.
  std::optional<std::uint16_t> separator_value;
  std::uint16_t anode_value = 11000;
  std::uint16_t cathode_value = 22000;
  std::uint16_t housing_value = 40000;
  std::uint16_t particle_value = 52000;
  double noise_sigma = 0.0;
  std::uint64_t seed = 7;
  /// Samples per axis and voxel for the partial volume at the edges of layers.
  int supersampling = 2;

  double cathode_thickness_mm = 0.1;
  double anode_thickness_mm = 0.1;
  double separator_thickness_mm = 0.05;
  /// Anode beyond the cathode at the ends (cylindrical: low z, high z) or the edges (stacked: low
  /// x, high x, low y, high y). Only the first two count for a cylindrical cell.
  std::array<double, 4> overhang_mm{0.3, 0.3, 0.3, 0.3};
  double housing_wall_mm = 0.15;
  /// Electrolyte between the electrodes and the housing.
  double housing_gap_mm = 0.2;

  // Cylindrical cell.
  /// Radius of the core: the hole the winding starts around, filled with gas.
  double core_radius_mm = 0.5;
  int turns = 6;
  /// Cathode height along the axis.
  double cathode_height_mm = 2.4;
  /// Extra relative length of the cell along x, 0 for a round cell: 0.02 makes it 2 % wider
  /// along x than along y (a deformed cell).
  double ovality = 0.0;

  // Stacked cell.
  int cathode_layers = 5;  // one more anode sheet, at both ends of the stack
  /// Cathode sheet size along x and y.
  std::array<double, 2> cathode_size_mm{3.4, 2.8};
  /// Electrolyte of this thickness over the whole stack between the separator after cathode
  /// sheet `gap_after` and the next anode sheet: the layers delaminated there. 0: none.
  double gap_mm = 0.0;
  int gap_after = 0;

  std::vector<CellSphere> particles;
  std::vector<CellSphere> voids;
  std::vector<CathodeExtension> extensions;
};

/// A cylindrical cell of six turns with an anode overhang of 0.3 mm, in 256 × 256 × 160 voxels of
/// 25 µm.
[[nodiscard]] CellSpec defaultCylindricalCell();
/// A pouch-like stack of five cathode sheets in 200 × 176 × 104 voxels of 25 µm.
[[nodiscard]] CellSpec defaultStackedCell();

/// Radial pitch of the winding or thickness of one repeat of the stack: cathode, separator,
/// anode, separator.
[[nodiscard]] double cellPitchMm(const CellSpec& spec);

/// Outer radius of the can (cylindrical) and outer size of the housing along x, y, z.
[[nodiscard]] double cellOuterRadiusMm(const CellSpec& spec);
[[nodiscard]] std::array<double, 3> cellOuterSizeMm(const CellSpec& spec);

/// Grey value at `point_mm` relative to the volume centre, without noise and partial volume.
[[nodiscard]] std::uint16_t cellValueAt(const CellSpec& spec,
                                        const std::array<double, 3>& point_mm);

/// Anode overhang of cathode layer `layer` at `edge` (see CathodeExtension), at angle `deg` for
/// a cylindrical cell.
[[nodiscard]] double cellOverhangMm(const CellSpec& spec, int layer, int edge, double deg = 0.0);

/// Ground truth: geometry, overhang per edge (nominal and smallest), particles and voids.
[[nodiscard]] nlohmann::json cellGroundTruth(const CellSpec& spec);

/// Computes the synthetic cell on the fly.
class CellSource final : public VolumeSource {
 public:
  explicit CellSource(CellSpec spec);
  [[nodiscard]] std::array<std::int64_t, 3> dims() const override { return spec_.dims; }
  [[nodiscard]] VoxelSize voxelSize() const override { return spec_.voxel_size; }
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;
  [[nodiscard]] const CellSpec& spec() const { return spec_; }

 private:
  CellSpec spec_;
};

}  // namespace voxelsieve
