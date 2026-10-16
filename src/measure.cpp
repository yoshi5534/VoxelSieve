#include "voxelsieve/measure.hpp"

#include <tbb/blocked_range.h>
#include <tbb/combinable.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "voxelsieve/compare.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;
using Vec = std::array<double, 3>;

Vec operator+(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec operator-(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec operator*(double s, const Vec& a) { return {s * a[0], s * a[1], s * a[2]}; }
double dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double norm(const Vec& a) { return std::sqrt(dot(a, a)); }
Vec normalized(const Vec& a) {
  const double length = norm(a);
  return length > 0.0 ? (1.0 / length) * a : Vec{0.0, 0.0, 1.0};
}
Vec toVec(const std::array<float, 3>& p) { return {p[0], p[1], p[2]}; }
double degrees(double radians) { return radians * 180.0 / std::numbers::pi; }
double radians(double degrees) { return degrees * std::numbers::pi / 180.0; }

/// Two unit vectors perpendicular to `n` and to each other.
std::pair<Vec, Vec> basis(const Vec& n) {
  const Vec helper = std::abs(n[0]) < 0.6 ? Vec{1.0, 0.0, 0.0} : Vec{0.0, 1.0, 0.0};
  const Vec u = normalized(cross(n, helper));
  return {u, cross(n, u)};
}

/// Eigen decomposition of a symmetric n x n matrix (row-major) by cyclic Jacobi rotations.
/// Returns the eigenvalues in ascending order and the eigenvectors as columns of a row-major
/// matrix in the same order.
std::pair<std::vector<double>, std::vector<double>> symmetricEigen(std::vector<double> a,
                                                                   std::size_t n) {
  std::vector<double> v(n * n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    v[i * n + i] = 1.0;
  }
  for (int sweep = 0; sweep < 100; ++sweep) {
    double off = 0.0;
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      for (std::size_t j = 0; j < n; ++j) {
        total += a[i * n + j] * a[i * n + j];
        if (i != j) {
          off += a[i * n + j] * a[i * n + j];
        }
      }
    }
    if (off <= 1e-30 * total || off == 0.0) {
      break;
    }
    for (std::size_t p = 0; p + 1 < n; ++p) {
      for (std::size_t q = p + 1; q < n; ++q) {
        const double apq = a[p * n + q];
        if (std::abs(apq) < 1e-300) {
          continue;
        }
        const double theta = (a[q * n + q] - a[p * n + p]) / (2.0 * apq);
        const double t =
            (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;
        for (std::size_t k = 0; k < n; ++k) {
          const double akp = a[k * n + p];
          const double akq = a[k * n + q];
          a[k * n + p] = c * akp - s * akq;
          a[k * n + q] = s * akp + c * akq;
        }
        for (std::size_t k = 0; k < n; ++k) {
          const double apk = a[p * n + k];
          const double aqk = a[q * n + k];
          a[p * n + k] = c * apk - s * aqk;
          a[q * n + k] = s * apk + c * aqk;
        }
        for (std::size_t k = 0; k < n; ++k) {
          const double vkp = v[k * n + p];
          const double vkq = v[k * n + q];
          v[k * n + p] = c * vkp - s * vkq;
          v[k * n + q] = s * vkp + c * vkq;
        }
      }
    }
  }
  std::vector<std::size_t> order(n);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::sort(order.begin(), order.end(),
            [&](std::size_t l, std::size_t r) { return a[l * n + l] < a[r * n + r]; });
  std::vector<double> values(n);
  std::vector<double> vectors(n * n);
  for (std::size_t j = 0; j < n; ++j) {
    values[j] = a[order[j] * n + order[j]];
    for (std::size_t i = 0; i < n; ++i) {
      vectors[i * n + j] = v[i * n + order[j]];
    }
  }
  return {values, vectors};
}

/// Solves the symmetric positive semi-definite system a x = b (n x n, row-major) through its
/// eigen decomposition, ignoring directions with eigenvalues below 1e-12 of the largest.
std::vector<double> solveSymmetric(const std::vector<double>& a, const std::vector<double>& b,
                                   std::size_t n) {
  const auto [values, vectors] = symmetricEigen(a, n);
  const double largest = std::max(std::abs(values.back()), 1e-300);
  std::vector<double> x(n, 0.0);
  for (std::size_t j = 0; j < n; ++j) {
    if (values[j] <= 1e-12 * largest) {
      continue;
    }
    double projection = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      projection += vectors[i * n + j] * b[i];
    }
    for (std::size_t i = 0; i < n; ++i) {
      x[i] += vectors[i * n + j] * projection / values[j];
    }
  }
  return x;
}

/// Unit outward normal of an element's surface at (or nearest to) `p`.
Vec elementNormal(const GeometricElement& e, const Vec& p) {
  switch (e.type) {
    case ElementType::kPlane:
    case ElementType::kPoint:
      return e.direction;
    case ElementType::kCylinder: {
      const Vec w = p - e.point;
      const Vec radial = normalized(w - dot(w, e.direction) * e.direction);
      return e.inner ? -1.0 * radial : radial;
    }
    case ElementType::kSphere: {
      const Vec radial = normalized(p - e.point);
      return e.inner ? -1.0 * radial : radial;
    }
  }
  return e.direction;
}

/// Unsigned distance of `p` to the element's surface, ignoring the side.
double surfaceDistance(const GeometricElement& e, const Vec& p) {
  return std::abs(elementDistance(e, p));
}

/// Distance of `p` from the axis of a cylinder.
double axisDistance(const GeometricElement& e, const Vec& p) {
  const Vec w = p - e.point;
  return norm(w - dot(w, e.direction) * e.direction);
}

/// The point of an element's surface nearest to `p`.
Vec projectOnto(const GeometricElement& e, const Vec& p) {
  switch (e.type) {
    case ElementType::kPlane:
      return p - dot(p - e.point, e.direction) * e.direction;
    case ElementType::kPoint:
      return e.point;
    case ElementType::kCylinder: {
      const Vec w = p - e.point;
      const Vec on_axis = e.point + dot(w, e.direction) * e.direction;
      return on_axis + e.radius * normalized(p - on_axis);
    }
    case ElementType::kSphere:
      return e.point + e.radius * normalized(p - e.point);
  }
  return p;
}

/// Squared distance from `p` to the triangle (a, b, c) (Ericson, Real-Time Collision Detection).
double triangleDistance2(const Vec& p, const Vec& a, const Vec& b, const Vec& c) {
  const Vec ab = b - a;
  const Vec ac = c - a;
  const Vec ap = p - a;
  const double d1 = dot(ab, ap);
  const double d2 = dot(ac, ap);
  Vec q;
  if (d1 <= 0.0 && d2 <= 0.0) {
    q = a;
  } else {
    const Vec bp = p - b;
    const double d3 = dot(ab, bp);
    const double d4 = dot(ac, bp);
    const Vec cp = p - c;
    const double d5 = dot(ab, cp);
    const double d6 = dot(ac, cp);
    const double vc = d1 * d4 - d3 * d2;
    const double vb = d5 * d2 - d1 * d6;
    const double va = d3 * d6 - d5 * d4;
    if (d3 >= 0.0 && d4 <= d3) {
      q = b;
    } else if (d6 >= 0.0 && d5 <= d6) {
      q = c;
    } else if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
      q = a + (d1 / (d1 - d3)) * ab;
    } else if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
      q = a + (d2 / (d2 - d6)) * ac;
    } else if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
      q = b + ((d4 - d3) / ((d4 - d3) + (d5 - d6))) * (c - b);
    } else {
      const double denominator = 1.0 / (va + vb + vc);
      q = a + (vb * denominator) * ab + (vc * denominator) * ac;
    }
  }
  const Vec d = p - q;
  return dot(d, d);
}

/// Distinct colours for features and elements.
/// The mesh with its vertices split where triangles of different groups (elements, features)
/// meet, so that each shows its own colour instead of a blend and its edges stay sharp; within a
/// group vertices stay shared and the shading smooth. Group 0 is the rest of the mesh.
IndexedMesh splitByGroup(const IndexedMesh& mesh, std::span<const std::uint32_t> groups,
                         std::span<const std::array<std::uint8_t, 3>> triangle_colors,
                         std::vector<std::array<std::uint8_t, 3>>& vertex_colors) {
  IndexedMesh split;
  split.triangles.reserve(mesh.triangles.size());
  vertex_colors.clear();
  std::unordered_map<std::uint64_t, std::uint32_t> index;
  for (std::size_t t = 0; t < mesh.triangles.size(); ++t) {
    const auto& c = triangle_colors[t];
    std::array<std::uint32_t, 3> triangle{};
    for (std::size_t k = 0; k < 3; ++k) {
      const std::uint32_t v = mesh.triangles[t][k];
      const auto [found, added] = index.try_emplace(
          (std::uint64_t{v} << 32U) | groups[t], static_cast<std::uint32_t>(split.points.size()));
      if (added) {
        split.points.push_back(mesh.points[v]);
        vertex_colors.push_back(c);
      }
      triangle[k] = found->second;
    }
    split.triangles.push_back(triangle);
  }
  return split;
}

std::array<std::uint8_t, 3> paletteColor(std::size_t i) {
  static constexpr std::array<std::array<std::uint8_t, 3>, 12> kPalette{{{31, 119, 180},
                                                                         {255, 127, 14},
                                                                         {44, 160, 44},
                                                                         {214, 39, 40},
                                                                         {148, 103, 189},
                                                                         {140, 86, 75},
                                                                         {227, 119, 194},
                                                                         {188, 189, 34},
                                                                         {23, 190, 207},
                                                                         {174, 199, 232},
                                                                         {255, 187, 120},
                                                                         {152, 223, 138}}};
  return kPalette[i % kPalette.size()];
}

/// Places a balloon for every anchor (image pixels): away from the middle of the image, clear of
/// the balloons placed before.
std::vector<std::array<double, 2>> placeBalloons(const std::vector<std::array<double, 2>>& anchors,
                                                 int width, int height, double radius) {
  std::vector<std::array<double, 2>> placed;
  const double cx = 0.5 * width;
  const double cy = 0.5 * height;
  for (const auto& a : anchors) {
    double dx = a[0] - cx;
    double dy = a[1] - cy;
    const double length = std::hypot(dx, dy);
    if (length < 1e-6) {
      dx = 1.0;
      dy = -1.0;
    }
    const double scale = 1.0 / std::hypot(dx, dy);
    dx *= scale;
    dy *= scale;
    std::array<double, 2> best{a[0] + 3.0 * radius * dx, a[1] + 3.0 * radius * dy};
    for (int step = 0; step < 40; ++step) {
      const double reach = (3.0 + 1.2 * step) * radius;
      // Alternate a little to both sides while moving out.
      const double turn = (step % 3 == 0 ? 0.0 : (step % 3 == 1 ? 0.35 : -0.35));
      const double ux = dx * std::cos(turn) - dy * std::sin(turn);
      const double uy = dx * std::sin(turn) + dy * std::cos(turn);
      const std::array<double, 2> candidate{
          std::clamp(a[0] + reach * ux, radius + 2.0, width - radius - 2.0),
          std::clamp(a[1] + reach * uy, radius + 2.0, height - radius - 2.0)};
      const bool clear = std::all_of(placed.begin(), placed.end(), [&](const auto& other) {
        return std::hypot(other[0] - candidate[0], other[1] - candidate[1]) > 2.3 * radius;
      });
      best = candidate;
      if (clear) {
        break;
      }
    }
    placed.push_back(best);
  }
  return placed;
}

Vec centroid(std::span<const SurfacePoint> points) {
  Vec sum{};
  for (const SurfacePoint& p : points) {
    sum = sum + p.position;
  }
  return (1.0 / static_cast<double>(std::max<std::size_t>(points.size(), 1))) * sum;
}

GeometricElement fitPlane(std::span<const SurfacePoint> points, const GeometricElement& start) {
  const Vec c = centroid(points);
  std::vector<double> cov(9, 0.0);
  for (const SurfacePoint& p : points) {
    const Vec d = p.position - c;
    for (std::size_t i = 0; i < 3; ++i) {
      for (std::size_t j = 0; j < 3; ++j) {
        cov[i * 3 + j] += d[i] * d[j];
      }
    }
  }
  const auto [values, vectors] = symmetricEigen(cov, 3);
  Vec n{vectors[0], vectors[3], vectors[6]};
  if (dot(n, start.direction) < 0.0) {
    n = -1.0 * n;
  }
  GeometricElement plane = start;
  plane.point = c;
  plane.direction = normalized(n);
  return plane;
}

