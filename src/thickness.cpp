#include "voxelsieve/thickness.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tinyply.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "detail/thickness.hpp"
#include "voxelsieve/compare.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/render.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {
namespace {

using Vec = std::array<double, 3>;
using Json = nlohmann::json;
using Method = WallThicknessOptions::Method;

constexpr float kNotMeasured = std::numeric_limits<float>::quiet_NaN();

Vec operator+(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec operator-(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec operator*(double s, const Vec& a) { return {s * a[0], s * a[1], s * a[2]}; }
double dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double norm(const Vec& a) { return std::sqrt(dot(a, a)); }
Vec normalized(const Vec& a) {
  const double n = norm(a);
  return n > 0.0 ? (1.0 / n) * a : Vec{0.0, 0.0, 0.0};
}
Vec toVec(const std::array<float, 3>& p) { return {p[0], p[1], p[2]}; }

/// Grey values of level 0 at positions in mm, trilinear between the voxel centres; removed
/// outside air and positions outside the volume read as the air level. For one thread.
class GreySampler {
 public:
  GreySampler(const Dataset& dataset, const VoxelSize& voxel_size)
      : sampler_(dataset, 0), air_(dataset.info().air_level), voxel_size_(voxel_size) {}

  double operator()(const Vec& mm) {
    const Vec u = voxel_size_.toVoxels(mm);
    const std::array<double, 3> floor{std::floor(u[0]), std::floor(u[1]), std::floor(u[2])};
    const Vec f = u - floor;
    const std::array<std::int64_t, 3> base{static_cast<std::int64_t>(floor[0]),
                                           static_cast<std::int64_t>(floor[1]),
                                           static_cast<std::int64_t>(floor[2])};
    double value = 0.0;
    for (int corner = 0; corner < 8; ++corner) {
      double weight = 1.0;
      std::array<std::int64_t, 3> voxel = base;
      for (std::size_t k = 0; k < 3; ++k) {
        const bool high = ((corner >> k) & 1) != 0;
        voxel[k] += high ? 1 : 0;
        weight *= high ? f[k] : 1.0 - f[k];
      }
      if (weight > 0.0) {
        value += weight * grey(voxel);
      }
    }
    return value;
  }

  /// Gradient per mm by central differences one voxel apart along each axis.
  Vec gradient(const Vec& mm) {
    Vec g{};
    for (std::size_t k = 0; k < 3; ++k) {
      Vec step{};
      step[k] = voxel_size_[k];
      g[k] = ((*this)(mm + step) - (*this)(mm - step)) / (2.0 * voxel_size_[k]);
    }
    return g;
  }

 private:
  double grey(const std::array<std::int64_t, 3>& voxel) {
    const std::optional<float> value = sampler_(voxel);
    return value ? *value : air_;
  }

  Dataset::Sampler sampler_;
  double air_;
  VoxelSize voxel_size_;
};

/// Nearest-neighbour search over the vertices of the surface, a k-d tree with the points in tree
/// order.
class PointTree {
 public:
  explicit PointTree(const std::vector<std::array<float, 3>>& points) {
    index_.resize(points.size());
    std::iota(index_.begin(), index_.end(), 0U);
    points_.reserve(points.size());
    for (const auto& p : points) {
      points_.push_back(toVec(p));
    }
    axis_.assign(points.size(), 0);
    build(0, points.size());
    ordered_.reserve(points.size());
    for (const std::uint32_t i : index_) {
      ordered_.push_back(points_[i]);
    }
  }

  /// The point closest to `q` other than point `skip`, and its squared distance.
  [[nodiscard]] std::pair<std::uint32_t, double> nearest(const Vec& q, std::uint32_t skip) const {
    std::pair<std::uint32_t, double> best{std::numeric_limits<std::uint32_t>::max(),
                                          std::numeric_limits<double>::infinity()};
    search(0, ordered_.size(), q, skip, best);
    return best;
  }

  [[nodiscard]] const Vec& point(std::uint32_t i) const { return points_[i]; }

 private:
  static constexpr std::size_t kLeaf = 8;

  void build(std::size_t lo, std::size_t hi) {
    if (hi - lo <= kLeaf) {
      return;
    }
    Vec low{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity()};
    Vec high = -1.0 * low;
    for (std::size_t i = lo; i < hi; ++i) {
      for (std::size_t k = 0; k < 3; ++k) {
        low[k] = std::min(low[k], points_[index_[i]][k]);
        high[k] = std::max(high[k], points_[index_[i]][k]);
      }
    }
    const Vec extent = high - low;
    const std::size_t axis = extent[0] >= extent[1] && extent[0] >= extent[2] ? 0
                             : extent[1] >= extent[2]                         ? 1
                                                                              : 2;
    const std::size_t mid = (lo + hi) / 2;
    std::nth_element(index_.begin() + static_cast<std::ptrdiff_t>(lo),
                     index_.begin() + static_cast<std::ptrdiff_t>(mid),
                     index_.begin() + static_cast<std::ptrdiff_t>(hi),
                     [this, axis](std::uint32_t a, std::uint32_t b) {
                       return points_[a][axis] < points_[b][axis];
                     });
    axis_[mid] = static_cast<std::uint8_t>(axis);
    build(lo, mid);
    build(mid, hi);
  }

  void search(std::size_t lo, std::size_t hi, const Vec& q, std::uint32_t skip,
              std::pair<std::uint32_t, double>& best) const {
    if (hi - lo <= kLeaf) {
      for (std::size_t i = lo; i < hi; ++i) {
        if (index_[i] == skip) {
          continue;
        }
        const Vec d = ordered_[i] - q;
        const double d2 = dot(d, d);
        if (d2 < best.second) {
          best = {index_[i], d2};
        }
      }
      return;
    }
    const std::size_t mid = (lo + hi) / 2;
    const std::size_t axis = axis_[mid];
    const double delta = q[axis] - ordered_[mid][axis];
    // Points left of mid are not above the split value, points from mid on not below it.
    if (delta < 0.0) {
      search(lo, mid, q, skip, best);
      if (delta * delta < best.second) {
        search(mid, hi, q, skip, best);
      }
    } else {
      search(mid, hi, q, skip, best);
      if (delta * delta < best.second) {
        search(lo, mid, q, skip, best);
      }
    }
  }

  std::vector<std::uint32_t> index_;
  std::vector<Vec> points_;   // by vertex index
  std::vector<Vec> ordered_;  // in tree order
  std::vector<std::uint8_t> axis_;
};

/// Area of every triangle, and a third of it for each of its corners.
std::vector<double> vertexAreas(const IndexedMesh& mesh) {
  std::vector<double> area(mesh.points.size(), 0.0);
  for (const auto& t : mesh.triangles) {
    const Vec a = toVec(mesh.points[t[0]]);
    const double third =
        norm(cross(toVec(mesh.points[t[1]]) - a, toVec(mesh.points[t[2]]) - a)) / 6.0;
    for (const std::uint32_t i : t) {
      area[i] += third;
    }
  }
  return area;
}

/// Area-weighted vertex normals, averaged once more over the neighbours of every vertex so that
/// the voxel staircase of the surface does not tilt the rays.
std::vector<Vec> smoothNormals(const IndexedMesh& mesh) {
  std::vector<Vec> normals(mesh.points.size(), Vec{});
  for (const auto& t : mesh.triangles) {
    const Vec a = toVec(mesh.points[t[0]]);
    const Vec n = cross(toVec(mesh.points[t[1]]) - a, toVec(mesh.points[t[2]]) - a);
    for (const std::uint32_t i : t) {
      normals[i] = normals[i] + n;
    }
  }
  for (Vec& n : normals) {
    n = normalized(n);
  }
  std::vector<Vec> smoothed = normals;
  for (const auto& t : mesh.triangles) {
    const Vec sum = normals[t[0]] + normals[t[1]] + normals[t[2]];
    for (const std::uint32_t i : t) {
      smoothed[i] = smoothed[i] + sum;
    }
  }
  for (Vec& n : smoothed) {
    n = normalized(n);
  }
  return smoothed;
}

/// Connected surfaces: a component id per vertex (union-find over the triangles).
std::vector<std::uint32_t> vertexComponents(const IndexedMesh& mesh) {
  std::vector<std::uint32_t> parent(mesh.points.size());
  std::iota(parent.begin(), parent.end(), 0U);
  const auto find = [&parent](std::uint32_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  for (const auto& t : mesh.triangles) {
    const std::uint32_t a = find(t[0]);
    for (std::size_t k = 1; k < 3; ++k) {
      const std::uint32_t b = find(t[k]);
      if (a != b) {
        parent[b] = a;
      }
    }
  }
  for (std::uint32_t i = 0; i < parent.size(); ++i) {
    parent[i] = find(i);
  }
  return parent;
}

struct RaySettings {
  float iso = 0.0F;
  double max_mm = 0.0;
  double step_mm = 0.0;    // half the smallest pitch
  double cos_limit = 0.0;  // of the search angle
};

/// Thickness along the ray from surface point `p` into the material against its outward normal
/// `n`, or NaN.
float rayThickness(GreySampler& grey, Vec p, const Vec& n, const RaySettings& s) {
  const double iso = s.iso;
  // Move the start onto the surface of the grey values: the crossing from material to air along
  // the normal closest to the vertex.
  const double fine = 0.5 * s.step_mm;
  std::optional<double> start;
  double previous = grey(p - (4.0 * fine) * n);
  for (int k = -3; k <= 4; ++k) {
    const double offset = k * fine;
    const double g = grey(p + offset * n);
    if (previous > iso && g <= iso) {
      const double crossing = offset - fine * (iso - g) / (previous - g);
      if (!start || std::abs(crossing) < std::abs(*start)) {
        start = crossing;
      }
    }
    previous = g;
  }
  if (start) {
    p = p + *start * n;
  }
  // March inward to where the grey values fall below the surface value and stay there for one
  // more step, so a noisy voxel inside the wall does not end it.
  const auto at = [&](double distance) { return grey(p - distance * n); };
  double before = at(s.step_mm);
  if (before <= iso) {
    return kNotMeasured;
  }
  for (double distance = 2.0 * s.step_mm; distance <= s.max_mm + s.step_mm; distance += s.step_mm) {
    const double g = at(distance);
    if (g < iso && at(distance + s.step_mm) < iso) {
      const double exit = distance - s.step_mm * (iso - g) / (before - g);
      if (exit > s.max_mm) {
        return kNotMeasured;
      }
      // The face the ray leaves through must face along the ray: the outward normal there is
      // against the grey value gradient.
      const Vec outward = normalized(-1.0 * grey.gradient(p - exit * n));
      if (dot(outward, -1.0 * n) < s.cos_limit) {
        return kNotMeasured;
      }
      return static_cast<float>(exit);
    }
    before = g;
  }
  return kNotMeasured;
}

/// Diameter of the largest ball inside the material that touches the surface at vertex `i` with
/// outward normal `n`, by shrinking a ball of radius max/2 until no other surface point lies in
/// it (Ma, Bae, Oude Elberink 2012), or NaN when it does not shrink.
float sphereThickness(const PointTree& tree, std::uint32_t i, const Vec& n, double max_mm,
                      double eps_mm) {
  const Vec p = tree.point(i);
  double r = 0.5 * max_mm;
  bool shrunk = false;
  // A point next to p that lies a little inside the ball only by the noise of the surface would
  // shrink it to nothing; such points see p and themselves under a small angle from the centre.
  const double min_separation = std::cos(std::numbers::pi / 6.0);
  for (int iteration = 0; iteration < 64; ++iteration) {
    const Vec c = p - r * n;
    const auto [q_index, d2] = tree.nearest(c, i);
    if (q_index == std::numeric_limits<std::uint32_t>::max() || d2 >= (r - eps_mm) * (r - eps_mm)) {
      break;
    }
    const Vec q = tree.point(q_index);
    const double along = dot(n, p - q);
    if (along <= 0.0) {
      break;
    }
    const double radius = dot(p - q, p - q) / (2.0 * along);
    if (radius >= r) {
      break;
    }
    const Vec centre = p - radius * n;
    if (dot(p - centre, q - centre) > min_separation * radius * radius) {
      break;
    }
    r = radius;
    shrunk = true;
  }
  return shrunk ? static_cast<float>(2.0 * r) : kNotMeasured;
}

/// The nominal geometry for addNominalThickness: distances and the outward normal of every
/// triangle.
struct NominalModel {
  const MeshDistance& distance;
  std::vector<Vec> normals;
  double max_mm = 0.0;
  double cos_limit = 0.0;
  double eps = 0.0;

  /// The outward normal of the triangle a query found.
  [[nodiscard]] const Vec& normal(const MeshDistance::Hit& hit) const {
    return normals[hit.triangle];
  }
};

/// Thickness of the model from its surface point `x` with outward normal `n` along -n to the
/// opposite face, by sphere tracing through the distance to the model; NaN as for rayThickness.
float nominalRayThickness(const NominalModel& model, const Vec& x, const Vec& n) {
  constexpr int kSteps = 512;
  const Vec dir = -1.0 * n;
  double t = 4.0 * model.eps;
  for (int step = 0; step < kSteps && t <= model.max_mm; ++step) {
    const MeshDistance::Hit hit = model.distance.query(x + t * dir);
    // Inside, the distance is negative; nearly zero or positive at the opposite face.
    const double r = -hit.distance;
    if (r < model.eps) {
      if (dot(model.normal(hit), dir) < model.cos_limit) {
        return kNotMeasured;
      }
      return static_cast<float>(t + r);
    }
    t += r;
  }
  return kNotMeasured;
}

/// Diameter of the largest ball inside the model that touches it at `x` with outward normal
/// `n`, by shrinking as sphereThickness against the exact distance to the model, with the same
/// rule for points close to `x`, so that edges read as they do on the scan.
float nominalSphereThickness(const NominalModel& model, const Vec& x, const Vec& n) {
  double r = 0.5 * model.max_mm;
  bool shrunk = false;
  const double min_separation = std::cos(std::numbers::pi / 6.0);
  for (int iteration = 0; iteration < 64; ++iteration) {
    const MeshDistance::Hit hit = model.distance.query(x - r * n);
    if (std::abs(hit.distance) >= r - model.eps) {
      break;
    }
    // The closest point lies inside the ball: shrink it to touch both x and that point.
    const Vec w = x - Vec{hit.closest[0], hit.closest[1], hit.closest[2]};
    const double along = dot(n, w);
    if (along <= model.eps * model.eps) {
      break;
    }
    const double next = 0.5 * dot(w, w) / along;
    if (next >= r - model.eps) {
      break;
    }
    const Vec centre = x - next * n;
    const Vec y = x - w;
    if (dot(x - centre, y - centre) > min_separation * next * next) {
      break;
    }
    r = next;
    shrunk = true;
  }
  return shrunk ? static_cast<float>(2.0 * r) : kNotMeasured;
}

ThicknessStats statsOf(const IndexedMesh& mesh, const std::vector<float>& thickness,
                       double min_wall, double max_wall) {
  constexpr int kBins = 40;
  ThicknessStats s;
  s.vertices = mesh.points.size();
  const std::vector<double> weight = vertexAreas(mesh);
  std::vector<std::uint32_t> measured;
  for (std::uint32_t i = 0; i < thickness.size(); ++i) {
    s.area_mm2 += weight[i];
    if (std::isfinite(thickness[i])) {
      measured.push_back(i);
      s.measured_area_mm2 += weight[i];
    }
  }
  s.histogram.assign(kBins, 0.0);
  if (s.measured_area_mm2 <= 0.0) {
    return s;
  }
  std::sort(measured.begin(), measured.end(),
            [&thickness](std::uint32_t a, std::uint32_t b) { return thickness[a] < thickness[b]; });
  s.min_mm = thickness[measured.front()];
  s.max_mm = thickness[measured.back()];
  double sum = 0.0;
  double sum2 = 0.0;
  for (const std::uint32_t i : measured) {
    const double t = thickness[i];
    sum += weight[i] * t;
    sum2 += weight[i] * t * t;
    if (min_wall > 0.0 && t < min_wall) {
      s.below_min += weight[i];
    }
    if (max_wall > 0.0 && t > max_wall) {
      s.above_max += weight[i];
    }
  }
  s.mean_mm = sum / s.measured_area_mm2;
  s.std_mm = std::sqrt(std::max(0.0, sum2 / s.measured_area_mm2 - s.mean_mm * s.mean_mm));
  s.below_min /= s.measured_area_mm2;
  s.above_max /= s.measured_area_mm2;
  double cumulative = 0.0;
  std::size_t next = 0;
  for (const std::uint32_t i : measured) {
    cumulative += weight[i];
    while (next < kThicknessPercentiles.size() &&
           cumulative >= kThicknessPercentiles[next] / 100.0 * s.measured_area_mm2) {
      s.percentiles_mm[next++] = thickness[i];
    }
  }
  for (; next < kThicknessPercentiles.size(); ++next) {
    s.percentiles_mm[next] = s.max_mm;
  }
  s.range_mm = detail::niceCeil(std::max({s.percentiles_mm[4], 1.25 * min_wall, max_wall}));
  for (const std::uint32_t i : measured) {
    const int bin =
        std::clamp(static_cast<int>(std::floor(thickness[i] / s.range_mm * kBins)), 0, kBins - 1);
    s.histogram[static_cast<std::size_t>(bin)] += weight[i] / s.measured_area_mm2;
  }
  return s;
}

/// Connected regions of the vertices that `flagged` selects, the extreme value by `more_extreme`.
template <class Flagged, class MoreExtreme>
std::vector<ThicknessRegion> regionsOf(const IndexedMesh& mesh, const std::vector<float>& thickness,
                                       Flagged flagged, MoreExtreme more_extreme) {
  std::vector<std::uint32_t> parent(mesh.points.size());
  std::iota(parent.begin(), parent.end(), 0U);
  const auto find = [&parent](std::uint32_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  for (const auto& t : mesh.triangles) {
    for (std::size_t k = 0; k < 3; ++k) {
      const std::uint32_t a = t[k];
      const std::uint32_t b = t[(k + 1) % 3];
      if (flagged(thickness[a]) && flagged(thickness[b])) {
        parent[find(a)] = find(b);
      }
    }
  }
  const std::vector<double> weight = vertexAreas(mesh);
  std::unordered_map<std::uint32_t, std::size_t> by_root;
  std::vector<ThicknessRegion> found;
  for (std::uint32_t i = 0; i < mesh.points.size(); ++i) {
    if (!flagged(thickness[i])) {
      continue;
    }
    const auto [it, added] = by_root.try_emplace(find(i), found.size());
    const Vec p = toVec(mesh.points[i]);
    if (added) {
      ThicknessRegion region;
      region.extreme_mm = thickness[i];
      region.location_mm = p;
      region.bounds_mm = {p, p};
      found.push_back(region);
    }
    ThicknessRegion& region = found[it->second];
    ++region.vertices;
    region.area_mm2 += weight[i];
    if (more_extreme(thickness[i], region.extreme_mm)) {
      region.extreme_mm = thickness[i];
      region.location_mm = p;
    }
    for (std::size_t k = 0; k < 3; ++k) {
      region.bounds_mm[0][k] = std::min(region.bounds_mm[0][k], p[k]);
      region.bounds_mm[1][k] = std::max(region.bounds_mm[1][k], p[k]);
    }
  }
  std::sort(found.begin(), found.end(),
            [&more_extreme](const ThicknessRegion& a, const ThicknessRegion& b) {
              if (a.extreme_mm != b.extreme_mm) {
                return more_extreme(a.extreme_mm, b.extreme_mm);
              }
              return a.area_mm2 > b.area_mm2;
            });
  for (std::size_t i = 0; i < found.size(); ++i) {
    found[i].id = static_cast<int>(i + 1);
  }
  return found;
}

Json regionsJson(const std::vector<ThicknessRegion>& regions) {
  Json list = Json::array();
  for (std::size_t i = 0; i < std::min(regions.size(), detail::kListedRegions); ++i) {
    const ThicknessRegion& r = regions[i];
    list.push_back({{"id", r.id},
                    {"thickness_mm", r.extreme_mm},
                    {"area_mm2", r.area_mm2},
                    {"vertices", r.vertices},
                    {"location_mm", r.location_mm},
                    {"bounds_mm", r.bounds_mm}});
  }
  return list;
}

void writePly(const std::filesystem::path& file, const WallThicknessResult& result) {
  const std::size_t n = result.mesh.points.size();
  std::vector<std::uint8_t> colors;
  colors.reserve(n * 3);
  for (std::size_t i = 0; i < n; ++i) {
    for (const std::uint8_t c : thicknessColor(result.thickness_mm[i], result.min_wall_mm,
                                               result.max_wall_mm, result.stats.range_mm)) {
      colors.push_back(c);
    }
  }
  std::vector<std::int32_t> faces;
  faces.reserve(result.mesh.triangles.size() * 3);
  for (const auto& t : result.mesh.triangles) {
    for (const std::uint32_t i : t) {
      faces.push_back(static_cast<std::int32_t>(i));
    }
  }
  // tinyply only reads the buffers when writing, but takes them as non-const bytes.
  const auto bytes = [](const auto* data) {
    using T = std::remove_cvref_t<decltype(*data)>;
    return reinterpret_cast<std::uint8_t*>(const_cast<T*>(data));
  };
  tinyply::PlyFile ply;
  ply.get_comments().emplace_back(
      "VoxelSieve wall thickness: measured surface in mm, wall thickness in mm per vertex (nan "
      "where none was found)");
  ply.add_properties_to_element("vertex", {"x", "y", "z"}, tinyply::Type::FLOAT32, n,
                                bytes(result.mesh.points.data()), tinyply::Type::INVALID, 0);
  ply.add_properties_to_element("vertex", {"thickness"}, tinyply::Type::FLOAT32, n,
                                bytes(result.thickness_mm.data()), tinyply::Type::INVALID, 0);
  if (result.nominal) {
    ply.get_comments().emplace_back(
        "nominal: thickness of the nominal model in mm, deviation: thickness minus nominal");
    ply.add_properties_to_element("vertex", {"nominal"}, tinyply::Type::FLOAT32, n,
                                  bytes(result.nominal->nominal_mm.data()), tinyply::Type::INVALID,
                                  0);
    ply.add_properties_to_element("vertex", {"deviation"}, tinyply::Type::FLOAT32, n,
                                  bytes(result.nominal->deviation_mm.data()),
                                  tinyply::Type::INVALID, 0);
  }
  ply.add_properties_to_element("vertex", {"red", "green", "blue"}, tinyply::Type::UINT8, n,
                                colors.data(), tinyply::Type::INVALID, 0);
  ply.add_properties_to_element(
      "face", {"vertex_indices"}, tinyply::Type::INT32, result.mesh.triangles.size(),
      reinterpret_cast<std::uint8_t*>(faces.data()), tinyply::Type::UINT8, 3);
  const std::filesystem::path partial = file.string() + ".partial";
  {
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    ply.write(out, true);
    if (!out) {
      throw std::runtime_error("Cannot write " + file.string());
    }
  }
  std::filesystem::rename(partial, file);
}

}  // namespace

const char* toString(WallThicknessOptions::Method method) {
  return method == Method::kSphere ? "sphere" : "ray";
}

WallThicknessOptions::Method thicknessMethodFromString(const std::string& name) {
  if (name == "ray") {
    return Method::kRay;
  }
  if (name == "sphere") {
    return Method::kSphere;
  }
  throw std::invalid_argument("Unknown wall thickness method '" + name + "' (ray or sphere)");
}

std::array<std::uint8_t, 3> thicknessColor(double thickness_mm, double min_wall_mm,
                                           double max_wall_mm, double range_mm) {
  if (!std::isfinite(thickness_mm)) {
    return {150, 150, 150};
  }
  if (min_wall_mm > 0.0 && thickness_mm < min_wall_mm) {
    return {200, 40, 170};
  }
  if (max_wall_mm > 0.0 && thickness_mm > max_wall_mm) {
    return {110, 50, 170};
  }
  // Red, yellow, green, cyan, blue at even steps from 0 to the range.
  constexpr std::array<std::array<double, 3>, 5> kStops{
      {{215, 40, 40}, {240, 220, 50}, {60, 190, 90}, {40, 190, 230}, {40, 70, 210}}};
  const double t = std::clamp(thickness_mm / std::max(range_mm, 1e-12), 0.0, 1.0) * 4.0;
  const auto segment = std::min(static_cast<std::size_t>(t), std::size_t{3});
  const double f = t - static_cast<double>(segment);
  std::array<std::uint8_t, 3> color{};
  for (std::size_t c = 0; c < 3; ++c) {
    color[c] = static_cast<std::uint8_t>(
        std::lround(kStops[segment][c] + (kStops[segment + 1][c] - kStops[segment][c]) * f));
  }
  return color;
}

WallThicknessResult analyzeWallThickness(const Dataset& dataset, const SurfaceMask& surface,
                                         const WallThicknessOptions& options) {
  if (options.max_thickness_mm < 0.0 || options.min_wall_mm < 0.0 || options.max_wall_mm < 0.0) {
    throw std::invalid_argument("Wall thickness limits must not be negative");
  }
  if (options.min_wall_mm > 0.0 && options.max_wall_mm > 0.0 &&
      options.min_wall_mm >= options.max_wall_mm) {
    throw std::invalid_argument("The minimum wall thickness must be below the maximum");
  }
  if (!(options.search_angle_deg > 0.0 && options.search_angle_deg <= 90.0)) {
    throw std::invalid_argument("The search angle must lie in (0, 90] degrees");
  }
  const SurfaceInfo& info = surface.info();
  if (info.dims != dataset.info().dims) {
    throw std::invalid_argument("The surface does not belong to the dataset");
  }
  WallThicknessResult result;
  result.method = options.method;
  result.voxel_size = dataset.info().voxel_size;
  result.iso_value = info.iso_value;
  result.search_angle_deg = options.search_angle_deg;
  result.min_wall_mm = options.min_wall_mm;
  result.max_wall_mm = options.max_wall_mm;
  const VoxelSize& v = result.voxel_size;

  std::optional<TelemetryPhase> phase(std::in_place, "surface mesh");
  // Every surface, so that pores limit the walls; values only where they are reported.
  ScanSurface scanned = scanSurface(surface, options.max_triangles, false);
  IndexedMesh& mesh = scanned.mesh;
  const std::vector<std::uint32_t> component = vertexComponents(mesh);
  std::unordered_map<std::uint32_t, double> component_area;
  const std::vector<double> area = vertexAreas(mesh);
  for (std::uint32_t i = 0; i < mesh.points.size(); ++i) {
    component_area[component[i]] += area[i];
  }
  result.components = component_area.size();
  std::uint32_t outer = 0;
  double outer_area = -1.0;
  for (const auto& [root, a] : component_area) {
    if (a > outer_area || (a == outer_area && root < outer)) {
      outer = root;
      outer_area = a;
    }
  }
  const auto reported = [&](std::uint32_t i) {
    return !options.outer_surface_only || component[i] == outer;
  };
  result.dropped_components = options.outer_surface_only ? result.components - 1 : 0;

  Vec low{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::infinity()};
  Vec high = -1.0 * low;
  for (const auto& p : mesh.points) {
    for (std::size_t k = 0; k < 3; ++k) {
      low[k] = std::min(low[k], static_cast<double>(p[k]));
      high[k] = std::max(high[k], static_cast<double>(p[k]));
    }
  }
  std::array<double, 3> extent{high[0] - low[0], high[1] - low[1], high[2] - low[2]};
  std::sort(extent.begin(), extent.end());
  result.max_thickness_mm =
      options.max_thickness_mm > 0.0 ? options.max_thickness_mm : 0.5 * extent[1];

  std::vector<Vec> normals = smoothNormals(mesh);
  {
    // Outward: against the grey value gradient. Decided by the majority of a sample of vertices,
    // so that the orientation of the mesher does not matter.
    GreySampler grey(dataset, v);
    double agreement = 0.0;
    const std::size_t stride = std::max<std::size_t>(1, mesh.points.size() / 2000);
    for (std::size_t i = 0; i < mesh.points.size(); i += stride) {
      agreement += dot(normals[i], -1.0 * normalized(grey.gradient(toVec(mesh.points[i]))));
    }
    if (agreement < 0.0) {
      for (Vec& n : normals) {
        n = -1.0 * n;
      }
    }
  }

  phase.reset();
  phase.emplace("thickness");
  std::vector<std::uint32_t> order;
  for (std::uint32_t i = 0; i < mesh.points.size(); ++i) {
    if (reported(i)) {
      order.push_back(i);
    }
  }
  // Vertices of one brick after the other, so that the rays of a task read the same bricks.
  const auto brick_key = [&](std::uint32_t i) {
    const auto brick = dataset.info().brick_size;
    std::array<std::int64_t, 3> b{};
    for (std::size_t k = 0; k < 3; ++k) {
      b[k] = static_cast<std::int64_t>(std::floor(mesh.points[i][k] / v[k])) / brick;
    }
    return b;
  };
  std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
    const auto ka = brick_key(a);
    const auto kb = brick_key(b);
    return std::tie(ka[2], ka[1], ka[0], a) < std::tie(kb[2], kb[1], kb[0], b);
  });
  std::vector<float> thickness(mesh.points.size(), kNotMeasured);
  std::unique_ptr<PointTree> tree;
  if (options.method == Method::kSphere) {
    tree = std::make_unique<PointTree>(mesh.points);
  }
  RaySettings ray;
  ray.iso = info.iso_value;
  ray.max_mm = result.max_thickness_mm;
  ray.step_mm = 0.5 * v.minMm();
  ray.cos_limit = std::cos(options.search_angle_deg * std::numbers::pi / 180.0);
  const double eps = 1e-3 * v.minMm();
  std::atomic<std::size_t> done{0};
  std::mutex progress_mutex;
  int reported_percent = -1;
  tbb::parallel_for(
      tbb::blocked_range<std::size_t>(0, order.size(), 512),
      [&](const tbb::blocked_range<std::size_t>& range) {
        GreySampler grey(dataset, v);
        for (std::size_t r = range.begin(); r != range.end(); ++r) {
          const std::uint32_t i = order[r];
          thickness[i] = options.method == Method::kSphere
                             ? sphereThickness(*tree, i, normals[i], result.max_thickness_mm, eps)
                             : rayThickness(grey, toVec(mesh.points[i]), normals[i], ray);
        }
        const std::size_t now = done += range.size();
        if (options.progress) {
          const std::scoped_lock lock(progress_mutex);
          const auto percent = static_cast<int>(100 * now / std::max<std::size_t>(order.size(), 1));
          if (percent > reported_percent) {
            reported_percent = percent;
            options.progress(percent / 100.0);
          }
        }
      });

  phase.reset();
  phase.emplace("regions");
  // The reported surface with its values.
  std::vector<std::uint32_t> remap(mesh.points.size(), std::numeric_limits<std::uint32_t>::max());
  for (const auto& t : mesh.triangles) {
    if (!reported(t[0])) {
      continue;
    }
    std::array<std::uint32_t, 3> mapped{};
    for (std::size_t k = 0; k < 3; ++k) {
      if (remap[t[k]] == std::numeric_limits<std::uint32_t>::max()) {
        remap[t[k]] = static_cast<std::uint32_t>(result.mesh.points.size());
        result.mesh.points.push_back(mesh.points[t[k]]);
        result.thickness_mm.push_back(thickness[t[k]]);
      }
      mapped[k] = remap[t[k]];
    }
    result.mesh.triangles.push_back(mapped);
  }
  result.stats =
      thicknessStats(result.mesh, result.thickness_mm, options.min_wall_mm, options.max_wall_mm);
  if (options.min_wall_mm > 0.0) {
    result.thin_regions = thinRegions(result.mesh, result.thickness_mm, options.min_wall_mm);
  }
  if (options.max_wall_mm > 0.0) {
    result.thick_regions = thickRegions(result.mesh, result.thickness_mm, options.max_wall_mm);
  }
  return result;
}

ThicknessStats thicknessStats(const IndexedMesh& mesh, const std::vector<float>& thickness_mm,
                              double min_wall_mm, double max_wall_mm) {
  return statsOf(mesh, thickness_mm, min_wall_mm, max_wall_mm);
}

std::vector<ThicknessRegion> thinRegions(const IndexedMesh& mesh,
                                         const std::vector<float>& thickness_mm, double limit_mm) {
  return regionsOf(
      mesh, thickness_mm, [limit_mm](float t) { return std::isfinite(t) && t < limit_mm; },
      [](double a, double b) { return a < b; });
}

std::vector<ThicknessRegion> thickRegions(const IndexedMesh& mesh,
                                          const std::vector<float>& thickness_mm, double limit_mm) {
  return regionsOf(
      mesh, thickness_mm, [limit_mm](float t) { return std::isfinite(t) && t > limit_mm; },
      [](double a, double b) { return a > b; });
}

std::vector<ThicknessRegion> nominalRegions(const IndexedMesh& mesh,
                                            const std::vector<float>& deviation_mm,
                                            double tolerance_mm, bool thinner) {
  std::vector<ThicknessRegion> regions = thinner ? thinRegions(mesh, deviation_mm, -tolerance_mm)
                                                 : thickRegions(mesh, deviation_mm, tolerance_mm);
  // A ray that reaches another wall than on the model gives a large deviation on a few points;
  // the regions that matter are the large ones.
  std::stable_sort(
      regions.begin(), regions.end(),
      [](const ThicknessRegion& a, const ThicknessRegion& b) { return a.area_mm2 > b.area_mm2; });
  for (std::size_t i = 0; i < regions.size(); ++i) {
    regions[i].id = static_cast<int>(i + 1);
  }
  return regions;
}

void addNominalThickness(WallThicknessResult& result, const Mesh& nominal,
                         const RigidTransform& nominal_to_scan,
                         const NominalThicknessOptions& options) {
  if (!(options.tolerance_mm > 0.0)) {
    throw std::invalid_argument("The thickness tolerance must be positive");
  }
  if (nominal.triangles.empty()) {
    throw std::invalid_argument("The nominal model has no triangles");
  }
  const TelemetryPhase phase("nominal thickness");
  const MeshDistance distance(nominal);
  NominalModel model{distance,
                     {},
                     result.max_thickness_mm,
                     std::cos(result.search_angle_deg * std::numbers::pi / 180.0),
                     1e-3 * result.voxel_size.minMm()};
  // Outward normals, also for a model wound inside out (as MeshDistance turns it).
  const double orientation = meshVolumeMm3(nominal) < 0.0 ? -1.0 : 1.0;
  model.normals.reserve(nominal.triangles.size());
  for (const auto& t : nominal.triangles) {
    const Vec a = toVec(t[0]);
    model.normals.push_back(orientation * normalized(cross(toVec(t[1]) - a, toVec(t[2]) - a)));
  }
  const RigidTransform scan_to_nominal = nominal_to_scan.inverse();
  NominalThickness out;
  out.source = options.source;
  out.tolerance_mm = options.tolerance_mm;
  const std::size_t n = result.mesh.points.size();
  out.nominal_mm.assign(n, kNotMeasured);
  out.deviation_mm.assign(n, kNotMeasured);
  std::atomic<std::size_t> done{0};
  std::mutex progress_mutex;
  int reported_percent = -1;
  tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, 256),
                    [&](const tbb::blocked_range<std::size_t>& range) {
                      for (std::size_t i = range.begin(); i != range.end(); ++i) {
                        const Vec p = scan_to_nominal.apply(toVec(result.mesh.points[i]));
                        const MeshDistance::Hit hit = distance.query(p);
                        if (std::abs(hit.distance) > model.max_mm) {
                          continue;  // nothing of the model here
                        }
                        const Vec x{hit.closest[0], hit.closest[1], hit.closest[2]};
                        const float t = result.method == Method::kSphere
                                            ? nominalSphereThickness(model, x, model.normal(hit))
                                            : nominalRayThickness(model, x, model.normal(hit));
                        out.nominal_mm[i] = t;
                        out.deviation_mm[i] = result.thickness_mm[i] - t;  // NaN if either is
                      }
                      const std::size_t now = done += range.size();
                      if (options.progress) {
                        const std::scoped_lock lock(progress_mutex);
                        const auto percent =
                            static_cast<int>(100 * now / std::max<std::size_t>(n, 1));
                        if (percent > reported_percent) {
                          reported_percent = percent;
                          options.progress(percent / 100.0);
                        }
                      }
                    });
  out.stats = deviationStats(result.mesh, out.deviation_mm, options.tolerance_mm);
  out.thinner_regions = nominalRegions(result.mesh, out.deviation_mm, options.tolerance_mm, true);
  out.thicker_regions = nominalRegions(result.mesh, out.deviation_mm, options.tolerance_mm, false);
  result.nominal = std::move(out);
}

