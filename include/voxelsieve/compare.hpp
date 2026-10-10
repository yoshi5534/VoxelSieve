#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>

#include "voxelsieve/mesh.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/transform.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// Nominal-actual comparison: the surface of a scan against the nominal (CAD) geometry of the
/// part as STL. The CAD mesh is aligned to the scan, then every point of the scanned surface gets
/// its signed distance to the CAD surface: positive where the part has more material than
/// nominal, negative where material is missing.

/// Exact signed distance to a closed triangle mesh (positive outside), with a bounding volume
/// hierarchy over the triangles. Inverted meshes (negative volume) are turned outward.
class MeshDistance {
 public:
  explicit MeshDistance(const Mesh& mesh);
  MeshDistance(MeshDistance&&) noexcept;
  MeshDistance& operator=(MeshDistance&&) noexcept;
  MeshDistance(const MeshDistance&) = delete;
  MeshDistance& operator=(const MeshDistance&) = delete;
  ~MeshDistance();

  struct Hit {
    double distance = 0.0;            // signed, mm
    std::array<double, 3> closest{};  // closest point on the mesh
    std::array<double, 3> normal{};   // unit direction with distance = normal . (p - closest)
  };
  [[nodiscard]] Hit query(const std::array<double, 3>& p) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct CompareOptions {
  enum class Alignment : std::uint8_t {
    kAuto,    // coarse alignment of the principal axes, then fine alignment (ICP)
    kRefine,  // fine alignment only, starting at `initial`
    kNone,    // use `initial` as it is
  };
  Alignment alignment = Alignment::kAuto;
  /// CAD to scan; the start of kRefine and the transform of kNone.
  RigidTransform initial;
  /// Deviations within +-tolerance count as in tolerance and are shown green.
  double tolerance_mm = 0.1;
  /// Compares only the largest connected surface, the outer skin: surfaces of closed internal
  /// voids (pores) are left out. They are counted in the result.
  bool outer_surface_only = true;
  /// Points sampled on the scanned surface for the fine alignment.
  std::size_t fit_points = 20000;
  /// Triangle budget of the scanned surface mesh (surfaceDisplayMesh).
  std::size_t max_triangles = 2000000;
  /// Non-rigid registration (ADR 0022) for parts that do not keep their nominal shape in the
  /// scanner, such as thin, flexible parts: after the rigid alignment, the scanned surface is
  /// bent onto the CAD model by a smooth, elastic deformation, and the deviation is measured on
  /// the bent surface. How far every point was moved is reported beside its deviation.
  struct Deformation {
    bool enabled = false;
    /// Spacing of the control points, mm: the shortest stretch over which the deformation can
    /// change. Features smaller than that stay deviations. 0: an eighth of the largest extent of
    /// the part.
    double spacing_mm = 0.0;
    /// Resistance against stretching and compressing, relative to the fit, without unit. Bending
    /// strains thin sections little and thick ones much, so a high value lets thin sections bend
    /// and keeps thick ones and the size of the part; a low value lets the deformation follow
    /// almost any smooth deviation.
    double stiffness = 1.0;
  };
  Deformation deformation;
};

[[nodiscard]] const char* toString(CompareOptions::Alignment alignment);
/// "auto", "refine" or "none"; throws otherwise.
[[nodiscard]] CompareOptions::Alignment alignmentFromString(const std::string& name);

struct DeviationStats {
  std::size_t vertices = 0;
  double area_mm2 = 0.0;
  /// Area-weighted moments and percentiles of the signed deviation, mm.
  double mean_mm = 0.0;
  double rms_mm = 0.0;
  double std_mm = 0.0;
  double min_mm = 0.0;
  double max_mm = 0.0;
  std::array<double, 5> percentiles_mm{};  // 1, 5, 50, 95, 99 %
  /// Area fractions within, above and below +-tolerance.
  double within_tolerance = 0.0;
  double above_tolerance = 0.0;
  double below_tolerance = 0.0;
  /// Colour and histogram range +-range_mm; area fraction per bin, the outer bins include what
  /// lies beyond.
  double range_mm = 0.0;
  std::vector<double> histogram;
};

inline constexpr std::array<double, 5> kDeviationPercentiles{1.0, 5.0, 50.0, 95.0, 99.0};

/// The non-rigid registration of a comparison, when it was asked for.
struct DeformationResult {
  bool applied = false;
  double spacing_mm = 0.0;
  double stiffness = 0.0;
  /// Control points per axis of the cubic B-spline deformation.
  std::array<std::size_t, 3> control_points{};
  /// Fit of the bent surface: RMS distance of the inlier points and their fraction.
  double fit_rms_mm = 0.0;
  double fit_inliers = 0.0;
  int iterations = 0;
  /// How far every vertex of the compared surface was moved, mm, and the statistics of that
  /// (area-weighted, as the deviation; the tolerance bands count the area moved by more than the
  /// tolerance).
  std::vector<float> displacement_mm;
  DeviationStats displacement;
  /// The deviation after the rigid alignment alone, for comparison.
  DeviationStats rigid;
};

struct CompareResult {
  RigidTransform cad_to_scan;
  CompareOptions::Alignment alignment = CompareOptions::Alignment::kAuto;
  /// Fine alignment: RMS distance of the inlier points and their fraction.
  double fit_rms_mm = 0.0;
  double fit_inliers = 0.0;
  int fit_iterations = 0;
  double tolerance_mm = 0.0;
  VoxelSize voxel_size;
  /// Connected surfaces of the scan, and those left out (internal voids).
  std::size_t components = 0;
  std::size_t dropped_components = 0;
  double dropped_area_mm2 = 0.0;
  std::size_t cad_triangles = 0;
  DeviationStats stats;
  /// Compared surface of the scan in mm (dataset world space) and the deviation per vertex,
  /// measured after the deformation when there is one.
  IndexedMesh mesh;
  std::vector<float> deviation_mm;
  DeformationResult deformation;
};

/// The surface of a scan in mm, in the scan's own coordinates (voxel index times voxel size): the
/// zero crossing of the mask, meshed without adaptivity so every vertex lies on the surface. With
/// `outer_only`, only the largest connected surface, the outer skin, is kept.
struct ScanSurface {
  IndexedMesh mesh;
  std::size_t components = 0;
  std::size_t dropped_components = 0;
  double dropped_area_mm2 = 0.0;
};
[[nodiscard]] ScanSurface scanSurface(const SurfaceMask& mask, std::size_t max_triangles,
                                      bool outer_only);

/// The triangles of `mesh` with equal corners merged into shared vertices.
[[nodiscard]] IndexedMesh indexedMesh(const Mesh& mesh);
[[nodiscard]] Mesh triangleSoup(const IndexedMesh& mesh);
[[nodiscard]] IndexedMesh transformed(IndexedMesh mesh, const RigidTransform& transform);

/// Least-squares rigid motion that maps `from[i]` onto `to[i]` (Horn's quaternion method), with
/// the remaining distance of every pair. Throws for fewer than three pairs or points on a line.
struct RigidFit {
  RigidTransform transform;
  std::vector<double> residuals_mm;
  double rms_mm = 0.0;
};
[[nodiscard]] RigidFit fitRigid(std::span<const std::array<double, 3>> from,
                                std::span<const std::array<double, 3>> to);

/// Best fit of one surface onto another (ADR 0018), both in the same coordinates: robust
/// point-to-plane ICP as in the nominal-actual comparison (ADR 0010), starting from where the
/// surfaces lie or, with `coarse`, from the best match of their principal axes.
struct SurfaceAlignOptions {
  bool coarse = false;
  std::size_t fit_points = 20000;
  /// Distances below this count as noise (Huber threshold floor, convergence); 0: a ten-thousandth
  /// of the target's size. Half a voxel for scans.
  double resolution_mm = 0.0;
};
struct SurfaceAlignment {
  /// Moves the moving surface onto the target.
  RigidTransform motion;
  /// RMS distance of the inlier points and their fraction.
  double rms_mm = 0.0;
  double inliers = 0.0;
  int iterations = 0;
  double resolution_mm = 0.0;
};
[[nodiscard]] SurfaceAlignment alignSurfaces(const IndexedMesh& moving, const IndexedMesh& target,
                                             const SurfaceAlignOptions& options = {});

/// Aligns `cad` to the surface of the scan and measures the deviation of the scanned surface.
[[nodiscard]] CompareResult compareToCad(const SurfaceMask& mask, const Mesh& cad,
                                         const CompareOptions& options = {});

/// Everything but the mesh, for compare.json and step summaries.
[[nodiscard]] nlohmann::json toJson(const CompareResult& result);

/// Colour of a deviation: green within tolerance, yellow to red above, cyan to blue below,
/// saturated at +-range.
[[nodiscard]] std::array<std::uint8_t, 3> deviationColor(double deviation_mm, double tolerance_mm,
                                                         double range_mm);

/// Directions of the rendered views deviation_view_1.png, deviation_view_2.png: azimuth and
/// elevation in degrees as in RenderView, one from above and one from below the other side.
inline constexpr std::array<std::array<double, 2>, 2> kDeviationViews{
    {{-60.0, 25.0}, {120.0, -25.0}}};

/// Writes compare.json, deviation.ply (binary PLY with a float `deviation` and colours per
/// vertex) and deviation_view_[12].png (shaded views, see kDeviationViews) into `dir`. With a
/// deformation, deviation.ply also has a float `displacement` per vertex, and
/// displacement_view_[12].png show it.
void writeComparison(const CompareResult& result, const std::filesystem::path& dir);

/// Writes the CAD model moved into scan coordinates as STL.
void writeAlignedCad(const CompareResult& result, const Mesh& cad,
                     const std::filesystem::path& file);

/// The mesh and deviations of a deviation.ply written by writeComparison.
struct DeviationMesh {
  IndexedMesh mesh;  // mm
  std::vector<float> deviation_mm;
  /// Empty without a deformation.
  std::vector<float> displacement_mm;
};
[[nodiscard]] DeviationMesh readDeviationPly(const std::filesystem::path& file);

}  // namespace voxelsieve