GeometricElement fitSphere(std::span<const SurfacePoint> points, const GeometricElement& start) {
  GeometricElement s = start;
  if (!(s.radius > 0.0)) {
    // Algebraic start: |p|^2 = 2 c.p + k.
    std::vector<double> a(16, 0.0);
    std::vector<double> b(4, 0.0);
    for (const SurfacePoint& p : points) {
      const std::array<double, 4> row{2.0 * p.position[0], 2.0 * p.position[1], 2.0 * p.position[2],
                                      1.0};
      const double rhs = dot(p.position, p.position);
      for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
          a[i * 4 + j] += row[i] * row[j];
        }
        b[i] += row[i] * rhs;
      }
    }
    const auto x = solveSymmetric(a, b, 4);
    s.point = {x[0], x[1], x[2]};
    s.radius = std::sqrt(std::max(x[3] + dot(s.point, s.point), 1e-12));
  }
  for (int iteration = 0; iteration < 50; ++iteration) {
    std::vector<double> a(16, 0.0);
    std::vector<double> b(4, 0.0);
    for (const SurfacePoint& p : points) {
      const Vec d = p.position - s.point;
      const double length = norm(d);
      if (!(length > 0.0)) {
        continue;
      }
      const Vec u = (1.0 / length) * d;
      // r = |p - c| - radius; dr/dc = -u, dr/dradius = -1.
      const std::array<double, 4> j{-u[0], -u[1], -u[2], -1.0};
      const double r = length - s.radius;
      for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t k = 0; k < 4; ++k) {
          a[i * 4 + k] += j[i] * j[k];
        }
        b[i] -= j[i] * r;
      }
    }
    const auto x = solveSymmetric(a, b, 4);
    s.point = s.point + Vec{x[0], x[1], x[2]};
    s.radius += x[3];
    if (std::abs(x[0]) + std::abs(x[1]) + std::abs(x[2]) + std::abs(x[3]) < 1e-10 * s.radius) {
      break;
    }
  }
  s.radius = std::abs(s.radius);
  return s;
}

GeometricElement fitCylinder(std::span<const SurfacePoint> points, const GeometricElement& start) {
  GeometricElement cyl = start;
  cyl.direction = normalized(cyl.direction);
  for (int iteration = 0; iteration < 60; ++iteration) {
    // Keep the axis point at the centroid's height, for a well conditioned system.
    const Vec c = centroid(points);
    cyl.point = cyl.point + dot(c - cyl.point, cyl.direction) * cyl.direction;
    const auto [u, v] = basis(cyl.direction);
    std::vector<double> a(25, 0.0);
    std::vector<double> b(5, 0.0);
    for (const SurfacePoint& p : points) {
      const Vec w = p.position - cyl.point;
      const double along = dot(w, cyl.direction);
      const Vec q = w - along * cyl.direction;
      const double d = norm(q);
      if (!(d > 0.0)) {
        continue;
      }
      const Vec rho = (1.0 / d) * q;
      const std::array<double, 5> j{-dot(rho, u), -dot(rho, v), -along * dot(rho, u),
                                    -along * dot(rho, v), -1.0};
      const double r = d - cyl.radius;
      for (std::size_t i = 0; i < 5; ++i) {
        for (std::size_t k = 0; k < 5; ++k) {
          a[i * 5 + k] += j[i] * j[k];
        }
        b[i] -= j[i] * r;
      }
    }
    const auto x = solveSymmetric(a, b, 5);
    cyl.point = cyl.point + x[0] * u + x[1] * v;
    cyl.direction = normalized(cyl.direction + x[2] * u + x[3] * v);
    cyl.radius += x[4];
    const double scale = std::max(cyl.radius, 1e-9);
    if (std::abs(x[0]) + std::abs(x[1]) + std::abs(x[4]) < 1e-10 * scale &&
        std::abs(x[2]) + std::abs(x[3]) < 1e-12) {
      break;
    }
  }
  if (dot(cyl.direction, start.direction) < 0.0) {
    cyl.direction = -1.0 * cyl.direction;
  }
  cyl.radius = std::abs(cyl.radius);
  double lo = std::numeric_limits<double>::max();
  double hi = std::numeric_limits<double>::lowest();
  for (const SurfacePoint& p : points) {
    const double along = dot(p.position - cyl.point, cyl.direction);
    lo = std::min(lo, along);
    hi = std::max(hi, along);
  }
  if (lo <= hi) {
    cyl.point = cyl.point + (0.5 * (lo + hi)) * cyl.direction;
    cyl.length = hi - lo;
  }
  return cyl;
}

/// A surface point: the nominal point moved along its normal by the median offset of the points.
GeometricElement fitPoint(std::span<const SurfacePoint> points, const GeometricElement& start) {
  std::vector<double> offsets;
  offsets.reserve(points.size());
  for (const SurfacePoint& p : points) {
    offsets.push_back(dot(p.position - start.point, start.direction));
  }
  const auto middle = offsets.begin() + static_cast<std::ptrdiff_t>(offsets.size() / 2);
  std::nth_element(offsets.begin(), middle, offsets.end());
  GeometricElement point = start;
  point.point = start.point + *middle * start.direction;
  return point;
}

GeometricElement fitOnce(std::span<const SurfacePoint> points, const GeometricElement& start) {
  switch (start.type) {
    case ElementType::kPlane:
      return fitPlane(points, start);
    case ElementType::kCylinder:
      return fitCylinder(points, start);
    case ElementType::kSphere:
      return fitSphere(points, start);
    case ElementType::kPoint:
      return fitPoint(points, start);
  }
  return start;
}

std::size_t minimumPoints(ElementType type) {
  switch (type) {
    case ElementType::kPlane:
      return 3;
    case ElementType::kCylinder:
      return 6;
    case ElementType::kSphere:
      return 4;
    case ElementType::kPoint:
      return 1;
  }
  return 3;
}

}  // namespace

const char* toString(ElementType type) {
  switch (type) {
    case ElementType::kPlane:
      return "plane";
    case ElementType::kCylinder:
      return "cylinder";
    case ElementType::kSphere:
      return "sphere";
    case ElementType::kPoint:
      return "point";
  }
  return "plane";
}

ElementType elementTypeFromString(const std::string& name) {
  if (name == "plane") {
    return ElementType::kPlane;
  }
  if (name == "cylinder") {
    return ElementType::kCylinder;
  }
  if (name == "sphere") {
    return ElementType::kSphere;
  }
  if (name == "point") {
    return ElementType::kPoint;
  }
  throw std::invalid_argument("Unknown element type '" + name +
                              "' (plane, cylinder, sphere or point)");
}

double elementDistance(const GeometricElement& e, const std::array<double, 3>& p) {
  switch (e.type) {
    case ElementType::kPlane:
    case ElementType::kPoint:
      return dot(p - e.point, e.direction);
    case ElementType::kCylinder: {
      const double d = axisDistance(e, p) - e.radius;
      return e.inner ? -d : d;
    }
    case ElementType::kSphere: {
      const double d = norm(p - e.point) - e.radius;
      return e.inner ? -d : d;
    }
  }
  return 0.0;
}

FittedElement fitElement(std::span<const SurfacePoint> points, const GeometricElement& start) {
  if (points.size() < minimumPoints(start.type)) {
    throw std::invalid_argument(std::string("Too few points for a ") + toString(start.type) + " (" +
                                std::to_string(points.size()) + ")");
  }
  FittedElement fitted;
  fitted.element = fitOnce(points, start);
  std::vector<SurfacePoint> kept(points.begin(), points.end());
  // Leave out points beyond three standard deviations, once more after the refit.
  for (int round = 0; round < 2 && start.type != ElementType::kPoint; ++round) {
    double sum2 = 0.0;
    for (const SurfacePoint& p : kept) {
      const double r = elementDistance(fitted.element, p.position);
      sum2 += r * r;
    }
    const double limit = 3.0 * std::sqrt(sum2 / static_cast<double>(kept.size()));
    std::vector<SurfacePoint> inliers;
    inliers.reserve(kept.size());
    for (const SurfacePoint& p : kept) {
      if (std::abs(elementDistance(fitted.element, p.position)) <= limit) {
        inliers.push_back(p);
      }
    }
    if (inliers.size() == kept.size() || inliers.size() < minimumPoints(start.type)) {
      break;
    }
    kept = std::move(inliers);
    fitted.element = fitOnce(kept, fitted.element);
  }
  fitted.points = kept.size();
  fitted.outliers = points.size() - kept.size();
  double sum2 = 0.0;
  fitted.min_mm = std::numeric_limits<double>::max();
  fitted.max_mm = std::numeric_limits<double>::lowest();
  for (const SurfacePoint& p : kept) {
    const double r = start.type == ElementType::kPoint
                         ? dot(p.position - fitted.element.point, fitted.element.direction)
                         : elementDistance(fitted.element, p.position);
    sum2 += r * r;
    fitted.min_mm = std::min(fitted.min_mm, r);
    fitted.max_mm = std::max(fitted.max_mm, r);
  }
  fitted.rms_mm = std::sqrt(sum2 / static_cast<double>(kept.size()));
  return fitted;
}

std::vector<SurfacePoint> surfacePoints(const SurfaceMask& mask, const Box& box, int stride) {
  const SurfaceInfo& info = mask.info();
  const VoxelSize& v = info.voxel_size;
  Box clamped = box;
  for (std::size_t a = 0; a < 3; ++a) {
    clamped.min[a] = std::clamp<std::int64_t>(clamped.min[a], 0, info.dims[a]);
    clamped.max[a] = std::clamp<std::int64_t>(clamped.max[a], clamped.min[a], info.dims[a]);
  }
  std::vector<SurfacePoint> points;
  if (clamped.voxelCount() == 0) {
    return points;
  }
  const std::int64_t step = std::max(stride, 1);
  // Slabs along z with a margin of one voxel below and two above for the differences.
  constexpr std::int64_t kSlab = 32;
  for (std::int64_t z0 = clamped.min[2]; z0 < clamped.max[2]; z0 += kSlab) {
    const std::int64_t z1 = std::min(z0 + kSlab, clamped.max[2]);
    Box read = clamped;
    read.min[2] = z0;
    read.max[2] = z1;
    for (std::size_t a = 0; a < 3; ++a) {
      read.min[a] = std::max<std::int64_t>(read.min[a] - 1, 0);
      read.max[a] = std::min<std::int64_t>(read.max[a] + 2, info.dims[a]);
    }
    std::vector<std::uint8_t> codes(static_cast<std::size_t>(read.voxelCount()));
    mask.readCodes(read, codes);
    std::array<float, 256> decoded{};
    for (int c = 0; c <= info.maxCode(); ++c) {
      decoded[static_cast<std::size_t>(c)] =
          decodeSurfaceDistance(static_cast<std::uint8_t>(c), info.bits, info.band_voxels);
    }
    const std::int64_t nx = read.size(0);
    const std::int64_t ny = read.size(1);
    const auto at = [&](std::int64_t x, std::int64_t y, std::int64_t z) -> double {
      x = std::clamp<std::int64_t>(x, read.min[0], read.max[0] - 1) - read.min[0];
      y = std::clamp<std::int64_t>(y, read.min[1], read.max[1] - 1) - read.min[1];
      z = std::clamp<std::int64_t>(z, read.min[2], read.max[2] - 1) - read.min[2];
      return decoded[codes[static_cast<std::size_t>((z * ny + y) * nx + x)]];
    };
    const auto gradient = [&](std::int64_t x, std::int64_t y, std::int64_t z) -> Vec {
      return {0.5 * (at(x + 1, y, z) - at(x - 1, y, z)) / v[0],
              0.5 * (at(x, y + 1, z) - at(x, y - 1, z)) / v[1],
              0.5 * (at(x, y, z + 1) - at(x, y, z - 1)) / v[2]};
    };
    for (std::int64_t z = z0; z < z1; z += step) {
      for (std::int64_t y = clamped.min[1]; y < clamped.max[1]; y += step) {
        for (std::int64_t x = clamped.min[0]; x < clamped.max[0]; x += step) {
          const double d0 = at(x, y, z);
          for (std::size_t axis = 0; axis < 3; ++axis) {
            std::array<std::int64_t, 3> next{x, y, z};
            ++next[axis];
            if (next[axis] >= info.dims[axis]) {
              continue;
            }
            const double d1 = at(next[0], next[1], next[2]);
            if ((d0 < 0.0) == (d1 < 0.0)) {
              continue;
            }
            const double t = d0 / (d0 - d1);
            Vec position{static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)};
            position[axis] += t;
            for (std::size_t k = 0; k < 3; ++k) {
              position[k] *= v[k];
            }
            const Vec g = (1.0 - t) * gradient(x, y, z) + t * gradient(next[0], next[1], next[2]);
            points.push_back({position, normalized(g)});
          }
        }
      }
    }
  }
  return points;
}

