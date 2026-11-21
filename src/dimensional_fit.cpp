// Fits of geometric elements to surface points and the geometry of elements.

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "detail/dimensional.hpp"

namespace voxelsieve::detail::dimensional {
namespace {

Vec centroid(std::span<const SurfacePoint> points) {
  Vec sum{};
  for (const SurfacePoint& p : points) {
    sum = sum + p.position;
  }
  return (1.0 / static_cast<double>(std::max<std::size_t>(points.size(), 1))) * sum;
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

/// Distance of `p` from the axis of a cylinder.
double axisDistance(const GeometricElement& e, const Vec& p) {
  const Vec w = p - e.point;
  return norm(w - dot(w, e.direction) * e.direction);
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

}  // namespace voxelsieve::detail::dimensional

namespace voxelsieve {

using namespace detail::dimensional;

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
  if (clamped.voxelCount() == 0) {
    return {};
  }
  const std::int64_t step = std::max(stride, 1);
  std::array<float, 256> decoded{};
  for (int c = 0; c <= info.maxCode(); ++c) {
    decoded[static_cast<std::size_t>(c)] =
        decodeSurfaceDistance(static_cast<std::uint8_t>(c), info.bits, info.band_voxels);
  }
  // Slabs along z of whole sample planes, in parallel; each is read with a margin of one voxel
  // below and a step and two above, for the crossings to the next sample and the differences.
  const std::int64_t planes_per_slab = std::max<std::int64_t>(1, 32 / step);
  const std::int64_t samples_z = (clamped.size(2) + step - 1) / step;
  const std::int64_t slabs = (samples_z + planes_per_slab - 1) / planes_per_slab;
  std::vector<std::vector<SurfacePoint>> found(static_cast<std::size_t>(slabs));
  tbb::parallel_for(std::int64_t{0}, slabs, [&](std::int64_t slab) {
    const std::int64_t z0 = clamped.min[2] + slab * planes_per_slab * step;
    const std::int64_t z1 = std::min(z0 + planes_per_slab * step, clamped.max[2]);
    Box read = clamped;
    read.min[2] = z0;
    read.max[2] = z1;
    for (std::size_t a = 0; a < 3; ++a) {
      read.min[a] = std::max<std::int64_t>(read.min[a] - 1, 0);
      read.max[a] = std::min<std::int64_t>(read.max[a] + step + 1, info.dims[a]);
    }
    std::vector<std::uint8_t> codes(static_cast<std::size_t>(read.voxelCount()));
    mask.readCodes(read, codes);
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
    std::vector<SurfacePoint>& points = found[static_cast<std::size_t>(slab)];
    for (std::int64_t z = z0; z < z1; z += step) {
      for (std::int64_t y = clamped.min[1]; y < clamped.max[1]; y += step) {
        for (std::int64_t x = clamped.min[0]; x < clamped.max[0]; x += step) {
          for (std::size_t axis = 0; axis < 3; ++axis) {
            // Along the grid line to the next sample, so that a surface between two samples, such
            // as a plane normal to the axis, is found as well: the first crossing on the way.
            std::array<std::int64_t, 3> p{x, y, z};
            const std::int64_t end = std::min(p[axis] + step, info.dims[axis] - 1);
            double d0 = at(p[0], p[1], p[2]);
            for (; p[axis] < end; ++p[axis]) {
              std::array<std::int64_t, 3> next = p;
              ++next[axis];
              const double d1 = at(next[0], next[1], next[2]);
              if ((d0 < 0.0) == (d1 < 0.0)) {
                d0 = d1;
                continue;
              }
              const double t = d0 / (d0 - d1);
              Vec position{static_cast<double>(p[0]), static_cast<double>(p[1]),
                           static_cast<double>(p[2])};
              position[axis] += t;
              for (std::size_t k = 0; k < 3; ++k) {
                position[k] *= v[k];
              }
              const Vec g =
                  (1.0 - t) * gradient(p[0], p[1], p[2]) + t * gradient(next[0], next[1], next[2]);
              points.push_back({position, normalized(g)});
              break;
            }
          }
        }
      }
    }
  });
  std::vector<SurfacePoint> points;
  for (const auto& part : found) {
    points.insert(points.end(), part.begin(), part.end());
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

}  // namespace voxelsieve
