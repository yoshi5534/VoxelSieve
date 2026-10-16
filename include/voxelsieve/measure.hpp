#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "voxelsieve/mesh.hpp"
#include "voxelsieve/render.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/transform.hpp"

namespace voxelsieve {

/// Dimensional measurement on a CT scan (ADR 0027): geometric elements (planes, cylinders,
/// spheres, surface points) are fitted to the scanned surface where the CAD model has them, the
/// part is aligned on its datums (3-2-1, RPS) or by best fit, and the characteristics of the
/// drawing (sizes, distances, angles, form, orientation, position, profile) are evaluated against
/// their tolerances. This is what a first article inspection report lists.

enum class ElementType : std::uint8_t { kPlane, kCylinder, kSphere, kPoint };

[[nodiscard]] const char* toString(ElementType type);
/// "plane", "cylinder", "sphere" or "point"; throws otherwise.
[[nodiscard]] ElementType elementTypeFromString(const std::string& name);

/// A geometric element, nominal or fitted, in mm.
struct GeometricElement {
  ElementType type = ElementType::kPlane;
  /// Plane: centroid of its points; cylinder: the point of the axis in the middle of its length;
  /// sphere: centre; point: the point on the surface.
  std::array<double, 3> point{};
  /// Plane and point: unit outward normal of the surface; cylinder: unit axis; sphere: unused.
  std::array<double, 3> direction{0.0, 0.0, 1.0};
  /// Cylinder and sphere.
  double radius = 0.0;
  /// Cylinder: extent of its points along the axis.
  double length = 0.0;
  /// Cylinder and sphere: a hole (the surface faces the axis or centre) rather than a boss.
  bool inner = false;
};

/// A fitted element with the quality of the fit. Fits are least squares (Gauss); points farther
/// than three standard deviations are left out once and the fit repeated.
struct FittedElement {
  GeometricElement element;
  std::size_t points = 0;
  std::size_t outliers = 0;
  double rms_mm = 0.0;
  /// Smallest and largest signed distance of the points to the fitted element (positive
  /// outside); their difference is the form deviation from the least-squares element.
  double min_mm = 0.0;
  double max_mm = 0.0;
  [[nodiscard]] double formMm() const { return max_mm - min_mm; }
};

/// A point of a surface with its unit outward normal.
struct SurfacePoint {
  std::array<double, 3> position{};
  std::array<double, 3> normal{};
};

/// Least-squares fit of an element to surface points, starting at `start` (the nominal element);
/// the type and the side (inner) come from `start`. Throws std::invalid_argument for too few
/// points: 3 for a plane, 6 for a cylinder, 4 for a sphere, 1 for a point.
[[nodiscard]] FittedElement fitElement(std::span<const SurfacePoint> points,
                                       const GeometricElement& start);

/// Signed distance of `p` to the surface of an element, positive outside the material.
[[nodiscard]] double elementDistance(const GeometricElement& element,
                                     const std::array<double, 3>& p);

/// Points of the scanned surface in the voxels `box` of the mask at full resolution: the zero
/// crossings of the distance between neighbouring voxels, with normals from its gradient, in mm
/// (dataset world space). With `stride` > 1 only every stride-th voxel along each axis is
/// visited.
[[nodiscard]] std::vector<SurfacePoint> surfacePoints(const SurfaceMask& mask, const Box& box,
                                                      int stride = 1);

/// Regular geometry of a CAD model, found on its triangles (ADR 0027).
struct CadFeature {
  int id = 0;  // 1, 2, ... by decreasing area
  GeometricElement element;
  double area_mm2 = 0.0;
  /// A point on the feature near its middle; probing there finds the feature again.
  std::array<double, 3> at{};
  /// Indices of its triangles in the mesh.
  std::vector<std::uint32_t> triangles;
  /// RMS distance of its vertices to the element.
  double rms_mm = 0.0;
};

struct FeatureOptions {
  /// Largest distance of a vertex from the element; 0: 2e-4 of the model's diagonal, at least
  /// 1 µm.
  double tolerance_mm = 0.0;
  /// Features smaller than this share of the model's area are not listed.
  double min_area_fraction = 0.001;
  std::size_t max_features = 100;
};

/// Finds and probes the regular geometry of a triangle mesh in its own coordinates.
class FeatureFinder {
 public:
  explicit FeatureFinder(const Mesh& mesh, const FeatureOptions& options = {});
  FeatureFinder(FeatureFinder&&) noexcept;
  FeatureFinder& operator=(FeatureFinder&&) noexcept;
  FeatureFinder(const FeatureFinder&) = delete;
  FeatureFinder& operator=(const FeatureFinder&) = delete;
  ~FeatureFinder();