nlohmann::json toJson(const GeometricElement& element) {
  Json json = {{"type", toString(element.type)}};
  switch (element.type) {
    case ElementType::kPlane:
      json["point"] = element.point;
      json["normal"] = element.direction;
      break;
    case ElementType::kCylinder:
      json["point"] = element.point;
      json["direction"] = element.direction;
      json["diameter"] = 2.0 * element.radius;
      json["length"] = element.length;
      json["inner"] = element.inner;
      break;
    case ElementType::kSphere:
      json["center"] = element.point;
      json["diameter"] = 2.0 * element.radius;
      json["inner"] = element.inner;
      break;
    case ElementType::kPoint:
      json["point"] = element.point;
      json["normal"] = element.direction;
      break;
  }
  return json;
}

nlohmann::json toJson(const CadFeature& feature) {
  Json json = toJson(feature.element);
  json["id"] = feature.id;
  json["area_mm2"] = feature.area_mm2;
  json["at"] = feature.at;
  json["triangles"] = feature.triangles.size();
  json["rms_mm"] = feature.rms_mm;
  return json;
}

struct FeatureFinder::Impl {
  IndexedMesh mesh;
  std::vector<Vec> normals;
  std::vector<double> areas;
  std::vector<Vec> centroids;
  /// Neighbour across each edge (v0-v1, v1-v2, v2-v0), or -1.
  std::vector<std::array<std::int64_t, 3>> neighbors;
  std::unique_ptr<MeshDistance> distance;
  FeatureOptions options;
  double tolerance = 0.0;
  double diagonal = 0.0;
  double total_area = 0.0;
  double median_edge = 0.0;
  /// Marks of the current search over triangles and vertices: equal to `epoch` when visited.
  mutable std::vector<std::uint32_t> triangle_mark;
  mutable std::vector<std::uint32_t> vertex_mark;
  mutable std::uint32_t epoch = 0;

  /// Starts a new search: every mark becomes stale.
  std::uint32_t nextEpoch() const {
    if (++epoch == 0) {
      std::fill(triangle_mark.begin(), triangle_mark.end(), 0U);
      std::fill(vertex_mark.begin(), vertex_mark.end(), 0U);
      epoch = 1;
    }
    return epoch;
  }

  [[nodiscard]] Vec vertex(std::uint32_t t, std::size_t k) const {
    return toVec(mesh.points[mesh.triangles[t][k]]);
  }

  [[nodiscard]] bool accepts(const GeometricElement& model, std::uint32_t t, double tol,
                             double cos_angle) const {
    for (std::size_t k = 0; k < 3; ++k) {
      if (surfaceDistance(model, vertex(t, k)) > tol) {
        return false;
      }
    }
    return dot(normals[t], elementNormal(model, centroids[t])) >= cos_angle;
  }

  /// Triangles connected to `seed` that fit `model`, leaving out those in `taken`.
  [[nodiscard]] std::vector<std::uint32_t> grow(std::uint32_t seed, const GeometricElement& model,
                                                double tol, double cos_angle,
                                                const std::vector<char>* taken) const {
    std::vector<std::uint32_t> region;
    if ((taken != nullptr && (*taken)[seed] != 0) || !accepts(model, seed, tol, cos_angle)) {
      return region;
    }
    const std::uint32_t mark = nextEpoch();
    triangle_mark[seed] = mark;
    region.push_back(seed);
    for (std::size_t next = 0; next < region.size(); ++next) {
      const std::uint32_t t = region[next];
      for (const std::int64_t n : neighbors[t]) {
        if (n < 0) {
          continue;
        }
        const auto u = static_cast<std::uint32_t>(n);
        if (triangle_mark[u] == mark || (taken != nullptr && (*taken)[u] != 0)) {
          continue;
        }
        triangle_mark[u] = mark;
        if (accepts(model, u, tol, cos_angle)) {
          region.push_back(u);
        }
      }
    }
    return region;
  }

  [[nodiscard]] std::vector<SurfacePoint> vertices(const std::vector<std::uint32_t>& region) const {
    const std::uint32_t mark = nextEpoch();
    std::vector<SurfacePoint> points;
    for (const std::uint32_t t : region) {
      for (const std::uint32_t i : mesh.triangles[t]) {
        if (vertex_mark[i] != mark) {
          vertex_mark[i] = mark;
          points.push_back({toVec(mesh.points[i]), normals[t]});
        }
      }
    }
    return points;
  }

  [[nodiscard]] double area(const std::vector<std::uint32_t>& region) const {
    double sum = 0.0;
    for (const std::uint32_t t : region) {
      sum += areas[t];
    }
    return sum;
  }

  /// Triangles around `seed` across smooth edges, until their normals turn by 12 degrees.
  [[nodiscard]] std::vector<std::uint32_t> neighborhood(std::uint32_t seed) const {
    const double smooth = std::cos(radians(30.0));
    const double enough = std::cos(radians(12.0));
    const std::uint32_t mark = nextEpoch();
    std::vector<std::uint32_t> found{seed};
    triangle_mark[seed] = mark;
    std::size_t next = 0;
    bool curved = false;
    while (next < found.size() && found.size() < 5000 && !(curved && found.size() >= 12)) {
      const std::uint32_t t = found[next++];
      for (const std::int64_t n : neighbors[t]) {
        if (n < 0) {
          continue;
        }
        const auto u = static_cast<std::uint32_t>(n);
        if (triangle_mark[u] == mark || dot(normals[t], normals[u]) < smooth) {
          continue;
        }
        triangle_mark[u] = mark;
        found.push_back(u);
        curved = curved || dot(normals[u], normals[seed]) < enough;
      }
    }
    return found;
  }

  [[nodiscard]] std::vector<std::uint32_t> growPlane(std::uint32_t seed,
                                                     const std::vector<char>* taken,
                                                     GeometricElement& plane) const {
    plane = {};
    plane.type = ElementType::kPlane;
    plane.point = centroids[seed];
    plane.direction = normals[seed];
    const double cos_angle = std::cos(radians(1.5));
    std::vector<std::uint32_t> region;
    for (int round = 0; round < 2; ++round) {
      region = grow(seed, plane, tolerance, cos_angle, taken);
      if (region.empty()) {
        return region;
      }
      plane = fitPlane(vertices(region), plane);
    }
    return grow(seed, plane, tolerance, cos_angle, taken);
  }

  /// A cylinder or sphere through the triangles around `seed`, grown over the triangles that fit
  /// it; empty when the surface there is flat or the element does not fit.
  [[nodiscard]] std::vector<std::uint32_t> growCurved(
      std::uint32_t seed, ElementType type, const std::vector<char>* taken,
      GeometricElement& element, std::vector<std::uint32_t>* tried = nullptr) const {
    const std::vector<std::uint32_t> around = neighborhood(seed);
    std::vector<double> spread(9, 0.0);
    for (const std::uint32_t t : around) {
      for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
          spread[i * 3 + j] += areas[t] * normals[t][i] * normals[t][j];
        }
      }
    }
    const auto [values, vectors] = symmetricEigen(spread, 3);
    if (values[1] < 1e-3 * values[2]) {
      return {};  // flat
    }
    const std::vector<SurfacePoint> points = vertices(around);
    element = {};
    element.type = type;
    if (type == ElementType::kCylinder) {
      const Vec axis = normalized({vectors[0], vectors[3], vectors[6]});
      const auto [u, v] = basis(axis);
      const Vec origin = centroids[seed];
      // Circle through the points projected along the axis (Kasa).
      std::vector<double> a(9, 0.0);
      std::vector<double> b(3, 0.0);
      for (const SurfacePoint& p : points) {
        const Vec d = p.position - origin;
        const std::array<double, 3> row{dot(d, u), dot(d, v), 1.0};
        const double rhs = -(row[0] * row[0] + row[1] * row[1]);
        for (std::size_t i = 0; i < 3; ++i) {
          for (std::size_t j = 0; j < 3; ++j) {
            a[i * 3 + j] += row[i] * row[j];
          }
          b[i] += row[i] * rhs;
        }
      }
      const auto x = solveSymmetric(a, b, 3);
      const double cx = -0.5 * x[0];
      const double cy = -0.5 * x[1];
      const double r2 = cx * cx + cy * cy - x[2];
      if (!(r2 > 0.0) || !std::isfinite(r2)) {
        return {};
      }
      element.point = origin + cx * u + cy * v;
      element.direction = axis;
      element.radius = std::sqrt(r2);
    } else {
      if (values[0] < 0.2 * values[1]) {
        return {};  // curved in one direction only
      }
      element = fitSphere(points, element);
    }
    if (!(element.radius > 0.0) || element.radius > 10.0 * diagonal) {
      return {};
    }
    const Vec radial = elementNormal(element, centroids[seed]);
    element.inner = dot(normals[seed], radial) < 0.0;
    const double cos_angle = std::cos(radians(20.0));
    const std::array<double, 2> loose{std::max(10.0 * tolerance, 0.05 * element.radius),
                                      3.0 * tolerance};
    for (const double tol : loose) {
      const std::vector<std::uint32_t> region = grow(seed, element, tol, cos_angle, taken);
      if (tried != nullptr && region.size() > tried->size()) {
        *tried = region;
      }
      if (region.size() < 3) {
        return {};
      }
      const bool inner = element.inner;
      element = fitOnce(vertices(region), element);
      element.inner = inner;
      if (!(element.radius > 0.0) || element.radius > 10.0 * diagonal) {
        return {};
      }
    }
    // Refit until the region stops growing: an estimate from part of the element covers only
    // part of it within the tolerance.
    std::vector<std::uint32_t> region;
    for (int round = 0; round < 8; ++round) {
      std::vector<std::uint32_t> grown = grow(seed, element, tolerance, cos_angle, taken);
      if (grown.size() < 3) {
        return {};
      }
      const bool larger = grown.size() > region.size();
      region = std::move(grown);
      if (!larger) {
        break;
      }
      const bool inner = element.inner;
      element = fitOnce(vertices(region), element);
      element.inner = inner;
    }
    // The normals must turn by at least 15 degrees, or a slightly curved patch would pass; on a
    // sphere in both directions, or a band of a cylinder or torus would pass.
    double turn = 1.0;
    std::vector<double> scatter(9, 0.0);
    for (const std::uint32_t t : region) {
      turn = std::min(turn, dot(normals[t], normals[seed]));
      for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
          scatter[i * 3 + j] += areas[t] * normals[t][i] * normals[t][j];
        }
      }
    }
    if (turn > std::cos(radians(15.0))) {
      return {};
    }
    if (type == ElementType::kSphere) {
      const std::vector<double> spread_values = symmetricEigen(scatter, 3).first;
      if (spread_values[0] < 0.05 * spread_values[1]) {
        return {};
      }
    }
    const bool inner = element.inner;
    element = fitOnce(vertices(region), element);
    element.inner = inner;
    return region;
  }

  [[nodiscard]] CadFeature feature(const GeometricElement& element,
                                   std::vector<std::uint32_t> region) const {
    CadFeature f;
    f.element = element;
    f.area_mm2 = area(region);
    Vec middle{};
    for (const std::uint32_t t : region) {
      middle = middle + areas[t] * centroids[t];
    }
    middle = (1.0 / std::max(f.area_mm2, 1e-300)) * middle;
    std::uint32_t nearest = region.front();
    double best = std::numeric_limits<double>::max();
    for (const std::uint32_t t : region) {
      const Vec d = centroids[t] - middle;
      if (dot(d, d) < best) {
        best = dot(d, d);
        nearest = t;
      }
    }
    f.at = projectOnto(element, centroids[nearest]);
    double sum2 = 0.0;
    const std::vector<SurfacePoint> points = vertices(region);
    for (const SurfacePoint& p : points) {
      const double d = elementDistance(element, p.position);
      sum2 += d * d;
    }
    f.rms_mm = std::sqrt(sum2 / static_cast<double>(std::max<std::size_t>(points.size(), 1)));
    std::sort(region.begin(), region.end());
    f.triangles = std::move(region);
    if (element.type == ElementType::kPlane) {
      // The middle of the face rather than the mean of the vertices.
      f.element.point = projectOnto(element, middle);
    }
    return f;
  }
};