nlohmann::json toJson(const WallThicknessResult& result) {
  const ThicknessStats& s = result.stats;
  Json percentiles = Json::object();
  for (std::size_t i = 0; i < kThicknessPercentiles.size(); ++i) {
    percentiles[std::to_string(static_cast<int>(kThicknessPercentiles[i]))] = s.percentiles_mm[i];
  }
  Json json = {{"method", toString(result.method)},
               {"voxel_size_mm", result.voxel_size},
               {"iso_value", result.iso_value},
               {"max_thickness_mm", result.max_thickness_mm},
               {"min_wall_mm", result.min_wall_mm},
               {"max_wall_mm", result.max_wall_mm},
               {"components", result.components},
               {"dropped_components", result.dropped_components},
               {"thickness",
                {{"vertices", s.vertices},
                 {"area_mm2", s.area_mm2},
                 {"measured_area_mm2", s.measured_area_mm2},
                 {"measured_fraction", s.area_mm2 > 0.0 ? s.measured_area_mm2 / s.area_mm2 : 0.0},
                 {"min_mm", s.min_mm},
                 {"max_mm", s.max_mm},
                 {"mean_mm", s.mean_mm},
                 {"std_mm", s.std_mm},
                 {"percentiles_mm", percentiles},
                 {"below_min", s.below_min},
                 {"above_max", s.above_max},
                 {"range_mm", s.range_mm},
                 {"histogram", s.histogram}}},
               {"thin_region_count", result.thin_regions.size()},
               {"thin_regions", regionsJson(result.thin_regions)},
               {"thick_region_count", result.thick_regions.size()},
               {"thick_regions", regionsJson(result.thick_regions)}};
  if (result.method == Method::kRay) {
    json["search_angle_deg"] = result.search_angle_deg;
  }
  if (result.nominal) {
    const NominalThickness& nominal = *result.nominal;
    const DeviationStats& d = nominal.stats;
    Json deviation_percentiles = Json::object();
    for (std::size_t i = 0; i < kDeviationPercentiles.size(); ++i) {
      deviation_percentiles[std::to_string(static_cast<int>(kDeviationPercentiles[i]))] =
          d.percentiles_mm[i];
    }
    json["nominal"] = {
        {"source", nominal.source},
        {"tolerance_mm", nominal.tolerance_mm},
        {"compared_area_mm2", d.area_mm2},
        {"compared_fraction", s.measured_area_mm2 > 0.0 ? d.area_mm2 / s.measured_area_mm2 : 0.0},
        {"deviation",
         {{"mean_mm", d.mean_mm},
          {"std_mm", d.std_mm},
          {"min_mm", d.min_mm},
          {"max_mm", d.max_mm},
          {"percentiles_mm", deviation_percentiles},
          {"within_tolerance", d.within_tolerance},
          {"thinner", d.below_tolerance},
          {"thicker", d.above_tolerance},
          {"range_mm", d.range_mm},
          {"histogram", d.histogram}}},
        {"thinner_region_count", nominal.thinner_regions.size()},
        {"thinner_regions", regionsJson(nominal.thinner_regions)},
        {"thicker_region_count", nominal.thicker_regions.size()},
        {"thicker_regions", regionsJson(nominal.thicker_regions)}};
  }
  return json;
}

