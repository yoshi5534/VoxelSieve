#pragma once

// Wall thickness of a part at its surface (ADR 0030). Every point of the extracted surface gets
// the thickness of the wall it lies on, by one of two methods:
//
// - ray: from the point along the inward surface normal through the grey values of the scan to
//   where they fall below the surface grey value again, the opposite side of the wall. Sub-voxel
//   accurate for walls with roughly parallel faces; a ray that leaves the wall at a steep angle,
//   through an edge or along a rib, finds no opposite face and leaves the point unmeasured.
// - sphere: the diameter of the largest sphere inside the material that touches the surface at
//   the point (shrinking ball). Defined everywhere, also in corners and at ribs; it is the local
//   thickness that castings are designed with.
//
// Internal pores reduce the wall: a ray stops at a pore and a sphere does not reach into one.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

struct WallThicknessOptions {
  enum class Method : std::uint8_t { kRay, kSphere };
  Method method = Method::kRay;
  /// Walls up to this thickness are measured; thicker ones stay unmeasured. 0: half the middle
  /// extent of the part's bounding box.
  double max_thickness_mm = 0.0;
  /// Ray method: the face where the ray leaves the wall counts as its opposite side when its
  /// normal lies within this angle of the ray, in degrees.
  double search_angle_deg = 30.0;
  /// Limits of the drawing: surface regions thinner than `min_wall_mm` or thicker than
  /// `max_wall_mm` are listed as regions of their own. 0: no limit.
  double min_wall_mm = 0.0;
  double max_wall_mm = 0.0;
  /// Measures only the largest connected surface, the outer skin; the surfaces of closed internal
  /// voids (pores) still limit the walls but get no values of their own.
  bool outer_surface_only = true;
  /// Triangle budget of the surface mesh whose vertices are measured (surfaceDisplayMesh).
  std::size_t max_triangles = 2000000;
  /// Called with the fraction of the measurement done, 0 to 1, from one thread at a time.
  std::function<void(double fraction)> progress;
};

[[nodiscard]] const char* toString(WallThicknessOptions::Method method);
/// "ray" or "sphere"; throws otherwise.
[[nodiscard]] WallThicknessOptions::Method thicknessMethodFromString(const std::string& name);

inline constexpr std::array<double, 5> kThicknessPercentiles{1.0, 5.0, 50.0, 95.0, 99.0};

/// Area-weighted statistics of the thickness over the measured surface.
struct ThicknessStats {
  std::size_t vertices = 0;
  double area_mm2 = 0.0;           // surface the thickness was measured on
  double measured_area_mm2 = 0.0;  // the part of it with a value
  double min_mm = 0.0;
  double max_mm = 0.0;
  double mean_mm = 0.0;
  double std_mm = 0.0;
  std::array<double, 5> percentiles_mm{};  // 1, 5, 50, 95, 99 %
  /// Fractions of the measured area below `min_wall_mm` and above `max_wall_mm`.
  double below_min = 0.0;
  double above_max = 0.0;
  /// Colour and histogram range 0 to range_mm; measured area fraction per bin, the last bin
  /// includes what lies beyond.
  double range_mm = 0.0;
  std::vector<double> histogram;
};

/// Connected surface region thinner than the minimum or thicker than the maximum wall.
struct ThicknessRegion {
  int id = 0;
  std::size_t vertices = 0;
  double area_mm2 = 0.0;
  /// Thinnest (thin regions) or thickest (thick regions) value and where it lies, mm in the
  /// dataset's coordinates.
  double extreme_mm = 0.0;
  std::array<double, 3> location_mm{};
  std::array<std::array<double, 3>, 2> bounds_mm{};
};

struct WallThicknessResult {
  WallThicknessOptions::Method method = WallThicknessOptions::Method::kRay;
  VoxelSize voxel_size;
  float iso_value = 0.0F;
  double max_thickness_mm = 0.0;
  double search_angle_deg = 0.0;
  double min_wall_mm = 0.0;
  double max_wall_mm = 0.0;
  /// Connected surfaces of the scan and those that got no values (internal voids).
  std::size_t components = 0;
  std::size_t dropped_components = 0;
  /// Measured surface in mm (dataset coordinates, voxel index times voxel size) and the thickness
  /// per vertex in mm, NaN where no wall up to `max_thickness_mm` was found.
  IndexedMesh mesh;
  std::vector<float> thickness_mm;
  ThicknessStats stats;
  std::vector<ThicknessRegion> thin_regions;   // thinnest first
  std::vector<ThicknessRegion> thick_regions;  // thickest first
};

/// Measures the wall thickness on the surface `surface` of `dataset` (level 0). The mesh is held
/// in memory within `max_triangles`; the grey values are read through the dataset's brick cache
/// along the rays, so memory does not grow with the scan (ADR 0004).
[[nodiscard]] WallThicknessResult analyzeWallThickness(const Dataset& dataset,
                                                       const SurfaceMask& surface,
                                                       const WallThicknessOptions& options = {});

/// Statistics of the thickness per vertex of `mesh` (mm, NaN where not measured) against the limits
/// (0: none), weighted by the area around each vertex.
[[nodiscard]] ThicknessStats thicknessStats(const IndexedMesh& mesh,
                                            const std::vector<float>& thickness_mm,
                                            double min_wall_mm, double max_wall_mm);
/// Connected regions of `mesh` thinner than `limit_mm` (thinnest first) or thicker than it
/// (thickest first).
[[nodiscard]] std::vector<ThicknessRegion> thinRegions(const IndexedMesh& mesh,
                                                       const std::vector<float>& thickness_mm,
                                                       double limit_mm);
[[nodiscard]] std::vector<ThicknessRegion> thickRegions(const IndexedMesh& mesh,
                                                        const std::vector<float>& thickness_mm,
                                                        double limit_mm);

/// The result without mesh and values (they go into the PLY file), regions included.
[[nodiscard]] nlohmann::json toJson(const WallThicknessResult& result);

/// Writes wall_thickness.json, thickness.ply (the measured surface in mm with the thickness and a
/// colour per vertex) and views of the coloured surface (thickness_view_1.png and _2.png, the
/// views of kDeviationViews).
void writeWallThickness(const WallThicknessResult& result, const std::filesystem::path& dir);

struct ThicknessMesh {
  IndexedMesh mesh;  // mm
  std::vector<float> thickness_mm;
};
[[nodiscard]] ThicknessMesh readThicknessPly(const std::filesystem::path& file);

/// Colour of a thickness for views and the GUI: red for thin over yellow and green to blue for
/// thick walls up to `range_mm`; magenta below `min_wall_mm` and violet above `max_wall_mm`
/// (limits of 0 are off); grey where nothing was measured (NaN).
[[nodiscard]] std::array<std::uint8_t, 3> thicknessColor(double thickness_mm, double min_wall_mm,
                                                         double max_wall_mm, double range_mm);

}  // namespace voxelsieve