FeatureFinder::FeatureFinder(const Mesh& mesh, const FeatureOptions& options)
    : impl_(std::make_unique<Impl>()) {
  Impl& m = *impl_;
  m.options = options;
  m.mesh = indexedMesh(mesh);
  if (m.mesh.triangles.empty()) {
    throw std::invalid_argument("The CAD mesh has no triangles");
  }
  const double orientation = meshVolumeMm3(mesh) < 0.0 ? -1.0 : 1.0;
  const std::size_t count = m.mesh.triangles.size();
  m.normals.resize(count);
  m.areas.resize(count);
  m.centroids.resize(count);
  Vec lo{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
         std::numeric_limits<double>::max()};
  Vec hi = -1.0 * lo;
  std::vector<double> edges;
  edges.reserve(count);
  for (std::size_t t = 0; t < count; ++t) {
    const auto i = static_cast<std::uint32_t>(t);
    const Vec a = m.vertex(i, 0);
    const Vec b = m.vertex(i, 1);
    const Vec c = m.vertex(i, 2);
    const Vec n = cross(b - a, c - a);
    m.areas[t] = 0.5 * norm(n);
    m.normals[t] = orientation * normalized(n);
    m.centroids[t] = (1.0 / 3.0) * (a + b + c);
    m.total_area += m.areas[t];
    edges.push_back(norm(b - a));
    for (const Vec& p : {a, b, c}) {
      for (std::size_t k = 0; k < 3; ++k) {
        lo[k] = std::min(lo[k], p[k]);
        hi[k] = std::max(hi[k], p[k]);
      }
    }
  }
  m.diagonal = norm(hi - lo);
  const auto middle = edges.begin() + static_cast<std::ptrdiff_t>(edges.size() / 2);
  std::nth_element(edges.begin(), middle, edges.end());
  m.median_edge = *middle;
  m.tolerance =
      options.tolerance_mm > 0.0 ? options.tolerance_mm : std::max(2e-4 * m.diagonal, 1e-3);
  // Edges shared by exactly two triangles connect them.
  std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint32_t, std::uint8_t>>> owners;
  owners.reserve(3 * count);
  for (std::size_t t = 0; t < count; ++t) {
    const auto& tri = m.mesh.triangles[t];
    for (std::uint8_t k = 0; k < 3; ++k) {
      const std::uint32_t a = tri[k];
      const std::uint32_t b = tri[(k + 1U) % 3U];
      const std::uint64_t key =
          (static_cast<std::uint64_t>(std::min(a, b)) << 32U) | std::max(a, b);
      owners[key].emplace_back(static_cast<std::uint32_t>(t), k);
    }
  }
  m.triangle_mark.assign(count, 0U);
  m.vertex_mark.assign(m.mesh.points.size(), 0U);
  m.neighbors.assign(count, {-1, -1, -1});
  for (const auto& [key, sides] : owners) {
    if (sides.size() == 2) {
      m.neighbors[sides[0].first][sides[0].second] = sides[1].first;
      m.neighbors[sides[1].first][sides[1].second] = sides[0].first;
    }
  }
  m.distance = std::make_unique<MeshDistance>(mesh);
}

FeatureFinder::FeatureFinder(FeatureFinder&&) noexcept = default;
FeatureFinder& FeatureFinder::operator=(FeatureFinder&&) noexcept = default;
FeatureFinder::~FeatureFinder() = default;

const IndexedMesh& FeatureFinder::mesh() const { return impl_->mesh; }
double FeatureFinder::toleranceMm() const { return impl_->tolerance; }

std::vector<CadFeature> FeatureFinder::detect() const {
  const Impl& m = *impl_;
  const std::size_t count = m.mesh.triangles.size();
  std::vector<std::uint32_t> order(count);
  std::iota(order.begin(), order.end(), 0U);
  std::stable_sort(order.begin(), order.end(),
                   [&](std::uint32_t l, std::uint32_t r) { return m.areas[l] > m.areas[r]; });
  std::vector<char> taken(count, 0);
  std::vector<CadFeature> features;
  const double min_area = m.options.min_area_fraction * m.total_area;
  // Large features first, so that small ones grown from an unlucky seed (a fillet taken for a
  // cylinder, a strip of a coarse cylinder taken for a plane) cannot cut them into pieces.
  const std::array<double, 2> stages{std::max(0.02 * m.total_area, min_area), min_area};
  for (const double threshold : stages) {
    // Seeds inside a region found too small in this stage would find it again.
    std::vector<char> tried(count, 0);
    for (const std::uint32_t seed : order) {
      if (taken[seed] != 0 || tried[seed] != 0) {
        continue;
      }
      GeometricElement best_element;
      std::vector<std::uint32_t> best = m.growPlane(seed, &taken, best_element);
      std::vector<std::uint32_t> attempted;
      for (const ElementType type : {ElementType::kCylinder, ElementType::kSphere}) {
        GeometricElement element;
        std::vector<std::uint32_t> region = m.growCurved(seed, type, &taken, element, &attempted);
        // A curved element must explain clearly more than a plane, so planes divided into
        // strips by a coarse model stay planes.
        if (!region.empty() && m.area(region) > 1.05 * m.area(best)) {
          best = std::move(region);
          best_element = element;
        }
      }
      tried[seed] = 1;
      if (best.empty() || m.area(best) < threshold) {
        // Seeds in what this one grew would grow the same.
        for (const std::uint32_t t : best) {
          tried[t] = 1;
        }
        for (const std::uint32_t t : attempted) {
          tried[t] = 1;
        }
        continue;
      }
      for (const std::uint32_t t : best) {
        taken[t] = 1;
      }
      features.push_back(m.feature(best_element, std::move(best)));
    }
  }
  std::stable_sort(features.begin(), features.end(), [](const CadFeature& l, const CadFeature& r) {
    return l.area_mm2 > r.area_mm2;
  });
  if (features.size() > m.options.max_features) {
    features.resize(m.options.max_features);
  }
  for (std::size_t i = 0; i < features.size(); ++i) {
    features[i].id = static_cast<int>(i + 1);
  }
  return features;
}

std::optional<CadFeature> FeatureFinder::probe(ElementType type,
                                               const std::array<double, 3>& at) const {
  const Impl& m = *impl_;
  const MeshDistance::Hit hit = m.distance->query(at);
  if (type == ElementType::kPoint) {
    CadFeature f;
    f.element.type = ElementType::kPoint;
    f.element.point = hit.closest;
    f.element.direction = normalized(hit.normal);
    f.at = hit.closest;
    return f;
  }
  // Seeds: the triangles within reach of `at`, nearest first.
  const double reach = std::abs(hit.distance) + std::max(2.0 * m.median_edge, 1e-3 * m.diagonal);
  std::vector<std::pair<double, std::uint32_t>> seeds;
  {
    std::set<std::uint32_t> seen{hit.triangle};
    std::deque<std::uint32_t> queue{hit.triangle};
    while (!queue.empty() && seeds.size() < 2000) {
      const std::uint32_t t = queue.front();
      queue.pop_front();
      const double d =
          std::sqrt(triangleDistance2(at, m.vertex(t, 0), m.vertex(t, 1), m.vertex(t, 2)));
      if (d > reach) {
        continue;
      }
      seeds.emplace_back(d, t);
      for (const std::int64_t n : m.neighbors[t]) {
        if (n >= 0 && seen.insert(static_cast<std::uint32_t>(n)).second) {
          queue.push_back(static_cast<std::uint32_t>(n));
        }
      }
    }
  }
  std::sort(seeds.begin(), seeds.end());
  // Every region grown from a seed, with its distance from `at`: the nearest wins, and among
  // regions about as near (the facets of a coarse mesh or a tangent edge) the largest.
  struct Candidate {
    double distance = 0.0;
    double area = 0.0;
    GeometricElement element;
    std::vector<std::uint32_t> region;
  };
  std::vector<Candidate> candidates;
  std::vector<char> covered(m.mesh.triangles.size(), 0);
  for (const auto& [d, seed] : seeds) {
    if (covered[seed] != 0) {
      continue;
    }
    GeometricElement element;
    std::vector<std::uint32_t> region = type == ElementType::kPlane
                                            ? m.growPlane(seed, nullptr, element)
                                            : m.growCurved(seed, type, nullptr, element);
    if (region.empty()) {
      covered[seed] = 1;
      continue;
    }
    double nearest2 = std::numeric_limits<double>::infinity();
    for (const std::uint32_t t : region) {
      covered[t] = 1;
      nearest2 =
          std::min(nearest2, triangleDistance2(at, m.vertex(t, 0), m.vertex(t, 1), m.vertex(t, 2)));
    }
    if (std::abs(elementDistance(element, at)) > reach) {
      continue;
    }
    candidates.push_back({std::sqrt(nearest2), m.area(region), element, std::move(region)});
  }
  if (candidates.empty()) {
    return std::nullopt;
  }
  double nearest = std::numeric_limits<double>::infinity();
  for (const Candidate& c : candidates) {
    nearest = std::min(nearest, c.distance);
  }
  const double near_enough = nearest + std::max(2.0 * m.tolerance, 1e-3 * m.diagonal);
  Candidate* best = nullptr;
  for (Candidate& c : candidates) {
    if (c.distance <= near_enough && (best == nullptr || c.area > best->area)) {
      best = &c;
    }
  }
  return m.feature(best->element, std::move(best->region));
}

RenderImage renderFeatures(const IndexedMesh& mesh, std::span<const CadFeature> features,
                           const RenderView& view) {
  std::vector<std::array<std::uint8_t, 3>> colors(mesh.triangles.size(), {196, 200, 207});
  std::vector<std::uint32_t> groups(mesh.triangles.size(), 0);
  for (std::size_t i = 0; i < features.size(); ++i) {
    for (const std::uint32_t t : features[i].triangles) {
      colors[t] = paletteColor(i);
      groups[t] = static_cast<std::uint32_t>(i + 1);
    }
  }
  RenderScene scene;
  const IndexedMesh colored = splitByGroup(mesh, groups, colors, scene.vertex_colors);
  scene.mesh = &colored;
  RenderImage image = render(scene, view);
  std::vector<std::array<double, 3>> anchors_3d;
  anchors_3d.reserve(features.size());
  for (const CadFeature& f : features) {
    anchors_3d.push_back(f.at);
  }
  const auto anchors = projectToImage(scene, view, anchors_3d);
  const int scale = view.width >= 1000 ? 2 : 1;
  const auto placed = placeBalloons(anchors, view.width, view.height, 8.0 * scale);
  for (std::size_t i = 0; i < features.size(); ++i) {
    drawBalloon(image, placed[i][0], placed[i][1], anchors[i][0], anchors[i][1],
                std::to_string(features[i].id), scale);
  }
  return image;
}