void writeWallThickness(const WallThicknessResult& result, const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
  writeJson(dir / "wall_thickness.json", toJson(result));
  writePly(dir / "thickness.ply", result);
  if (result.mesh.points.empty()) {
    return;
  }
  RenderScene scene;
  scene.mesh = &result.mesh;
  scene.vertex_colors.reserve(result.thickness_mm.size());
  for (const float t : result.thickness_mm) {
    scene.vertex_colors.push_back(
        thicknessColor(t, result.min_wall_mm, result.max_wall_mm, result.stats.range_mm));
  }
  for (std::size_t k = 0; k < kDeviationViews.size(); ++k) {
    RenderView view;
    view.azimuth_degrees = kDeviationViews[k][0];
    view.elevation_degrees = kDeviationViews[k][1];
    writePng(render(scene, view), dir / ("thickness_view_" + std::to_string(k + 1) + ".png"));
  }
  if (result.nominal) {
    const NominalThickness& nominal = *result.nominal;
    scene.vertex_colors.clear();
    for (const float d : nominal.deviation_mm) {
      scene.vertex_colors.push_back(
          std::isfinite(d) ? deviationColor(d, nominal.tolerance_mm, nominal.stats.range_mm)
                           : thicknessColor(d, 0.0, 0.0, 1.0));
    }
    for (std::size_t k = 0; k < kDeviationViews.size(); ++k) {
      RenderView view;
      view.azimuth_degrees = kDeviationViews[k][0];
      view.elevation_degrees = kDeviationViews[k][1];
      writePng(render(scene, view),
               dir / ("thickness_deviation_view_" + std::to_string(k + 1) + ".png"));
    }
  }
}

