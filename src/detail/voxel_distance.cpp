#include "detail/voxel_distance.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <queue>
#include <random>
#include <vector>

namespace voxelsieve::detail {
namespace {

using openvdb::Coord;
using openvdb::CoordBBox;
using Tree = openvdb::BoolTree;
using Root = Tree::RootNodeType;
using Upper = Root::ChildNodeType;
using Lower = Upper::ChildNodeType;
using Leaf = Tree::LeafNodeType;

/// A node of a tree waiting to be searched, with the gap from the query to its bounding box.
struct Pending {
  double gap = 0.0;
  int level = 0;  // 2 upper internal node, 1 lower internal node, 0 leaf
  const void* node = nullptr;
  bool operator>(const Pending& other) const { return gap > other.gap; }
};

/// Calls `visit(leaf)` for the leaves of `tree` in the order of their gap to `query`, as long as
/// that gap is below `best`, which `visit` may lower.
template <typename Visit>
void leavesByGap(const Tree& tree, const CoordBBox& query, const VoxelSize& voxel_size,
                 const double& best, Visit&& visit) {
  std::priority_queue<Pending, std::vector<Pending>, std::greater<>> queue;
  const auto push = [&](const CoordBBox& box, int level, const void* node) {
    const double gap = voxelGapMm(query, box, voxel_size);
    if (gap < best) {
      queue.push({gap, level, node});
    }
  };
  for (auto it = tree.root().cbeginChildOn(); it; ++it) {
    push(it->getNodeBoundingBox(), 2, &*it);
  }
  while (!queue.empty() && queue.top().gap < best) {
    const Pending next = queue.top();
    queue.pop();
    if (next.level == 2) {
      for (auto it = static_cast<const Upper*>(next.node)->cbeginChildOn(); it; ++it) {
        push(it->getNodeBoundingBox(), 1, &*it);
      }
    } else if (next.level == 1) {
      for (auto it = static_cast<const Lower*>(next.node)->cbeginChildOn(); it; ++it) {
        push(it->getNodeBoundingBox(), 0, &*it);
      }
    } else {
      visit(*static_cast<const Leaf*>(next.node));
    }
  }
}

double squaredDistance(const std::array<double, 3>& a, const std::array<double, 3>& b) {
  double sum = 0.0;
  for (std::size_t i = 0; i < 3; ++i) {
    sum += (a[i] - b[i]) * (a[i] - b[i]);
  }
  return sum;
}

bool encloses(const EnclosingSphere& sphere, const std::array<double, 3>& p) {
  if (sphere.radius < 0.0) {
    return false;
  }
  const double r2 = sphere.radius * sphere.radius;
  return squaredDistance(sphere.center, p) <= r2 * (1.0 + 1e-10) + 1e-12;
}

/// Smallest sphere with all `count` support points on its surface; empty (radius -1) for none.
EnclosingSphere throughSupport(const std::array<std::array<double, 3>, 4>& support, int count) {
  if (count == 0) {
    return {{0.0, 0.0, 0.0}, -1.0};
  }
  const auto& origin = support[0];
  if (count == 1) {
    return {origin, 0.0};
  }
  // The centre lies in the affine hull of the support: origin + sum of lambda_j * u_j with
  // u_j = support_j - origin, and is as far from every support point as from the origin.
  const auto m = static_cast<std::size_t>(count - 1);
  std::array<std::array<double, 3>, 3> u{};
  for (std::size_t j = 0; j < m; ++j) {
    for (std::size_t a = 0; a < 3; ++a) {
      u[j][a] = support[j + 1][a] - origin[a];
    }
  }
  const auto dot = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };
  std::array<std::array<double, 4>, 3> rows{};  // augmented system
  double scale = 0.0;
  for (std::size_t j = 0; j < m; ++j) {
    for (std::size_t i = 0; i < m; ++i) {
      rows[j][i] = 2.0 * dot(u[j], u[i]);
    }
    rows[j][3] = dot(u[j], u[j]);
    scale = std::max(scale, rows[j][3]);
  }
  for (std::size_t col = 0; col < m; ++col) {
    std::size_t pivot = col;
    for (std::size_t r = col + 1; r < m; ++r) {
      if (std::abs(rows[r][col]) > std::abs(rows[pivot][col])) {
        pivot = r;
      }
    }
    if (std::abs(rows[pivot][col]) <= 1e-12 * std::max(scale, 1e-30)) {
      // Degenerate support (collinear or coplanar points): drop the last point. The caller
      // grows the radius to enclose every point at the end.
      return throughSupport(support, count - 1);
    }
    std::swap(rows[col], rows[pivot]);
    for (std::size_t r = 0; r < m; ++r) {
      if (r != col) {
        const double f = rows[r][col] / rows[col][col];
        for (std::size_t c = col; c < 4; ++c) {
          rows[r][c] -= f * rows[col][c];
        }
      }
    }
  }
  EnclosingSphere sphere{origin, 0.0};
  for (std::size_t j = 0; j < m; ++j) {
    const double lambda = rows[j][3] / rows[j][j];
    for (std::size_t a = 0; a < 3; ++a) {
      sphere.center[a] += lambda * u[j][a];
    }
  }
  sphere.radius = std::sqrt(squaredDistance(sphere.center, origin));
  return sphere;
}

/// Smallest sphere enclosing the first `n` points with the `count` support points on its surface.
/// Recursion goes at most four levels deep, one per support point.
EnclosingSphere welzl(std::span<const std::array<double, 3>> points, std::size_t n,
                      std::array<std::array<double, 3>, 4>& support, int count) {
  EnclosingSphere sphere = throughSupport(support, count);
  if (count == 4) {
    return sphere;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (!encloses(sphere, points[i])) {
      support[static_cast<std::size_t>(count)] = points[i];
      sphere = welzl(points, i, support, count + 1);
    }
  }
  return sphere;
}

}  // namespace