namespace {

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

constexpr std::array<KindInfo, 14> kKinds{{
    {"diameter", Kind::kDiameter, 1, false, "Diameter"},
    {"radius", Kind::kRadius, 1, false, "Radius"},
    {"distance", Kind::kDistance, 2, false, "Distance"},
    {"angle", Kind::kAngle, 2, false, "Angle"},
    {"coordinate", Kind::kCoordinate, 1, false, "Coordinate"},
    {"position", Kind::kPosition, 1, true, "Position"},
    {"flatness", Kind::kFlatness, 1, true, "Flatness"},
    {"cylindricity", Kind::kCylindricity, 1, true, "Cylindricity"},
    {"sphericity", Kind::kSphericity, 1, true, "Sphericity"},
    {"form", Kind::kForm, 1, true, "Form"},
    {"parallelism", Kind::kParallelism, 1, true, "Parallelism"},
    {"perpendicularity", Kind::kPerpendicularity, 1, true, "Perpendicularity"},
    {"profile", Kind::kProfile, 1, true, "Surface profile"},
    {"deviation", Kind::kDeviation, 1, false, "Surface point deviation"},
}};

const KindInfo& kindInfo(const std::string& name) {
  for (const KindInfo& info : kKinds) {
    if (name == info.name) {
      return info;
    }
  }
  std::string known;
  for (const KindInfo& info : kKinds) {
    known += (known.empty() ? "" : ", ") + std::string(info.name);
  }
  throw std::invalid_argument("Unknown characteristic type '" + name + "' (" + known + ")");
}

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

Vec readVec(const Json& json, const std::string& key, const std::string& what) {
  if (!json.contains(key)) {
    throw std::invalid_argument(what + ": missing '" + key + "'");
  }
  const Json& value = json.at(key);
  if (!value.is_array() || value.size() != 3 ||
      !std::all_of(value.begin(), value.end(), [](const Json& c) { return c.is_number(); })) {
    throw std::invalid_argument(what + ": '" + key + "' must be [x, y, z]");
  }
  return value.get<Vec>();
}

double readPositive(const Json& json, const std::string& key, const std::string& what) {
  if (!json.contains(key) || !json.at(key).is_number() || !(json.at(key).get<double>() > 0.0)) {
    throw std::invalid_argument(what + ": '" + key + "' must be a positive number");
  }
  return json.at(key).get<double>();
}

int axisIndex(const Json& value, const std::string& what) {
  if (value.is_string()) {
    const std::string axis = value.get<std::string>();
    if (axis == "x") {
      return 0;
    }
    if (axis == "y") {
      return 1;
    }
    if (axis == "z") {
      return 2;
    }
  }
  throw std::invalid_argument(what + R"(: axis must be "x", "y" or "z")");
}

std::string jsonText(const Json& value) {
  return value.is_string() ? value.get<std::string>() : value.dump();
}

ElementPlan parseElement(const Json& item, const FeatureFinder& finder) {
  if (!item.is_object() || !item.contains("id") || !item.contains("type")) {
    throw std::invalid_argument("Every element needs an id and a type");
  }
  ElementPlan e;
  e.id = jsonText(item.at("id"));
  const std::string what = "Element '" + e.id + "'";
  const ElementType type = elementTypeFromString(item.at("type").get<std::string>());
  e.search_mm = item.value("search_mm", 0.0);
  e.margin_mm = item.value("edge_margin_mm", 0.0);
  e.radius_mm = item.value("radius_mm", 0.0);
  if (item.contains("at")) {
    const Vec at = readVec(item, "at", what);
    const std::optional<CadFeature> found = finder.probe(type, at);
    if (!found) {
      throw std::invalid_argument(what + ": no " + toString(type) + " of the CAD model at (" +
                                  std::to_string(at[0]) + ", " + std::to_string(at[1]) + ", " +
                                  std::to_string(at[2]) +
                                  "); list the features of the model with cad_features");
    }
    e.nominal = found->element;
    e.triangles = found->triangles;
    e.anchor = found->at;
    e.from_cad = true;
    if (item.contains("inner") && type != ElementType::kPlane && type != ElementType::kPoint &&
        item.at("inner").get<bool>() != e.nominal.inner) {
      throw std::invalid_argument(what + ": the CAD model has a " +
                                  (e.nominal.inner ? "hole" : "boss") + " there");
    }
    return e;
  }
  e.nominal.type = type;
  switch (type) {
    case ElementType::kPlane:
      e.nominal.point = readVec(item, "point", what);
      e.nominal.direction = normalized(readVec(item, "normal", what));
      e.radius_mm = readPositive(item, "radius_mm", what);
      break;
    case ElementType::kCylinder:
      e.nominal.point = readVec(item, "point", what);
      e.nominal.direction = normalized(readVec(item, "direction", what));
      e.nominal.radius = 0.5 * readPositive(item, "diameter", what);
      e.nominal.length = readPositive(item, "length", what);
      e.nominal.inner = item.value("inner", true);
      break;
    case ElementType::kSphere:
      e.nominal.point = readVec(item, "center", what);
      e.nominal.radius = 0.5 * readPositive(item, "diameter", what);
      e.nominal.inner = item.value("inner", false);
      break;
    case ElementType::kPoint:
      e.nominal.point = readVec(item, "point", what);
      e.nominal.direction = normalized(readVec(item, "normal", what));
      break;
  }
  e.anchor = type == ElementType::kCylinder || type == ElementType::kSphere
                 ? e.nominal.point + e.nominal.radius * basis(e.nominal.direction).first
                 : e.nominal.point;
  return e;
}

std::size_t findElement(const std::vector<ElementPlan>& elements, const Json& id,
                        const std::string& what) {
  const std::string name = jsonText(id);
  for (std::size_t i = 0; i < elements.size(); ++i) {
    if (elements[i].id == name) {
      return i;
    }
  }
  throw std::invalid_argument(what + ": no element '" + name + "'");
}

CharacteristicPlan parseCharacteristic(const Json& item, std::size_t index,
                                       const std::vector<ElementPlan>& elements) {
  if (!item.is_object() || !item.contains("type")) {
    throw std::invalid_argument("Characteristic " + std::to_string(index + 1) + " needs a type");
  }
  CharacteristicPlan c;
  c.spec = item;
  c.number = item.contains("number") ? jsonText(item.at("number")) : std::to_string(index + 1);
  c.kind = &kindInfo(item.at("type").get<std::string>());
  const std::string what = "Characteristic " + c.number;
  if (item.contains("elements")) {
    for (const Json& id : item.at("elements")) {
      c.elements.push_back(findElement(elements, id, what));
    }
  } else if (item.contains("element")) {
    c.elements.push_back(findElement(elements, item.at("element"), what));
  }
  if (c.elements.size() != static_cast<std::size_t>(c.kind->elements)) {
    throw std::invalid_argument(what + " (" + c.kind->name + ") relates " +
                                std::to_string(c.kind->elements) + " element(s): give " +
                                (c.kind->elements == 1 ? "\"element\"" : "\"elements\": [a, b]"));
  }
  const auto type_of = [&](std::size_t k) { return elements[c.elements[k]].nominal.type; };
  const auto require = [&](bool ok, const std::string& message) {
    if (!ok) {
      throw std::invalid_argument(what + " (" + c.kind->name + "): " + message);
    }
  };
  switch (c.kind->kind) {
    case Kind::kDiameter:
    case Kind::kRadius:
      require(type_of(0) == ElementType::kCylinder || type_of(0) == ElementType::kSphere,
              "needs a cylinder or sphere");
      break;
    case Kind::kFlatness:
      require(type_of(0) == ElementType::kPlane, "needs a plane");
      break;
    case Kind::kCylindricity:
      require(type_of(0) == ElementType::kCylinder, "needs a cylinder");
      break;
    case Kind::kSphericity:
      require(type_of(0) == ElementType::kSphere, "needs a sphere");
      break;
    case Kind::kForm:
      require(type_of(0) != ElementType::kPoint, "needs a plane, cylinder or sphere");
      break;
    case Kind::kDeviation:
      require(type_of(0) == ElementType::kPoint, "needs a point");
      break;
    case Kind::kAngle:
      require(type_of(0) != ElementType::kSphere && type_of(1) != ElementType::kSphere,
              "needs planes, cylinders or points");
      break;
    case Kind::kParallelism:
    case Kind::kPerpendicularity: {
      require(type_of(0) == ElementType::kPlane || type_of(0) == ElementType::kCylinder,
              "needs a plane or cylinder");
      require(item.contains("datum"), "needs a datum");
      c.datum = findElement(elements, item.at("datum"), what);
      const ElementType d = elements[*c.datum].nominal.type;
      require(d == ElementType::kPlane || d == ElementType::kCylinder,
              "the datum must be a plane or cylinder");
      break;
    }
    case Kind::kCoordinate:
      require(item.contains("axis"), "needs an axis");
      c.axis = axisIndex(item.at("axis"), what);
      break;
    case Kind::kDistance:
      if (item.contains("along")) {
        c.axis = axisIndex(item.at("along"), what);
      }
      break;
    case Kind::kPosition:
    case Kind::kProfile:
      break;
  }
  if (item.contains("nominal")) {
    c.nominal = item.at("nominal").get<double>();
  }
  if (item.contains("upper")) {
    c.upper = item.at("upper").get<double>();
  }
  if (item.contains("lower")) {
    c.lower = item.at("lower").get<double>();
  }
  if (item.contains("tolerance")) {
    const double t = item.at("tolerance").get<double>();
    require(t >= 0.0, "the tolerance must not be negative");
    if (!c.upper) {
      c.upper = t;
    }
    if (!c.lower && !c.kind->zone) {
      c.lower = -t;
    }
  }
  if (c.upper && c.lower) {
    require(*c.lower <= *c.upper, "the lower deviation lies above the upper one");
  }
  std::string label = item.value("label", std::string());
  if (label.empty()) {
    label = c.kind->label;
    if (c.axis >= 0) {
      label += std::string(" ") + "xyz"[c.axis];
    }
    for (const std::size_t e : c.elements) {
      label += " " + elements[e].id;
    }
    if (c.datum) {
      label += " to " + elements[*c.datum].id;
    }
  }
  c.label = label;
  return c;
}

// ---------------------------------------------------------------------------------------------
// Extraction of the scanned points of an element

/// Edges of a region of triangles, to keep points away from them.
class EdgeGrid {
 public:
  EdgeGrid(const FeatureFinder& finder, const std::vector<std::uint32_t>& region,
           const std::vector<char>& member, double margin)
      : cell_(std::max(margin, 1e-6)), margin_(margin) {
    const IndexedMesh& mesh = finder.mesh();
    // Neighbours are recovered from shared vertex pairs within the region.
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edge_count;
    for (const std::uint32_t t : region) {
      const auto& tri = mesh.triangles[t];
      for (std::size_t k = 0; k < 3; ++k) {
        const std::uint32_t a = tri[k];
        const std::uint32_t b = tri[(k + 1) % 3];
        ++edge_count[{std::min(a, b), std::max(a, b)}];
      }
    }
    (void)member;
    for (const auto& [edge, count] : edge_count) {
      if (count != 1) {
        continue;  // inner edge of the region
      }
      const Vec a = toVec(mesh.points[edge.first]);
      const Vec b = toVec(mesh.points[edge.second]);
      const auto index = static_cast<std::uint32_t>(segments_.size());
      segments_.emplace_back(a, b);
      const double length = norm(b - a);
      const int steps = std::max(1, static_cast<int>(std::ceil(2.0 * length / cell_)));
      for (int s = 0; s <= steps; ++s) {
        const Vec p = a + (static_cast<double>(s) / steps) * (b - a);
        const auto c = cellOf(p);
        for (std::int64_t dz = -1; dz <= 1; ++dz) {
          for (std::int64_t dy = -1; dy <= 1; ++dy) {
            for (std::int64_t dx = -1; dx <= 1; ++dx) {
              auto& list = cells_[key({c[0] + dx, c[1] + dy, c[2] + dz})];
              if (list.empty() || list.back() != index) {
                list.push_back(index);
              }
            }
          }
        }
      }
    }
  }

  /// Whether `p` lies within the margin of an edge.
  [[nodiscard]] bool nearEdge(const Vec& p) const {
    if (margin_ <= 0.0) {
      return false;
    }
    const auto it = cells_.find(key(cellOf(p)));
    if (it == cells_.end()) {
      return false;
    }
    for (const std::uint32_t i : it->second) {
      const auto& [a, b] = segments_[i];
      const Vec ab = b - a;
      const double t = std::clamp(dot(p - a, ab) / std::max(dot(ab, ab), 1e-300), 0.0, 1.0);
      if (norm(p - (a + t * ab)) < margin_) {
        return true;
      }
    }
    return false;
  }

 private:
  [[nodiscard]] std::array<std::int64_t, 3> cellOf(const Vec& p) const {
    return {static_cast<std::int64_t>(std::floor(p[0] / cell_)),
            static_cast<std::int64_t>(std::floor(p[1] / cell_)),
            static_cast<std::int64_t>(std::floor(p[2] / cell_))};
  }
  static std::uint64_t key(const std::array<std::int64_t, 3>& c) {
    constexpr std::uint64_t kMask = (1U << 21U) - 1U;
    return ((static_cast<std::uint64_t>(c[0]) & kMask) << 42U) |
           ((static_cast<std::uint64_t>(c[1]) & kMask) << 21U) |
           (static_cast<std::uint64_t>(c[2]) & kMask);
  }

