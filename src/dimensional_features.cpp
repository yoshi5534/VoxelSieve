// Elements found on the CAD model: FeatureFinder and the image of its features.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "detail/dimensional.hpp"

namespace voxelsieve::detail::dimensional {
namespace {

/// Unsigned distance of `p` to the element's surface, ignoring the side.
double surfaceDistance(const GeometricElement& e, const Vec& p) {
  return std::abs(elementDistance(e, p));
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

}  // namespace

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

}  // namespace voxelsieve::detail::dimensional

namespace voxelsieve {

using namespace detail::dimensional;

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

}  // namespace voxelsieve