  /// Planes, cylinders and spheres of the model, largest first.
  [[nodiscard]] std::vector<CadFeature> detect() const;
  /// The feature of the given type at `at`, grown from the triangles nearest to it; nullopt when
  /// no element of that type fits there. A point is the nearest point of the surface with its
  /// normal.
  [[nodiscard]] std::optional<CadFeature> probe(ElementType type,
                                                const std::array<double, 3>& at) const;
  [[nodiscard]] const IndexedMesh& mesh() const;
  [[nodiscard]] double toleranceMm() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] nlohmann::json toJson(const GeometricElement& element);
[[nodiscard]] nlohmann::json toJson(const CadFeature& feature);

/// Renders a mesh with its features in colours and their ids as labels, for a client that has to
/// tell them apart.
[[nodiscard]] RenderImage renderFeatures(const IndexedMesh& mesh,
                                         std::span<const CadFeature> features,
                                         const RenderView& view = {});

/// An inspection plan in the coordinates of the CAD model, which are taken to be those of the
/// drawing. See examples/first_article_plan.json and ADR 0027.
///
/// Elements are named and either probed on the CAD model ("at": a point on the feature) or given
/// explicitly (plane: point, normal, radius; cylinder: point, direction, diameter, length; sphere:
/// center, diameter; point: point, normal). The alignment is "best_fit" (the comparison's) or a
/// list of datums in order of precedence; each datum fixes what its element can fix and what the
/// datums before it left free (a plane: its normal and offset; a cylinder: its axis; a sphere: its
/// centre; a point: its offset along the given directions or its normal). Characteristics have a
/// number (the balloon of the drawing), a type, the elements they relate, the nominal value
/// (default: from the nominal elements) and the tolerance (upper and lower deviation, or
/// "tolerance" for ± or for the width of a tolerance zone).
struct MeasureOptions {
  /// How far from the nominal surface scanned points are taken; 0: five voxels.
  double search_mm = 0.0;
  /// Points closer than this to the edge of an element are left out, where the scan rounds
  /// edges; 0: two voxels.
  double edge_margin_mm = 0.0;
  /// Largest angle between the normals of a scanned point and the nominal surface.
  double max_normal_angle_deg = 30.0;
  /// Points per element at most; denser regions are thinned evenly.
  std::size_t max_points = 200000;
};

/// The measurement of an inspection plan: elements, alignment and characteristics as JSON (see
/// ADR 0027), plus everything needed for the pictures.
struct Measurement {
  nlohmann::json json;
  /// Scan to drawing (CAD) coordinates after the alignment.
  RigidTransform scan_to_cad;
  /// Characteristics in the plan; of them passed, failed, not measurable and without tolerance.
  std::size_t characteristics = 0;
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::size_t not_measured = 0;
  std::size_t unrated = 0;
  /// The triangles of the CAD model that make up each element of json["elements"], in the same
  /// order; empty for elements given explicitly.
  std::vector<std::vector<std::uint32_t>> element_triangles;
  /// Every characteristic was measured and none failed.
  [[nodiscard]] bool allPassed() const {
    return characteristics > 0 && failed == 0 && not_measured == 0;
  }
};

/// Measures `plan` on the scanned surface `mask`, with the CAD model `cad` placed in the scan by
/// `cad_to_scan` (from a nominal-actual comparison). Throws std::invalid_argument for a plan
/// that is not well formed.
[[nodiscard]] Measurement measure(const SurfaceMask& mask, const Mesh& cad,
                                  const RigidTransform& cad_to_scan, const nlohmann::json& plan,
                                  const MeasureOptions& options = {});

/// Writes measurement.json, characteristics.csv and the views measurement_view_[12].png (the CAD
/// model with the measured elements in colour and the characteristic numbers as balloons) into
/// `dir`.
void writeMeasurement(const Measurement& measurement, const Mesh& cad,
                      const std::filesystem::path& dir);

/// The characteristics of a measurement.json as CSV with a header line.
[[nodiscard]] std::string characteristicsCsv(const nlohmann::json& measurement);

}  // namespace voxelsieve
