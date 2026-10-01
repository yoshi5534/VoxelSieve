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
  /// Compared surface of the scan in mm (dataset world space) and the deviation per vertex.
  IndexedMesh mesh;
  std::vector<float> deviation_mm;
};

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
/// vertex) and deviation_view_[12].png (shaded views, see kDeviationViews) into `dir`.
void writeComparison(const CompareResult& result, const std::filesystem::path& dir);

/// Writes the CAD model moved into scan coordinates as STL.
void writeAlignedCad(const CompareResult& result, const Mesh& cad,
                     const std::filesystem::path& file);

/// The mesh and deviations of a deviation.ply written by writeComparison.
struct DeviationMesh {
  IndexedMesh mesh;  // mm
  std::vector<float> deviation_mm;
};
[[nodiscard]] DeviationMesh readDeviationPly(const std::filesystem::path& file);

}  // namespace voxelsieve
