// Extraction of the scanned points of an element and their fit.

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "detail/dimensional.hpp"

namespace voxelsieve::detail::dimensional {
namespace {

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

}  // namespace

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

}  // namespace voxelsieve::detail::dimensional
