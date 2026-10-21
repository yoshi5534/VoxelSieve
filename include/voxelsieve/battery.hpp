#pragma once

// Inspection of battery cells (ADR 0029): the housing and its deformation, the electrode layers
// (count, pitch, gaps), the anode overhang at every edge of the electrode stack, foreign
// particles and gas voids. Works for cylindrical cells (wound electrodes) and stacked cells
// (pouch, prismatic). The cell axis or stack normal lies along a volume axis; tilts of a few
// degrees are followed.

#include <array>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "voxelsieve/cells.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

struct BatteryOptions {
  /// Cell type; unset: a cell with a round cross-section is cylindrical, any other stacked.
  std::optional<CellType> cell_type;
  /// Cell axis (cylindrical) or stack normal (stacked), 0 to 2; unset: from the shape.
  std::optional<int> axis;
  /// Radial profiles per cross-section of a cylindrical cell.
  int angles = 360;
  /// Cross-sections along the axis (cylindrical) or profiles per in-plane direction (stacked)
  /// for the housing, the deformation and the layers.
  int sections = 5;
  /// Sections across the electrode edges for the overhang: angles around a cylindrical cell, or
  /// positions per edge of a stacked cell.
  int overhang_sections = 36;
  /// Voxels on each side across which profiles are averaged against noise, along the layers.
  int average_voxels = 2;
  /// Whether the cathode layers are the brighter electrode (metal oxide or phosphate cathodes
  /// next to graphite anodes); false when the anode is brighter, for example when the copper
  /// foil of the anode is resolved and outshines the cathode coating.
  bool cathode_brighter = true;
  /// Grey value from which a voxel in the electrode stack is a foreign particle; unset: as far
  /// above the brighter electrode as that is above the gaps between the layers.
  std::optional<float> particle_threshold;
  /// Grey value below which a voxel in the electrode stack is gas; unset: half way between air
  /// and the gaps between the layers.
  std::optional<float> void_threshold;
  std::int64_t min_particle_voxels = 2;
  /// Bright components longer than this are taken for parts of the cell, such as tabs, foil ends
  /// or welds, not for particles.
  double max_particle_size_mm = 1.0;
  std::int64_t min_void_voxels = 8;
  /// A space between neighbouring layers more than this factor above the median is a gap
  /// (delamination, a fold or a missing layer).
  double gap_factor = 1.6;
  /// Layer ends farther than this from the median end of their edge are left out of the
  /// overhang: layers lost by the tracking, tabs, the start of a strip.
  double max_end_deviation_mm = 1.0;
  /// Edge of a pixel of section images (analyzeBatteryImages); unset: from the positions of the
  /// cross-sections over the height of the longitudinal sections.
  std::optional<double> image_pixel_mm;
};

/// Grey value levels found in the cell.
struct BatteryLevels {
  double air = 0.0;
  double gap = 0.0;  // separator and electrolyte between the layers
  double anode = 0.0;
  double cathode = 0.0;
  double housing = 0.0;
  /// Whether anode and cathode layers could be told apart; without that there is no overhang.
  bool electrodes_distinct = false;
};

/// One cross-section of a cylindrical cell (normal to the axis), or one profile position of a
/// stacked cell (along the normal). Positions in mm are voxel index times voxel size.
struct BatterySection {
  /// Cylindrical: position along the axis. Stacked: in-plane position (two values, u and v).
  std::array<double, 2> position_mm{};
  /// Cylindrical: centre of the can in the plane (the two other axes in ascending order).
  std::array<double, 2> center_mm{};
  /// Cylindrical: diameter of the circle fitted to the can's outer surface. Stacked: outer
  /// thickness of the cell along the normal.
  double outer_mm = 0.0;
  /// Cylindrical: largest minus smallest outer radius about that centre.
  double out_of_roundness_mm = 0.0;
  /// Largest inward deviation of the outer surface from the fitted circle (a dent).
  double dent_mm = 0.0;
  /// Wall of the can or housing, NaN where it cannot be told from what lies inside.
  double min_wall_mm = 0.0;
  double mean_wall_mm = 0.0;
  /// Cylindrical: diameter and out-of-roundness of the core, 0 without a hollow core.
  double core_diameter_mm = 0.0;
  double core_out_of_roundness_mm = 0.0;
  /// Distance of the core centre from the can centre.
  double core_offset_mm = 0.0;
  /// Cylindrical: diameter over the outermost electrode layer. Stacked: thickness from the first
  /// to the last electrode layer.
  double stack_mm = 0.0;
  /// Layers crossed by the profiles (median).
  int cathode_layers = 0;
  int anode_layers = 0;
  /// Median distance between neighbouring cathode layers.
  double pitch_mm = 0.0;
  /// Outer and core radius per angle (cylindrical, `angles` values from the first in-plane axis
  /// towards the second).
  std::vector<double> outer_radius_mm;
  std::vector<double> core_radius_mm;
};