  double cell_;
  double margin_;
  std::vector<std::pair<Vec, Vec>> segments_;
  std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells_;
};

/// Corners of the box around an element's nominal region, frame coordinates.
std::pair<Vec, Vec> elementBounds(const ElementPlan& e, const FeatureFinder& finder, double pad) {
  Vec lo{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
         std::numeric_limits<double>::max()};
  Vec hi = -1.0 * lo;
  const auto extend = [&](const Vec& p, double r) {
    for (std::size_t k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], p[k] - r);
      hi[k] = std::max(hi[k], p[k] + r);
    }
  };
  const GeometricElement& n = e.nominal;
  if (!e.triangles.empty()) {
    const IndexedMesh& mesh = finder.mesh();
    for (const std::uint32_t t : e.triangles) {
      for (const std::uint32_t v : mesh.triangles[t]) {
        extend(toVec(mesh.points[v]), pad);
      }
    }
  } else if (n.type == ElementType::kCylinder) {
    extend(n.point + (0.5 * n.length) * n.direction, n.radius + pad);
    extend(n.point - (0.5 * n.length) * n.direction, n.radius + pad);
  } else if (n.type == ElementType::kSphere) {
    extend(n.point, n.radius + pad);
  } else {  // plane or point: a disc of radius_mm
    extend(n.point, e.radius_mm + pad);
  }
  return {lo, hi};
}

struct Extraction {
  const SurfaceMask* mask = nullptr;
  const FeatureFinder* finder = nullptr;
  const MeshDistance* cad = nullptr;
  MeasureOptions options;
  double voxel_mm = 0.0;
};

ElementResult extractAndFit(const Extraction& x, const ElementPlan& e,
                            const RigidTransform& scan_to_frame) {
  ElementResult result;
  const double search = e.search_mm > 0.0
                            ? e.search_mm
                            : (x.options.search_mm > 0.0 ? x.options.search_mm : 5.0 * x.voxel_mm);
  const double margin =
      e.margin_mm > 0.0
          ? e.margin_mm
          : (x.options.edge_margin_mm > 0.0 ? x.options.edge_margin_mm : 2.0 * x.voxel_mm);
  const double point_radius = e.radius_mm > 0.0 ? e.radius_mm : 3.0 * x.voxel_mm;
  const GeometricElement& nominal = e.nominal;
  const auto [lo, hi] = elementBounds(e, *x.finder, search + margin);
  // The box in scan voxels.
  const RigidTransform frame_to_scan = scan_to_frame.inverse();
  const SurfaceInfo& info = x.mask->info();
  Vec scan_lo{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
              std::numeric_limits<double>::max()};
  Vec scan_hi = -1.0 * scan_lo;
  for (int corner = 0; corner < 8; ++corner) {
    const Vec p{(corner & 1) != 0 ? hi[0] : lo[0], (corner & 2) != 0 ? hi[1] : lo[1],
                (corner & 4) != 0 ? hi[2] : lo[2]};
    const Vec q = frame_to_scan.apply(p);
    for (std::size_t k = 0; k < 3; ++k) {
      scan_lo[k] = std::min(scan_lo[k], q[k]);
      scan_hi[k] = std::max(scan_hi[k], q[k]);
    }
  }
  Box box;
  for (std::size_t k = 0; k < 3; ++k) {
    box.min[k] = static_cast<std::int64_t>(std::floor(scan_lo[k] / info.voxel_size[k])) - 1;
    box.max[k] = static_cast<std::int64_t>(std::ceil(scan_hi[k] / info.voxel_size[k])) + 2;
    box.min[k] = std::clamp<std::int64_t>(box.min[k], 0, info.dims[k]);
    box.max[k] = std::clamp<std::int64_t>(box.max[k], box.min[k], info.dims[k]);
  }
  int stride = 1;
  constexpr double kMaxVoxels = 128.0 * 1024.0 * 1024.0;
  while (static_cast<double>(box.voxelCount()) / (stride * stride * stride) > kMaxVoxels) {
    ++stride;
  }
  const std::vector<SurfacePoint> raw = surfacePoints(*x.mask, box, stride);
  std::vector<char> member;
  std::optional<EdgeGrid> edges;
  if (!e.triangles.empty()) {
    member.assign(x.finder->mesh().triangles.size(), 0);
    for (const std::uint32_t t : e.triangles) {
      member[t] = 1;
    }
    edges.emplace(*x.finder, e.triangles, member, margin);
  }
  const double cos_angle = std::cos(radians(x.options.max_normal_angle_deg));
  std::vector<char> keep(raw.size(), 0);
  std::vector<double> deviation(raw.size(), 0.0);
  std::vector<Vec> position(raw.size());
  std::vector<Vec> normal(raw.size());
  tbb::parallel_for(tbb::blocked_range<std::size_t>(0, raw.size(), 4096), [&](const auto& range) {
    for (std::size_t i = range.begin(); i < range.end(); ++i) {
      const Vec p = scan_to_frame.apply(raw[i].position);
      const Vec n = scan_to_frame.rotate(raw[i].normal);
      position[i] = p;
      normal[i] = n;
      const double d = elementDistance(nominal, p);
      if (std::abs(d) > search || dot(n, elementNormal(nominal, p)) < cos_angle) {
        continue;
      }
      if (nominal.type == ElementType::kPoint) {
        const Vec w = p - nominal.point;
        if (norm(w - dot(w, nominal.direction) * nominal.direction) > point_radius) {
          continue;
        }
      } else if (e.triangles.empty()) {
        if (nominal.type == ElementType::kPlane) {
          const Vec w = p - nominal.point;
          if (norm(w - dot(w, nominal.direction) * nominal.direction) > e.radius_mm - margin) {
            continue;
          }
        } else if (nominal.type == ElementType::kCylinder) {
          if (std::abs(dot(p - nominal.point, nominal.direction)) > 0.5 * nominal.length - margin) {
            continue;
          }
        }
      }
      const MeshDistance::Hit hit = x.cad->query(p);
      if (!e.triangles.empty()) {
        if (member[hit.triangle] == 0 || edges->nearEdge(hit.closest)) {
          continue;
        }
      }
      deviation[i] = hit.distance;
      keep[i] = 1;
    }
  });
  std::vector<SurfacePoint> points;
  std::vector<double> kept_deviation;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (keep[i] != 0) {
      points.push_back({position[i], normal[i]});
      kept_deviation.push_back(deviation[i]);
    }
  }
  if (points.size() > x.options.max_points && x.options.max_points > 0) {
    const double step =
        static_cast<double>(points.size()) / static_cast<double>(x.options.max_points);
    std::vector<SurfacePoint> thinned;
    std::vector<double> thinned_deviation;
    for (std::size_t k = 0; k < x.options.max_points; ++k) {
      const auto i = static_cast<std::size_t>(static_cast<double>(k) * step);
      thinned.push_back(points[i]);
      thinned_deviation.push_back(kept_deviation[i]);
    }
    points = std::move(thinned);
    kept_deviation = std::move(thinned_deviation);
  }
  try {
    result.fit = fitElement(points, nominal);
    result.measured = true;
  } catch (const std::invalid_argument& error) {
    result.message = std::string("not enough scanned surface found: ") + error.what();
    return result;
  }
  result.points.reserve(points.size());
  for (const SurfacePoint& p : points) {
    result.points.push_back(p.position);
  }
  result.cad_deviation = std::move(kept_deviation);
  return result;
}

// ---------------------------------------------------------------------------------------------
// Alignment on datums

/// One linear equation on the small motion (rotation vector w, translation t) of the measured
/// datums about `origin`: row . (w, t) = rhs.
struct Equation {
  std::array<double, 6> row{};
  double rhs = 0.0;
};

/// Equations that bring the direction `d` (measured) onto `n` (nominal).
void directionEquations(const Vec& d, const Vec& n, std::vector<Equation>& out) {
  const auto [u, v] = basis(n);
  for (const Vec& b : {u, v}) {
    Equation eq;
    const Vec r = cross(d, b);
    eq.row = {r[0], r[1], r[2], 0.0, 0.0, 0.0};
    eq.rhs = -dot(b, d);
    out.push_back(eq);
  }
}

/// Equation that moves point `p` (measured) along `e` onto the nominal value.
Equation offsetEquation(const Vec& p, const Vec& nominal, const Vec& e, const Vec& origin) {
  Equation eq;
  const Vec r = cross(p - origin, e);
  eq.row = {r[0], r[1], r[2], e[0], e[1], e[2]};
  eq.rhs = -dot(e, p - nominal);
  return eq;
}

std::vector<Equation> datumEquations(const GeometricElement& nominal,
                                     const GeometricElement& actual, const DatumPlan& datum,
                                     const Vec& origin) {
  std::vector<Equation> eqs;
  switch (nominal.type) {
    case ElementType::kPlane:
      directionEquations(actual.direction, nominal.direction, eqs);
      eqs.push_back(offsetEquation(actual.point, nominal.point, nominal.direction, origin));
      break;
    case ElementType::kCylinder: {
      Vec d = actual.direction;
      if (dot(d, nominal.direction) < 0.0) {
        d = -1.0 * d;
      }
      directionEquations(d, nominal.direction, eqs);
      // The point of the measured axis level with the nominal axis point.
      const Vec p = actual.point + dot(nominal.point - actual.point, d) * d;
      const auto [u, v] = basis(nominal.direction);
      eqs.push_back(offsetEquation(p, nominal.point, u, origin));
      eqs.push_back(offsetEquation(p, nominal.point, v, origin));
      break;
    }
    case ElementType::kSphere:
      for (const Vec& e : {Vec{1.0, 0.0, 0.0}, Vec{0.0, 1.0, 0.0}, Vec{0.0, 0.0, 1.0}}) {
        eqs.push_back(offsetEquation(actual.point, nominal.point, e, origin));
      }
      break;
    case ElementType::kPoint:
      if (datum.directions.empty()) {
        eqs.push_back(offsetEquation(actual.point, nominal.point, nominal.direction, origin));
      }
      for (const Vec& e : datum.directions) {
        eqs.push_back(offsetEquation(actual.point, nominal.point, e, origin));
      }
      break;
  }
  return eqs;
}

/// The small motion that satisfies the equations of the datums in order of precedence: each
/// datum is solved within the freedom the datums before it left; freedom no datum fixes keeps
/// the starting pose. Returns the motion and the number of degrees of freedom fixed.
std::pair<std::array<double, 6>, int> solveDatums(
    const std::vector<std::vector<Equation>>& datums) {
  std::array<double, 6> x{};
  // Columns of the free subspace, 6 x free.
  std::vector<std::array<double, 6>> free;
  for (std::size_t i = 0; i < 6; ++i) {
    std::array<double, 6> column{};
    column[i] = 1.0;
    free.push_back(column);
  }
  for (const auto& eqs : datums) {
    if (free.empty() || eqs.empty()) {
      continue;
    }
    const std::size_t m = free.size();
    // A = J N, b = rhs - J x.
    std::vector<double> ata(m * m, 0.0);
    std::vector<double> atb(m, 0.0);
    double scale = 0.0;
    for (const Equation& eq : eqs) {
      std::vector<double> a(m, 0.0);
      for (std::size_t j = 0; j < m; ++j) {
        for (std::size_t k = 0; k < 6; ++k) {
          a[j] += eq.row[k] * free[j][k];
        }
      }
      double jx = 0.0;
      for (std::size_t k = 0; k < 6; ++k) {
        jx += eq.row[k] * x[k];
        scale += eq.row[k] * eq.row[k];
      }
      const double b = eq.rhs - jx;
      for (std::size_t i = 0; i < m; ++i) {
        for (std::size_t j = 0; j < m; ++j) {
          ata[i * m + j] += a[i] * a[j];
        }
        atb[i] += a[i] * b;
      }
    }
    const auto [values, vectors] = symmetricEigen(ata, m);
    // Constraints the datums before already fixed leave only tiny components; they fix nothing.
    const double threshold = 1e-6 * std::max(scale, 1e-300);
    std::vector<std::array<double, 6>> remaining;
    for (std::size_t j = 0; j < m; ++j) {
      std::array<double, 6> direction{};
      for (std::size_t i = 0; i < m; ++i) {
        for (std::size_t k = 0; k < 6; ++k) {
          direction[k] += vectors[i * m + j] * free[i][k];
        }
      }
      if (values[j] <= threshold) {
        remaining.push_back(direction);
        continue;
      }
      double projection = 0.0;
      for (std::size_t i = 0; i < m; ++i) {
        projection += vectors[i * m + j] * atb[i];
      }
      for (std::size_t k = 0; k < 6; ++k) {
        x[k] += direction[k] * projection / values[j];
      }
    }
    free = std::move(remaining);
  }
  return {x, static_cast<int>(6 - free.size())};
}