ThicknessMesh readThicknessPly(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot open " + file.string());
  }
  ThicknessMesh out;
  try {
    tinyply::PlyFile ply;
    if (!ply.parse_header(in)) {
      throw std::runtime_error("no PLY header");
    }
    const auto points = ply.request_properties_from_element("vertex", {"x", "y", "z"});
    const auto thickness = ply.request_properties_from_element("vertex", {"thickness"});
    bool has_nominal = false;
    for (const tinyply::PlyElement& element : ply.get_elements()) {
      for (const tinyply::PlyProperty& property : element.properties) {
        has_nominal = has_nominal || (element.name == "vertex" && property.name == "nominal");
      }
    }
    std::shared_ptr<tinyply::PlyData> nominal;
    std::shared_ptr<tinyply::PlyData> deviation;
    if (has_nominal) {
      nominal = ply.request_properties_from_element("vertex", {"nominal"});
      deviation = ply.request_properties_from_element("vertex", {"deviation"});
    }
    const auto faces = ply.request_properties_from_element("face", {"vertex_indices"}, 3);
    ply.read(in);
    if (points->t != tinyply::Type::FLOAT32 || thickness->t != tinyply::Type::FLOAT32 ||
        (faces->t != tinyply::Type::INT32 && faces->t != tinyply::Type::UINT32) ||
        thickness->count != points->count) {
      throw std::runtime_error("unexpected property types");
    }
    out.mesh.points.resize(points->count);
    std::memcpy(out.mesh.points.data(), points->buffer.get(), points->buffer.size_bytes());
    out.thickness_mm.resize(thickness->count);
    std::memcpy(out.thickness_mm.data(), thickness->buffer.get(), thickness->buffer.size_bytes());
    for (const auto& [data, values] :
         {std::pair{nominal, &out.nominal_mm}, std::pair{deviation, &out.deviation_mm}}) {
      if (!data) {
        continue;
      }
      if (data->t != tinyply::Type::FLOAT32 || data->count != points->count) {
        throw std::runtime_error("unexpected property types");
      }
      values->resize(data->count);
      std::memcpy(values->data(), data->buffer.get(), data->buffer.size_bytes());
    }
    std::vector<std::int32_t> indices(faces->count * 3);
    if (faces->buffer.size_bytes() != indices.size() * sizeof(std::int32_t)) {
      throw std::runtime_error("faces that are not triangles");
    }
    std::memcpy(indices.data(), faces->buffer.get(), faces->buffer.size_bytes());
    out.mesh.triangles.resize(faces->count);
    for (std::size_t i = 0; i < indices.size(); ++i) {
      if (indices[i] < 0 || static_cast<std::size_t>(indices[i]) >= points->count) {
        throw std::runtime_error("invalid face");
      }
      out.mesh.triangles[i / 3][i % 3] = static_cast<std::uint32_t>(indices[i]);
    }
  } catch (const std::exception& error) {
    throw std::runtime_error("Cannot read " + file.string() + ": " + error.what());
  }
  return out;
}

}  // namespace voxelsieve
