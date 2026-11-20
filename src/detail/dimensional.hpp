#pragma once

// Internals of the dimensional measurement (dimensional.cpp and dimensional_*.cpp, ADR 0027):
// vector algebra, element geometry, the inspection plan, the extraction of points and the
// alignment on datums.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "voxelsieve/compare.hpp"
#include "voxelsieve/dimensional.hpp"
#include "voxelsieve/render.hpp"

namespace voxelsieve::detail::dimensional {

using Json = nlohmann::json;
using Vec = std::array<double, 3>;

inline Vec operator+(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec operator-(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec operator*(double s, const Vec& a) { return {s * a[0], s * a[1], s * a[2]}; }
inline double dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec cross(const Vec& a, const Vec& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double norm(const Vec& a) { return std::sqrt(dot(a, a)); }
inline Vec normalized(const Vec& a) {
  const double length = norm(a);
  return length > 0.0 ? (1.0 / length) * a : Vec{0.0, 0.0, 1.0};
}
inline Vec toVec(const std::array<float, 3>& p) { return {p[0], p[1], p[2]}; }
inline double degrees(double radians) { return radians * 180.0 / std::numbers::pi; }
inline double radians(double degrees) { return degrees * std::numbers::pi / 180.0; }

/// Two unit vectors perpendicular to `n` and to each other.
std::pair<Vec, Vec> basis(const Vec& n);
/// Eigen decomposition of a symmetric n x n matrix (row-major) by cyclic Jacobi rotations.
/// Returns the eigenvalues in ascending order and the eigenvectors as columns of a row-major
/// matrix in the same order.
std::pair<std::vector<double>, std::vector<double>> symmetricEigen(std::vector<double> a,
                                                                   std::size_t n);
/// Solves the symmetric positive semi-definite system a x = b (n x n, row-major) through its
/// eigen decomposition, ignoring directions with eigenvalues below 1e-12 of the largest.
std::vector<double> solveSymmetric(const std::vector<double>& a, const std::vector<double>& b,
                                   std::size_t n);
/// Unit outward normal of an element's surface at (or nearest to) `p`.
Vec elementNormal(const GeometricElement& e, const Vec& p);
/// Distance of `p` from the axis of a cylinder.
double axisDistance(const GeometricElement& e, const Vec& p);
/// Least-squares fits of one element type, from `start`; fitOnce fits any type.
GeometricElement fitPlane(std::span<const SurfacePoint> points, const GeometricElement& start);
GeometricElement fitSphere(std::span<const SurfacePoint> points, const GeometricElement& start);
GeometricElement fitOnce(std::span<const SurfacePoint> points, const GeometricElement& start);

/// The mesh with its vertices split where triangles of different groups (elements, features)
/// meet, so that each shows its own colour instead of a blend and its edges stay sharp; within a
/// group vertices stay shared and the shading smooth. Group 0 is the rest of the mesh.
IndexedMesh splitByGroup(const IndexedMesh& mesh, std::span<const std::uint32_t> groups,
                         std::span<const std::array<std::uint8_t, 3>> triangle_colors,
                         std::vector<std::array<std::uint8_t, 3>>& vertex_colors);
/// Places a balloon for every anchor (image pixels): away from the middle of the image, clear of
/// the balloons placed before.
std::vector<std::array<double, 2>> placeBalloons(const std::vector<std::array<double, 2>>& anchors,
                                                 int width, int height, double radius);

// ---------------------------------------------------------------------------------------------
// Inspection plan

struct ElementPlan {
  std::string id;
  GeometricElement nominal;
  std::vector<std::uint32_t> triangles;  // CAD region; empty when given explicitly
  Vec anchor{};
  bool from_cad = false;
  double search_mm = 0.0;
  double margin_mm = 0.0;
  /// Point: radius around the nominal point; plane given explicitly: radius of its disc.
  double radius_mm = 0.0;
};

struct ElementResult {
  bool measured = false;
  std::string message;
  FittedElement fit;
  std::vector<Vec> points;  // frame coordinates
  std::vector<double> cad_deviation;
};

struct DatumPlan {
  std::size_t element = 0;
  /// Point datums: the directions it fixes; empty: its nominal normal.
  std::vector<Vec> directions;
};

enum class Kind : std::uint8_t {
  kDiameter,
  kRadius,
  kDistance,
  kAngle,
  kCoordinate,
  kPosition,
  kFlatness,
  kCylindricity,
  kSphericity,
  kForm,
  kParallelism,
  kPerpendicularity,
  kProfile,
  kDeviation,
};

struct KindInfo {
  const char* name;
  Kind kind;
  int elements;  // 1 or 2
  bool zone;     // tolerance zone (nominal 0, only an upper limit)
  const char* label;
};

struct CharacteristicPlan {
  std::string number;
  std::string label;
  const KindInfo* kind = nullptr;
  std::vector<std::size_t> elements;
  std::optional<std::size_t> datum;
  int axis = -1;  // coordinate; distance along an axis
  std::optional<double> nominal;
  std::optional<double> upper;
  std::optional<double> lower;
  Json spec;
};

int axisIndex(const Json& value, const std::string& what);
std::string jsonText(const Json& value);
ElementPlan parseElement(const Json& item, const FeatureFinder& finder);
std::size_t findElement(const std::vector<ElementPlan>& elements, const Json& id,
                        const std::string& what);
CharacteristicPlan parseCharacteristic(const Json& item, std::size_t index,
                                       const std::vector<ElementPlan>& elements);

// ---------------------------------------------------------------------------------------------
// Extraction

struct Extraction {
  const SurfaceMask* mask = nullptr;
  const FeatureFinder* finder = nullptr;
  const MeshDistance* cad = nullptr;
  DimensionalOptions options;
  double voxel_mm = 0.0;
};

ElementResult extractAndFit(const Extraction& x, const ElementPlan& e,
                            const RigidTransform& scan_to_frame);

// ---------------------------------------------------------------------------------------------
// Alignment on datums

/// One linear equation on the small motion (rotation vector w, translation t) of the measured
/// datums about `origin`: row . (w, t) = rhs.
struct Equation {
  std::array<double, 6> row{};
  double rhs = 0.0;
};

std::vector<Equation> datumEquations(const GeometricElement& nominal,
                                     const GeometricElement& actual, const DatumPlan& datum,
                                     const Vec& origin);
/// The small motion that satisfies the equations of the datums in order of precedence: each
/// datum is solved within the freedom the datums before it left; freedom no datum fixes keeps
/// the starting pose. Returns the motion and the number of degrees of freedom fixed.
std::pair<std::array<double, 6>, int> solveDatums(const std::vector<std::vector<Equation>>& datums);
RigidTransform smallMotion(const std::array<double, 6>& x, const Vec& origin);
GeometricElement moved(GeometricElement e, const RigidTransform& t);

}  // namespace voxelsieve::detail::dimensional