RigidTransform smallMotion(const std::array<double, 6>& x, const Vec& origin) {
  const Vec w{x[0], x[1], x[2]};
  const double angle = norm(w);
  RigidTransform rotation;
  if (angle > 0.0) {
    rotation = RigidTransform::fromAxisAngle(w, degrees(angle));
  }
  // p' = R (p - o) + o + t
  rotation.translation = origin - rotation.rotate(origin) + Vec{x[3], x[4], x[5]};
  return rotation;
}

GeometricElement moved(GeometricElement e, const RigidTransform& t) {
  e.point = t.apply(e.point);
  e.direction = t.rotate(e.direction);
  return e;
}

// ---------------------------------------------------------------------------------------------
// Characteristics

Vec directionOf(const GeometricElement& e) { return e.direction; }

double distanceValue(const GeometricElement& a, const GeometricElement& b, int along) {
  if (along >= 0) {
    return std::abs(b.point[static_cast<std::size_t>(along)] -
                    a.point[static_cast<std::size_t>(along)]);
  }
  if (a.type == ElementType::kPlane) {
    return std::abs(dot(a.direction, b.point - a.point));
  }
  if (b.type == ElementType::kPlane) {
    return std::abs(dot(b.direction, a.point - b.point));
  }
  if (a.type == ElementType::kCylinder) {
    return axisDistance(a, b.point);
  }
  if (b.type == ElementType::kCylinder) {
    return axisDistance(b, a.point);
  }
  return norm(b.point - a.point);
}

double angleValue(const GeometricElement& a, const GeometricElement& b) {
  const double angle =
      degrees(std::acos(std::clamp(dot(directionOf(a), directionOf(b)), -1.0, 1.0)));
  const bool axis_a = a.type == ElementType::kCylinder;
  const bool axis_b = b.type == ElementType::kCylinder;
  const double folded = std::min(angle, 180.0 - angle);  // an axis has no side
  if (axis_a && axis_b) {
    return folded;
  }
  if (axis_a || axis_b) {
    return 90.0 - folded;  // between an axis and a face, from the face's normal
  }
  return angle;
}

double spread(const std::vector<Vec>& points, const Vec& direction) {
  double lo = std::numeric_limits<double>::max();
  double hi = std::numeric_limits<double>::lowest();
  for (const Vec& p : points) {
    const double s = dot(p, direction);
    lo = std::min(lo, s);
    hi = std::max(hi, s);
  }
  return points.empty() ? 0.0 : hi - lo;
}

std::pair<Vec, Vec> axisEnds(const GeometricElement& cylinder) {
  const Vec half = (0.5 * cylinder.length) * cylinder.direction;
  return {cylinder.point - half, cylinder.point + half};
}

Vec perpendicularPart(const Vec& v, const Vec& to) { return v - dot(v, to) * to; }

double orientationZone(Kind kind, const GeometricElement& feature, const std::vector<Vec>& points,
                       const GeometricElement& datum) {
  const Vec r = normalized(datum.direction);
  const bool datum_plane = datum.type == ElementType::kPlane;
  // Planes perpendicular to the datum plane or parallel to the datum axis, turned like the
  // feature.
  const Vec across = normalized(perpendicularPart(feature.direction, r));
  if (feature.type == ElementType::kPlane) {
    if (kind == Kind::kParallelism) {
      return datum_plane ? spread(points, r) : spread(points, across);
    }
    return datum_plane ? spread(points, across) : spread(points, r);
  }
  const auto [e1, e2] = axisEnds(feature);
  const Vec s = e2 - e1;
  const bool along_r = (kind == Kind::kParallelism) != datum_plane;
  return along_r ? norm(perpendicularPart(s, r)) : std::abs(dot(s, r));
}

double positionZone(const GeometricElement& nominal, const GeometricElement& actual,
                    const std::vector<Vec>& points) {
  switch (nominal.type) {
    case ElementType::kCylinder: {
      const auto [e1, e2] = axisEnds(actual);
      return 2.0 * std::max(axisDistance(nominal, e1), axisDistance(nominal, e2));
    }
    case ElementType::kPlane: {
      double worst = 0.0;
      for (const Vec& p : points) {
        worst = std::max(worst, std::abs(dot(p - nominal.point, nominal.direction)));
      }
      return 2.0 * worst;
    }
    case ElementType::kSphere:
    case ElementType::kPoint:
      return 2.0 * norm(actual.point - nominal.point);
  }
  return 0.0;
}

double characteristicValue(const CharacteristicPlan& c,
                           const std::vector<const GeometricElement*>& elements,
                           const GeometricElement* datum) {
  const GeometricElement& a = *elements[0];
  switch (c.kind->kind) {
    case Kind::kDiameter:
      return 2.0 * a.radius;
    case Kind::kRadius:
      return a.radius;
    case Kind::kDistance:
      return distanceValue(a, *elements[1], c.axis);
    case Kind::kAngle:
      return angleValue(a, *elements[1]);
    case Kind::kCoordinate:
      return a.point[static_cast<std::size_t>(c.axis)];
    default:
      (void)datum;
      return 0.0;
  }
}

std::string unitOf(Kind kind) { return kind == Kind::kAngle ? "deg" : "mm"; }

}  // namespace

Measurement measure(const SurfaceMask& mask, const Mesh& cad, const RigidTransform& cad_to_scan,
                    const nlohmann::json& plan, const MeasureOptions& options) {
  if (!plan.is_object()) {
    throw std::invalid_argument("The inspection plan must be a JSON object");
  }
  std::optional<TelemetryPhase> phase(std::in_place, "nominal elements");
  const FeatureFinder finder(cad);
  const MeshDistance cad_distance(cad);
  std::vector<ElementPlan> elements;
  for (const Json& item : plan.value("elements", Json::array())) {
    elements.push_back(parseElement(item, finder));
    for (std::size_t i = 0; i + 1 < elements.size(); ++i) {
      if (elements[i].id == elements.back().id) {
        throw std::invalid_argument("Element '" + elements.back().id + "' is defined twice");
      }
    }
  }
  if (elements.empty()) {
    throw std::invalid_argument("The inspection plan has no elements");
  }
  std::vector<DatumPlan> datums;
  const Json alignment = plan.value("alignment", Json("best_fit"));
  if (alignment.is_object() && alignment.contains("datums")) {
    for (const Json& item : alignment.at("datums")) {
      DatumPlan datum;
      const Json id = item.is_object() ? item.value("element", Json()) : item;
      datum.element = findElement(elements, id, "Datum");
      if (item.is_object() && item.contains("directions")) {
        for (const Json& d : item.at("directions")) {
          if (d.is_string()) {
            Vec axis{};
            axis[static_cast<std::size_t>(axisIndex(d, "Datum " + jsonText(id)))] = 1.0;
            datum.directions.push_back(axis);
          } else {
            datum.directions.push_back(normalized(d.get<Vec>()));
          }
        }
      }
      datums.push_back(datum);
    }
  } else if (!(alignment.is_string() && alignment.get<std::string>() == "best_fit")) {
    throw std::invalid_argument(
        R"(alignment must be "best_fit" or {"datums": [primary, secondary, tertiary]})");
  }
  std::vector<CharacteristicPlan> characteristics;
  const Json items = plan.value("characteristics", Json::array());
  for (std::size_t i = 0; i < items.size(); ++i) {
    characteristics.push_back(parseCharacteristic(items[i], i, elements));
  }

  const VoxelSize& v = mask.info().voxel_size;
  Extraction x;
  x.mask = &mask;
  x.finder = &finder;
  x.cad = &cad_distance;
  x.options = options;
  x.voxel_mm = std::max({v[0], v[1], v[2]});

  RigidTransform correction;  // in the CAD (drawing) frame, after the best fit
  const RigidTransform best_fit = cad_to_scan.inverse();
  std::vector<ElementResult> results(elements.size());
  int fixed = 0;
  Json datum_json = Json::array();
  if (!datums.empty()) {
    phase.reset();
    phase.emplace("datum alignment");
    Vec origin{};
    for (const DatumPlan& d : datums) {
      origin = origin + elements[d.element].nominal.point;
    }
    origin = (1.0 / static_cast<double>(datums.size())) * origin;
    for (int pass = 0; pass < 3; ++pass) {
      const RigidTransform scan_to_frame = correction.after(best_fit);
      std::vector<GeometricElement> measured;
      for (const DatumPlan& d : datums) {
        ElementResult& r = results[d.element];
        r = extractAndFit(x, elements[d.element], scan_to_frame);
        if (!r.measured) {
          throw std::invalid_argument("Datum '" + elements[d.element].id +
                                      "' could not be measured: " + r.message);
        }
        measured.push_back(r.fit.element);
      }
      // Gauss-Newton on the small motion, the datums in order of precedence.
      RigidTransform step;
      for (int iteration = 0; iteration < 20; ++iteration) {
        std::vector<std::vector<Equation>> equations;
        for (std::size_t i = 0; i < datums.size(); ++i) {
          equations.push_back(datumEquations(elements[datums[i].element].nominal,
                                             moved(measured[i], step), datums[i], origin));
        }
        const auto [motion, dof] = solveDatums(equations);
        fixed = dof;
        step = smallMotion(motion, origin).after(step);
        double size = 0.0;
        for (const double m : motion) {
          size = std::max(size, std::abs(m));
        }
        if (size < 1e-12) {
          break;
        }
      }
      correction = step.after(correction);
    }
    for (const DatumPlan& d : datums) {
      Json entry = {{"element", elements[d.element].id}};
      if (!d.directions.empty()) {
        entry["directions"] = d.directions;
      }
      datum_json.push_back(entry);
    }
  }

  phase.reset();
  phase.emplace("elements");
  const RigidTransform scan_to_frame = correction.after(best_fit);
  for (std::size_t i = 0; i < elements.size(); ++i) {
    results[i] = extractAndFit(x, elements[i], scan_to_frame);
  }

  phase.reset();
  phase.emplace("characteristics");
  Measurement measurement;
  measurement.scan_to_cad = scan_to_frame;
  Json element_json = Json::array();
  for (std::size_t i = 0; i < elements.size(); ++i) {
    const ElementPlan& e = elements[i];
    const ElementResult& r = results[i];
    Json entry = {{"id", e.id},
                  {"type", toString(e.nominal.type)},
                  {"source", e.from_cad ? "cad" : "given"},
                  {"anchor", e.anchor},
                  {"nominal", toJson(e.nominal)},
                  {"measured", r.measured}};
    if (r.measured) {
      entry["actual"] = toJson(r.fit.element);
      entry["points"] = r.fit.points;
      entry["outliers"] = r.fit.outliers;
      entry["rms_mm"] = r.fit.rms_mm;
      entry["form_mm"] = r.fit.formMm();
    } else {
      entry["message"] = r.message;
    }
    element_json.push_back(entry);
    measurement.element_triangles.push_back(e.triangles);
  }

  Json characteristic_json = Json::array();
  for (const CharacteristicPlan& c : characteristics) {
    Json entry = {{"number", c.number},
                  {"label", c.label},
                  {"type", c.kind->name},
                  {"unit", unitOf(c.kind->kind)}};
    Json ids = Json::array();
    for (const std::size_t e : c.elements) {
      ids.push_back(elements[e].id);
    }
    entry["elements"] = ids;
    if (c.datum) {
      entry["datum"] = elements[*c.datum].id;
    }
    if (c.axis >= 0) {
      entry[c.kind->kind == Kind::kCoordinate ? "axis" : "along"] = std::string(1, "xyz"[c.axis]);
    }
    for (const char* key : {"drawing_zone", "method", "note"}) {
      if (c.spec.contains(key)) {
        entry[key] = c.spec.at(key);
      }
    }
    std::vector<const GeometricElement*> nominal;
    std::vector<const GeometricElement*> actual;
    std::string missing;
    for (const std::size_t e : c.elements) {
      nominal.push_back(&elements[e].nominal);
      actual.push_back(&results[e].fit.element);
      if (!results[e].measured) {
        missing = elements[e].id;
      }
    }
    if (c.datum && !results[*c.datum].measured) {
      missing = elements[*c.datum].id;
    }
    double nominal_value = c.kind->zone || c.kind->kind == Kind::kDeviation
                               ? 0.0
                               : characteristicValue(c, nominal, nullptr);
    if (c.nominal) {
      nominal_value = *c.nominal;
    }
    entry["nominal"] = nominal_value;
    if (c.upper) {
      entry["upper"] = *c.upper;
    }
    if (c.lower) {
      entry["lower"] = *c.lower;
    } else if (c.kind->zone) {
      entry["lower"] = 0.0;
    }
    if (!missing.empty()) {
      entry["result"] = "not measured";
      entry["passed"] = nullptr;
      entry["message"] = "element '" + missing + "' was not measured";
      ++measurement.not_measured;
      characteristic_json.push_back(entry);
      continue;
    }
    const std::size_t first = c.elements[0];
    const ElementResult& r = results[first];
    double value = 0.0;
    switch (c.kind->kind) {
      case Kind::kFlatness:
      case Kind::kCylindricity:
      case Kind::kSphericity:
      case Kind::kForm:
        value = r.fit.formMm();
        break;
      case Kind::kParallelism:
      case Kind::kPerpendicularity:
        value =
            orientationZone(c.kind->kind, r.fit.element, r.points, results[*c.datum].fit.element);
        break;
      case Kind::kPosition:
        value = positionZone(elements[first].nominal, r.fit.element, r.points);
        break;
      case Kind::kProfile: {
        double worst = 0.0;
        for (const double d : r.cad_deviation) {
          worst = std::max(worst, std::abs(d));
        }
        value = 2.0 * worst;
        break;
      }
      case Kind::kDeviation:
        value = dot(r.fit.element.point - elements[first].nominal.point,
                    elements[first].nominal.direction);
        break;
      default:
        value = characteristicValue(c, actual, nullptr);
        break;
    }
    const double deviation = value - nominal_value;
    entry["actual"] = value;
    entry["deviation"] = deviation;
    if (!c.upper && !c.lower) {
      entry["result"] = "no tolerance";
      entry["passed"] = nullptr;
      ++measurement.unrated;
    } else {
      const bool passed = (!c.upper || deviation <= *c.upper + 1e-12) &&
                          (!c.lower || deviation >= *c.lower - 1e-12);
      entry["passed"] = passed;
      entry["result"] = passed ? "OK" : "NOK";
      if (c.upper) {
        const double band = c.lower ? *c.upper - *c.lower : *c.upper;
        if (band > 0.0) {
          const double middle = c.lower ? 0.5 * (*c.upper + *c.lower) : 0.0;
          entry["utilization_percent"] = c.lower
                                             ? 100.0 * std::abs(deviation - middle) / (0.5 * band)
                                             : 100.0 * deviation / band;
        }
      }
      ++(passed ? measurement.passed : measurement.failed);
    }
    characteristic_json.push_back(entry);
  }
  measurement.characteristics = characteristics.size();

  Json alignment_json;
  if (datums.empty()) {
    alignment_json = {{"method", "best_fit"}};
  } else {
    alignment_json = {{"method", "datums"},
                      {"datums", datum_json},
                      {"fixed_degrees_of_freedom", fixed},
                      {"correction_rotation_deg", correction.angleDegrees()},
                      {"correction_translation_mm", correction.translation}};
  }
  alignment_json["scan_to_cad"] = scan_to_frame.matrix();
  measurement.json = {
      {"alignment", alignment_json},
      {"voxel_size_mm", v},
      {"search_mm", options.search_mm > 0.0 ? options.search_mm : 5.0 * x.voxel_mm},
      {"edge_margin_mm", options.edge_margin_mm > 0.0 ? options.edge_margin_mm : 2.0 * x.voxel_mm},
      {"elements", element_json},
      {"characteristics", characteristic_json},
      {"summary",
       {{"characteristics", measurement.characteristics},
        {"passed", measurement.passed},
        {"failed", measurement.failed},
        {"not_measured", measurement.not_measured},
        {"unrated", measurement.unrated},
        {"all_passed", measurement.allPassed()}}}};
  return measurement;
}