/// A space between neighbouring layers much larger than the others.
struct LayerGap {
  std::array<double, 3> position_mm{};
  double size_mm = 0.0;      // distance between the layers
  double expected_mm = 0.0;  // median distance between neighbouring layers
  int section = 0;
};

/// The ends of a cathode layer and its neighbouring anode layers in one section across an edge.
struct OverhangSample {
  int edge = 0;            // index into BatteryResult::edges
  double section = 0.0;    // cylindrical: angle in degrees; stacked: position along the edge in mm
  int layer = 0;           // cathode layer from the core or the low end of the normal, from 0
  double across_mm = 0.0;  // radius, or position along the normal
  double anode_across_mm = 0.0;  // the same for the anode neighbour at its end
  double cathode_end_mm = 0.0;   // position along the edge direction
  double anode_end_mm = 0.0;     // the anode neighbour whose end gives the smaller overhang
  /// Anode end beyond the cathode end, outwards; negative when the cathode sticks out.
  double overhang_mm = 0.0;
};

/// One edge of the electrode stack: an end of a cylindrical cell or a side of a stack.
struct BatteryEdge {
  int axis = 0;
  int side = 1;      // 0: the low end of the axis, 1: the high end
  std::string name;  // "z min", "x max", ...
  std::int64_t samples = 0;
  double min_mm = 0.0;
  double mean_mm = 0.0;
  double max_mm = 0.0;
  double std_mm = 0.0;
  /// Index of the sample with the smallest overhang, -1 without samples.
  std::int64_t worst = -1;
  /// Spread (largest minus smallest) of the positions of the cathode and anode ends: an uneven
  /// edge from telescoping or shifted sheets.
  double cathode_edge_spread_mm = 0.0;
  double anode_edge_spread_mm = 0.0;
  /// Mean position of the anode ends along the axis, the end of the electrode stack, and of the
  /// cathode ends.
  double anode_edge_mm = 0.0;
  double cathode_edge_mm = 0.0;
};

/// A foreign particle or gas void in the electrode stack.
struct CellFinding {
  int id = 0;
  std::array<double, 3> center_mm{};
  std::int64_t voxels = 0;
  double volume_mm3 = 0.0;
  /// Longest edge of the bounding box.
  double size_mm = 0.0;
  double equivalent_diameter_mm = 0.0;
  /// Brightest (particle) or darkest (void) grey value.
  double extreme_value = 0.0;
  /// Cylindrical: radius and angle in the cell; stacked: position along the normal.
  double radius_mm = 0.0;
  double angle_deg = 0.0;
};

/// An image of the result, rendered during the analysis from the sections it reads.
struct BatteryImage {
  std::string name;  // file name without extension
  std::int64_t width = 0;
  std::int64_t height = 0;
  std::vector<std::uint8_t> rgb;  // rows top to bottom
  double pixel_width_mm = 0.0;
  double pixel_height_mm = 0.0;
};