double voxelGapMm(const CoordBBox& a, const CoordBBox& b, const VoxelSize& voxel_size) {
  double sum = 0.0;
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const auto apart =
        std::max<std::int64_t>({0, static_cast<std::int64_t>(a.min()[axis]) - b.max()[axis],
                                static_cast<std::int64_t>(b.min()[axis]) - a.max()[axis]});
    // Neighbouring voxels (one index apart) touch.
    const double gap = static_cast<double>(std::max<std::int64_t>(0, apart - 1)) * voxel_size[axis];
    sum += gap * gap;
  }
  return std::sqrt(sum);
}

double maskGapMm(const Tree& a, const Tree& b, const VoxelSize& voxel_size, double bound) {
  CoordBBox box_a;
  CoordBBox box_b;
  if (!a.evalActiveVoxelBoundingBox(box_a) || !b.evalActiveVoxelBoundingBox(box_b)) {
    return std::numeric_limits<double>::infinity();
  }
  double best = bound;
  leavesByGap(a, box_b, voxel_size, best, [&](const Leaf& leaf_a) {
    for (auto it = leaf_a.cbeginValueOn(); it; ++it) {
      const CoordBBox voxel(it.getCoord(), it.getCoord());
      if (voxelGapMm(voxel, box_b, voxel_size) >= best) {
        continue;
      }
      leavesByGap(b, voxel, voxel_size, best, [&](const Leaf& leaf_b) {
        for (auto jt = leaf_b.cbeginValueOn(); jt; ++jt) {
          best = std::min(best,
                          voxelGapMm(voxel, CoordBBox(jt.getCoord(), jt.getCoord()), voxel_size));
        }
      });
    }
  });
  return best;
}

EnclosingSphere enclosingSphere(std::span<std::array<double, 3>> points) {
  if (points.empty()) {
    return {};
  }
  // A fixed seed keeps the result reproducible; the order only affects the running time.
  std::mt19937_64 random(points.size());
  std::shuffle(points.begin(), points.end(), random);
  std::array<std::array<double, 3>, 4>
      support{};  // NOLINT(misc-const-correctness): written by welzl
  EnclosingSphere sphere = welzl(points, points.size(), support, 0);
  // Degenerate supports may leave a point slightly outside: grow the radius to enclose all.
  double r2 = sphere.radius * sphere.radius;
  for (const auto& p : points) {
    r2 = std::max(r2, squaredDistance(sphere.center, p));
  }
  sphere.radius = std::sqrt(r2);
  return sphere;
}

}  // namespace voxelsieve::detail