std::string characteristicsCsv(const nlohmann::json& measurement) {
  const auto field = [](const Json& value) -> std::string {
    if (value.is_null()) {
      return "";
    }
    if (value.is_number()) {
      std::array<char, 32> text{};
      (void)std::snprintf(text.data(), text.size(), "%.4f", value.get<double>());
      return text.data();
    }
    std::string s = value.is_string() ? value.get<std::string>() : value.dump();
    if (s.find_first_of(",\"\n") != std::string::npos) {
      std::string quoted = "\"";
      for (const char ch : s) {
        quoted += ch == '"' ? std::string("\"\"") : std::string(1, ch);
      }
      return quoted + "\"";
    }
    return s;
  };
  std::ostringstream out;
  out << "number,label,type,elements,datum,nominal,upper,lower,actual,deviation,unit,result\n";
  for (const Json& c : measurement.value("characteristics", Json::array())) {
    std::string ids;
    for (const Json& id : c.value("elements", Json::array())) {
      ids += (ids.empty() ? "" : " ") + id.get<std::string>();
    }
    out << field(c.value("number", Json())) << ',' << field(c.value("label", Json())) << ','
        << field(c.value("type", Json())) << ',' << field(ids) << ','
        << field(c.value("datum", Json())) << ',' << field(c.value("nominal", Json())) << ','
        << field(c.value("upper", Json())) << ',' << field(c.value("lower", Json())) << ','
        << field(c.value("actual", Json())) << ',' << field(c.value("deviation", Json())) << ','
        << field(c.value("unit", Json())) << ',' << field(c.value("result", Json())) << '\n';
  }
  return out.str();
}

void writeMeasurement(const Measurement& measurement, const Mesh& cad,
                      const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
  {
    std::ofstream out(dir / "measurement.json", std::ios::binary);
    out << measurement.json.dump(2) << '\n';
    if (!out) {
      throw std::runtime_error("Cannot write " + (dir / "measurement.json").string());
    }
  }
  {
    std::ofstream out(dir / "characteristics.csv", std::ios::binary);
    out << characteristicsCsv(measurement.json);
  }
  const IndexedMesh mesh = indexedMesh(cad);
  const Json& elements = measurement.json.at("elements");
  const Json& characteristics = measurement.json.at("characteristics");
  // Colour of an element: red when one of its characteristics failed, amber when one could not
  // be measured, green when all passed, blue without tolerances.
  std::map<std::string, int> state;  // 0 none, 1 unrated, 2 ok, 3 not measured, 4 failed
  for (const Json& c : characteristics) {
    const std::string result = c.value("result", "");
    const int s = result == "NOK" ? 4 : (result == "not measured" ? 3 : (result == "OK" ? 2 : 1));
    for (const Json& id : c.at("elements")) {
      int& current = state[id.get<std::string>()];
      current = std::max(current, s);
    }
  }
  const auto color = [&](int s) -> std::array<std::uint8_t, 3> {
    switch (s) {
      case 4:
        return {214, 39, 40};
      case 3:
        return {240, 168, 32};
      case 2:
        return {60, 170, 90};
      case 1:
        return {70, 130, 200};
      default:
        return {150, 155, 165};
    }
  };
  RenderScene scene;
  std::vector<std::array<std::uint8_t, 3>> colors(mesh.triangles.size(), {205, 208, 214});
  std::vector<std::uint32_t> groups(mesh.triangles.size(), 0);
  std::map<std::string, Vec> anchors;
  double size = 0.0;
  {
    const Bounds b = meshBounds(cad);
    size = norm(Vec{b.max[0] - b.min[0], b.max[1] - b.min[1], b.max[2] - b.min[2]});
  }
  for (std::size_t i = 0; i < elements.size(); ++i) {
    const std::string id = elements[i].at("id").get<std::string>();
    anchors[id] = elements[i].at("anchor").get<Vec>();
    const auto c = color(state.count(id) != 0 ? state.at(id) : 0);
    if (i < measurement.element_triangles.size() && !measurement.element_triangles[i].empty()) {
      for (const std::uint32_t t : measurement.element_triangles[i]) {
        if (t < mesh.triangles.size()) {
          colors[t] = c;
          groups[t] = static_cast<std::uint32_t>(i + 1);
        }
      }
    } else {
      scene.spheres.push_back({anchors[id], 0.008 * size, c});
    }
  }
  const IndexedMesh colored = splitByGroup(mesh, groups, colors, scene.vertex_colors);
  scene.mesh = &colored;
  // Element triangles by id, for anchors on the side that faces the camera.
  std::map<std::string, std::size_t> index_of;
  for (std::size_t i = 0; i < elements.size(); ++i) {
    index_of[elements[i].at("id").get<std::string>()] = i;
  }
  const auto anchor_facing = [&](const std::string& id, const Vec& towards) -> std::optional<Vec> {
    const std::size_t i = index_of.at(id);
    if (i >= measurement.element_triangles.size() || measurement.element_triangles[i].empty()) {
      return anchors.at(id);
    }
    // The middle of the element's triangles that face the camera, moved onto the nearest of
    // them.
    Vec sum{};
    double weight = 0.0;
    std::vector<std::pair<Vec, double>> facing;
    for (const std::uint32_t t : measurement.element_triangles[i]) {
      const auto& tri = mesh.triangles[t];
      const Vec a = toVec(mesh.points[tri[0]]);
      const Vec b = toVec(mesh.points[tri[1]]);
      const Vec c = toVec(mesh.points[tri[2]]);
      const Vec n = cross(b - a, c - a);
      const double area = 0.5 * norm(n);
      if (!(area > 0.0) || dot(normalized(n), towards) < 0.25) {
        continue;
      }
      const Vec middle = (1.0 / 3.0) * (a + b + c);
      sum = sum + area * middle;
      weight += area;
      facing.emplace_back(middle, area);
    }
    if (facing.empty()) {
      return std::nullopt;
    }
    // Inside a hole the middle of the far wall is hidden behind the near one; its rim is not.
    const Json& element = elements[i];
    const bool hole = element.at("nominal").value("inner", false);
    Vec target = (1.0 / weight) * sum;
    if (hole) {
      for (const auto& [p, area] : facing) {
        if (dot(p, towards) > dot(target, towards)) {
          target = p;
        }
      }
    }
    Vec best = facing.front().first;
    for (const auto& [p, area] : facing) {
      if (norm(p - target) < norm(best - target)) {
        best = p;
      }
    }
    return best;
  };
  std::vector<std::string> labels;
  std::vector<std::string> anchor_ids;
  for (const Json& c : characteristics) {
    anchor_ids.push_back(c.at("elements").back().get<std::string>());
    labels.push_back(c.at("number").get<std::string>());
  }
  // A balloon goes into the views where its element faces the camera, or into the first view
  // when it faces none.
  std::vector<std::vector<std::optional<Vec>>> view_anchors;
  for (const auto& direction : kDeviationViews) {
    const double azimuth = radians(direction[0]);
    const double elevation = radians(direction[1]);
    const Vec towards{std::cos(elevation) * std::cos(azimuth),
                      std::cos(elevation) * std::sin(azimuth), std::sin(elevation)};
    std::vector<std::optional<Vec>> found;
    found.reserve(anchor_ids.size());
    for (const std::string& id : anchor_ids) {
      found.push_back(anchor_facing(id, towards));
    }
    view_anchors.push_back(std::move(found));
  }
  for (std::size_t i = 0; i < anchor_ids.size(); ++i) {
    if (std::none_of(view_anchors.begin(), view_anchors.end(),
                     [i](const auto& v) { return v[i].has_value(); })) {
      view_anchors[0][i] = anchors.at(anchor_ids[i]);
    }
  }
  for (std::size_t k = 0; k < kDeviationViews.size(); ++k) {
    RenderView view;
    view.azimuth_degrees = kDeviationViews[k][0];
    view.elevation_degrees = kDeviationViews[k][1];
    view.width = 1200;
    view.height = 900;
    view.margin = 0.12;
    std::vector<Vec> balloon_anchors;
    std::vector<std::string> shown;
    for (std::size_t i = 0; i < labels.size(); ++i) {
      if (view_anchors[k][i]) {
        balloon_anchors.push_back(*view_anchors[k][i]);
        shown.push_back(labels[i]);
      }
    }
    RenderImage image = render(scene, view);
    const auto projected = projectToImage(scene, view, balloon_anchors);
    const auto placed = placeBalloons(projected, view.width, view.height, 24.0);
    for (std::size_t i = 0; i < shown.size(); ++i) {
      drawBalloon(image, placed[i][0], placed[i][1], projected[i][0], projected[i][1], shown[i], 3);
    }
    writePng(image, dir / ("measurement_view_" + std::to_string(k + 1) + ".png"));
  }
}

}  // namespace voxelsieve