struct BatteryResult {
  CellType type = CellType::kCylindrical;
  int axis = 2;
  std::array<std::int64_t, 3> dims{};
  VoxelSize voxel_size;
  BatteryLevels levels;
  /// The region analysed: the bounding box of the cell with a small margin, in mm.
  std::array<double, 3> bounds_min_mm{};
  std::array<double, 3> bounds_max_mm{};
  /// Outer size of the cell along each axis, measured at half height of the housing on a line
  /// through the middle (cylindrical: through the axis, so the first two in-plane values are
  /// diameters).
  std::array<double, 3> outer_size_mm{};
  /// Outer size along the axis (cylindrical) or the normal (stacked).
  double length_mm = 0.0;
  std::vector<BatterySection> sections;
  /// Over all sections: number of layers (median), pitch, outer size, roundness, wall.
  int cathode_layers = 0;
  int anode_layers = 0;
  double pitch_mm = 0.0;
  double outer_mean_mm = 0.0;
  double outer_min_size_mm = 0.0;
  double outer_max_size_mm = 0.0;
  double max_out_of_roundness_mm = 0.0;
  double max_dent_mm = 0.0;
  double min_wall_mm = 0.0;
  double max_core_out_of_roundness_mm = 0.0;
  std::vector<LayerGap> gaps;
  std::vector<BatteryEdge> edges;
  std::vector<OverhangSample> overhang;
  /// Smallest overhang over all edges, NaN without samples.
  double min_overhang_mm = 0.0;
  double particle_threshold = 0.0;
  double void_threshold = 0.0;
  std::vector<CellFinding> particles;  // largest first
  std::vector<CellFinding> voids;      // largest first
  /// Bright components in the electrode stack too large for particles (tabs, foil, welds).
  std::int64_t bright_structures = 0;
  /// Analysed from section images instead of a volume: particles and voids are their sections in
  /// the images (voxels are pixels, no volume), positions along the axis count from the bottom
  /// row of the longitudinal sections.
  bool from_images = false;
  std::vector<std::string> warnings;
  /// cross_section (normal to the axis, or across the layers of a stack along its longer side)
  /// with the layers, particles and voids marked, longitudinal (along the axis through the
  /// middle, or across the layers along the shorter side) and overhang_<edge> (the section with
  /// the smallest overhang at each edge, its electrode ends marked). Not in the JSON.
  std::vector<BatteryImage> images;
};

/// A grey image of a section through a cell, rows top to bottom.
struct CellImage {
  /// Longitudinal section: angle in degrees; cross-section: position along the axis in mm.
  double position = 0.0;
  std::int64_t width = 0;
  std::int64_t height = 0;
  std::vector<std::uint16_t> values;
};

/// A cylindrical cell given as section images instead of a volume (ADR 0029): longitudinal
/// sections through the axis, the axis vertical, the top row at the high end; the right half of
/// the image lies at the section's angle, the left half at the angle plus 180 degrees. The
/// longitudinal section at 0 degrees runs along the x axis of the cross-sections, which are
/// normal to the axis. All images have square pixels of the same size.
struct CellImages {
  std::string cell;
  std::vector<CellImage> longitudinal;  // ascending angle
  std::vector<CellImage> cross;         // ascending position
};

/// The cells in a folder or ZIP archive of section images: for every cell a folder holding
/// `axial_images/<angle>.png` (longitudinal sections) and `radial_images/<position>.png`
/// (cross-sections). Names are the cell folders' names, in natural order.
[[nodiscard]] std::vector<std::string> listCellImages(const std::filesystem::path& source);

/// Reads the section images of `cell` (a name from listCellImages, or the path of its folder in
/// the source).
[[nodiscard]] CellImages readCellImages(const std::filesystem::path& source,
                                        const std::string& cell);

/// Inspects a cylindrical cell from its section images: housing and layers from the
/// cross-sections, the overhang at both ends from the longitudinal sections, particles and voids
/// in both.
[[nodiscard]] BatteryResult analyzeBatteryImages(const CellImages& images,
                                                 const BatteryOptions& options = {});

/// Inspects the battery cell in `dataset` (level 0).
[[nodiscard]] BatteryResult analyzeBattery(const Dataset& dataset,
                                           const BatteryOptions& options = {});

[[nodiscard]] nlohmann::json toJson(const BatteryResult& result);

/// Writes battery.json and the images as PNG files.
void writeBatteryResult(const BatteryResult& result, const std::filesystem::path& dir);

}  // namespace voxelsieve
