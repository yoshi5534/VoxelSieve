#include "voxelsieve/compare.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tinyply.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "voxelsieve/io.hpp"
#include "voxelsieve/render.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;
using Vec = std::array<double, 3>;
using Mat = std::array<double, 9>;

Vec operator+(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec operator-(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec operator*(double s, const Vec& a) { return {s * a[0], s * a[1], s * a[2]}; }
double dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double norm(const Vec& a) { return std::sqrt(dot(a, a)); }

Mat multiply(const Mat& a, const Mat& b) {
  Mat m{};
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      for (std::size_t k = 0; k < 3; ++k) {
        m[3 * r + c] += a[3 * r + k] * b[3 * k + c];
      }
    }
  }
  return m;
}
Mat transpose(const Mat& a) { return {a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8]}; }
double determinant(const Mat& a) {
  return a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
         a[2] * (a[3] * a[7] - a[4] * a[6]);
}

/// Rotation matrix of the rotation vector `w` (axis times angle in radians).
Mat rodrigues(const Vec& w) {
  const double angle = norm(w);
  if (angle < 1e-15) {
    return {1.0, -w[2], w[1], w[2], 1.0, -w[0], -w[1], w[0], 1.0};
  }
  const Vec k = (1.0 / angle) * w;
  const double c = std::cos(angle);
  const double s = std::sin(angle);
  const double t = 1.0 - c;
  return {t * k[0] * k[0] + c,        t * k[0] * k[1] - s * k[2], t * k[0] * k[2] + s * k[1],
          t * k[0] * k[1] + s * k[2], t * k[1] * k[1] + c,        t * k[1] * k[2] - s * k[0],
          t * k[0] * k[2] - s * k[1], t * k[1] * k[2] + s * k[0], t * k[2] * k[2] + c};
}

/// Eigen decomposition of a symmetric 3x3 matrix by Jacobi rotations. Returns the eigenvectors
/// as columns, sorted by descending eigenvalue.
std::pair<Mat, Vec> symmetricEigen(Mat a) {
  Mat v{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  for (int sweep = 0; sweep < 50; ++sweep) {
    const double off = a[1] * a[1] + a[2] * a[2] + a[5] * a[5];
    if (off < 1e-30 * (a[0] * a[0] + a[4] * a[4] + a[8] * a[8]) + 1e-300) {
      break;
    }
    for (const auto& [p, q] :
         {std::pair<std::size_t, std::size_t>{0, 1}, std::pair<std::size_t, std::size_t>{0, 2},
          std::pair<std::size_t, std::size_t>{1, 2}}) {
      const double apq = a[3 * p + q];
      if (std::abs(apq) < 1e-300) {
        continue;
      }
      const double theta = (a[3 * q + q] - a[3 * p + p]) / (2.0 * apq);
      const double t =
          (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
      const double c = 1.0 / std::sqrt(t * t + 1.0);
      const double s = t * c;
      Mat j{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
      j[3 * p + p] = c;
      j[3 * q + q] = c;
      j[3 * p + q] = s;
      j[3 * q + p] = -s;
      a = multiply(transpose(j), multiply(a, j));
      v = multiply(v, j);
    }
  }
  std::array<std::size_t, 3> order{0, 1, 2};
  std::sort(order.begin(), order.end(),
            [&a](std::size_t l, std::size_t r) { return a[4 * l] > a[4 * r]; });
  Mat sorted{};
  Vec values{};
  for (std::size_t c = 0; c < 3; ++c) {
    values[c] = a[4 * order[c]];
    for (std::size_t r = 0; r < 3; ++r) {
      sorted[3 * r + c] = v[3 * r + order[c]];
    }
  }
  return {sorted, values};
}

// ---------------------------------------------------------------------------------------------
// Closest point on a triangle (Ericson, Real-Time Collision Detection, 5.1.5)

enum class Region : std::uint8_t { kFace, kEdge, kVertex };

struct TrianglePoint {
  Vec point;
  Region region = Region::kFace;
  std::size_t vertex = 0;  // for kVertex
};

TrianglePoint closestOnTriangle(const Vec& p, const Vec& a, const Vec& b, const Vec& c) {
  const Vec ab = b - a;
  const Vec ac = c - a;
  const Vec ap = p - a;
  const double d1 = dot(ab, ap);
  const double d2 = dot(ac, ap);
  if (d1 <= 0.0 && d2 <= 0.0) {
    return {a, Region::kVertex, 0};
  }
  const Vec bp = p - b;
  const double d3 = dot(ab, bp);
  const double d4 = dot(ac, bp);
  if (d3 >= 0.0 && d4 <= d3) {
    return {b, Region::kVertex, 1};
  }
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
    return {a + (d1 / (d1 - d3)) * ab, Region::kEdge};
  }
  const Vec cp = p - c;
  const double d5 = dot(ab, cp);
  const double d6 = dot(ac, cp);
  if (d6 >= 0.0 && d5 <= d6) {
    return {c, Region::kVertex, 2};
  }
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
    return {a + (d2 / (d2 - d6)) * ac, Region::kEdge};
  }
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
    return {b + ((d4 - d3) / ((d4 - d3) + (d5 - d6))) * (c - b), Region::kEdge};
  }
  const double denom = 1.0 / (va + vb + vc);
  return {a + (vb * denom) * ab + (vc * denom) * ac, Region::kFace};
}

// ---------------------------------------------------------------------------------------------
// Surface moments for the coarse alignment

struct Moments {
  Vec centroid{};
  Mat covariance{};
  double area = 0.0;
};

/// Area-weighted centroid and covariance of a surface, exact per triangle:
/// the integral of x x^T over a triangle is area / 12 * (a a^T + b b^T + c c^T + s s^T), s = a+b+c.
template <typename ForEachTriangle>
Moments surfaceMoments(ForEachTriangle for_each) {
  Moments m;
  Vec first{};
  Mat second{};
  for_each([&](const Vec& a, const Vec& b, const Vec& c) {
    const double area = 0.5 * norm(cross(b - a, c - a));
    const Vec s = a + b + c;
    m.area += area;
    first = first + (area / 3.0) * s;
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t k = 0; k < 3; ++k) {
        second[3 * r + k] += area / 12.0 * (a[r] * a[k] + b[r] * b[k] + c[r] * c[k] + s[r] * s[k]);
      }
    }
  });
  if (m.area <= 0.0) {
    throw std::invalid_argument("The surface has no area");
  }
  m.centroid = (1.0 / m.area) * first;
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t k = 0; k < 3; ++k) {
      m.covariance[3 * r + k] = second[3 * r + k] / m.area - m.centroid[r] * m.centroid[k];
    }
  }
  return m;
}

// ---------------------------------------------------------------------------------------------
// Fine alignment: point-to-plane ICP with Huber weights

struct Fit {
  RigidTransform transform;  // scan to CAD
  double rms = 0.0;
  double inliers = 0.0;
  double median = 0.0;
  /// Mean distance with each point capped at ten times the floor: ranks candidate alignments,
  /// also by small features that fit badly.
  double score = 0.0;
  int iterations = 0;
};

struct Residual {
  double r = 0.0;
  Vec x{};
  Vec n{};
};

void residuals(const MeshDistance& cad, std::span<const Vec> points, const RigidTransform& t,
               std::vector<Residual>& out) {
  out.resize(points.size());
  tbb::parallel_for(tbb::blocked_range<std::size_t>(0, points.size(), 256),
                    [&](const tbb::blocked_range<std::size_t>& range) {
                      for (std::size_t i = range.begin(); i != range.end(); ++i) {
                        const Vec x = t.apply(points[i]);
                        const MeshDistance::Hit hit = cad.query(x);
                        out[i] = {hit.distance, x, hit.normal};
                      }
                    });
}

double medianAbs(const std::vector<Residual>& res) {
  std::vector<double> a(res.size());
  std::transform(res.begin(), res.end(), a.begin(),
                 [](const Residual& r) { return std::abs(r.r); });
  const auto mid = a.begin() + static_cast<std::ptrdiff_t>(a.size() / 2);
  std::nth_element(a.begin(), mid, a.end());
  return *mid;
}

/// Solves the 6x6 system by Gaussian elimination with partial pivoting.
std::array<double, 6> solve6(std::array<std::array<double, 7>, 6> m) {
  for (std::size_t col = 0; col < 6; ++col) {
    std::size_t pivot = col;
    for (std::size_t r = col + 1; r < 6; ++r) {
      if (std::abs(m[r][col]) > std::abs(m[pivot][col])) {
        pivot = r;
      }
    }
    std::swap(m[col], m[pivot]);
    if (std::abs(m[col][col]) < 1e-300) {
      continue;
    }
    for (std::size_t r = 0; r < 6; ++r) {
      if (r != col) {
        const double f = m[r][col] / m[col][col];
        for (std::size_t k = col; k < 7; ++k) {
          m[r][k] -= f * m[col][k];
        }
      }
    }
  }
  std::array<double, 6> x{};
  for (std::size_t r = 0; r < 6; ++r) {
    x[r] = std::abs(m[r][r]) < 1e-300 ? 0.0 : m[r][6] / m[r][r];
  }
  return x;
}

Fit icp(const MeshDistance& cad, std::span<const Vec> points, RigidTransform start,
        int max_iterations, double floor_mm) {
  Fit fit;
  fit.transform = start;
  // Converged when no point moves by more than a thousandth of the floor (half a voxel).
  Vec centre{};
  for (const Vec& p : points) {
    centre = centre + p;
  }
  centre = (1.0 / static_cast<double>(std::max<std::size_t>(points.size(), 1))) * centre;
  double radius = 0.0;
  for (const Vec& p : points) {
    radius = std::max(radius, norm(p - centre));
  }
  std::vector<Residual> res;
  for (int iteration = 0; iteration < max_iterations; ++iteration) {
    residuals(cad, points, fit.transform, res);
    const double sigma = 1.4826 * medianAbs(res);
    const double huber = std::max(2.0 * sigma, floor_mm);
    const double reject = std::max(5.0 * sigma, 3.0 * floor_mm);
    std::array<std::array<double, 7>, 6> system{};
    for (const Residual& r : res) {
      const double a = std::abs(r.r);
      if (a > reject) {
        continue;
      }
      const double w = a <= huber ? 1.0 : huber / a;
      const Vec xn = cross(r.x, r.n);
      const std::array<double, 6> j{xn[0], xn[1], xn[2], r.n[0], r.n[1], r.n[2]};
      for (std::size_t row = 0; row < 6; ++row) {
        for (std::size_t col = 0; col < 6; ++col) {
          system[row][col] += w * j[row] * j[col];
        }
        system[row][6] -= w * j[row] * r.r;
      }
    }
    // A little damping keeps symmetric parts (free rotation about an axis) solvable.
    double trace = 0.0;
    for (std::size_t d = 0; d < 6; ++d) {
      trace += system[d][d];
    }
    for (std::size_t d = 0; d < 6; ++d) {
      system[d][d] += 1e-9 * trace + 1e-300;
    }
    const auto step = solve6(system);
    const Vec w{step[0], step[1], step[2]};
    const Vec delta{step[3], step[4], step[5]};
    RigidTransform update;
    update.rotation = rodrigues(w);
    update.translation = delta;
    fit.transform = update.after(fit.transform);
    fit.iterations = iteration + 1;
    // The largest motion of a point: the rotation acts about the origin of the CAD frame, where
    // the points lie at the transformed centre plus up to `radius`.
    const double lever = norm(fit.transform.apply(centre)) + radius;
    if (norm(w) * lever + norm(delta) < 1e-3 * floor_mm) {
      break;
    }
  }
  residuals(cad, points, fit.transform, res);
  fit.median = medianAbs(res);
  const double cut = std::max(3.0 * 1.4826 * fit.median, floor_mm);
  double sum = 0.0;
  double capped = 0.0;
  std::size_t count = 0;
  for (const Residual& r : res) {
    capped += std::min(std::abs(r.r), 10.0 * floor_mm);
    if (std::abs(r.r) <= cut) {
      sum += r.r * r.r;
      ++count;
    }
  }
  fit.rms = count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
  fit.score = res.empty() ? 0.0 : capped / static_cast<double>(res.size());
  fit.inliers = res.empty() ? 0.0 : static_cast<double>(count) / static_cast<double>(res.size());
  return fit;
}

/// Coarse alignment: principal axes of both surfaces, every proper axis assignment (and, when two
/// principal moments are about equal, rotations about the third axis), each refined briefly; the
/// best one wins.
RigidTransform coarseAlignment(const MeshDistance& cad, std::span<const Vec> points,
                               const Moments& scan, const Moments& nominal, double floor_mm) {
  const auto [scan_axes, scan_values] = symmetricEigen(scan.covariance);
  const auto [cad_axes, cad_values] = symmetricEigen(nominal.covariance);
  // Rotations about the CAD axis whose other two moments are about equal.
  std::vector<Mat> spins{Mat{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}};
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const std::size_t p = (axis + 1) % 3;
    const std::size_t q = (axis + 2) % 3;
    if (std::abs(cad_values[p] - cad_values[q]) <= 0.15 * std::max(cad_values[p], cad_values[q])) {
      const Vec k{cad_axes[axis], cad_axes[3 + axis], cad_axes[6 + axis]};
      for (int step = 1; step < 12; ++step) {
        spins.push_back(rodrigues((step * std::numbers::pi / 6.0) * k));
      }
      break;
    }
  }
  std::vector<RigidTransform> candidates;
  const std::array<std::array<std::size_t, 3>, 6> permutations{
      {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};
  for (const auto& perm : permutations) {
    for (std::size_t signs = 0; signs < 8; ++signs) {
      Mat p{};
      for (std::size_t r = 0; r < 3; ++r) {
        p[3 * r + perm[r]] = ((signs >> r) & 1U) != 0 ? -1.0 : 1.0;
      }
      const Mat base = multiply(cad_axes, multiply(p, transpose(scan_axes)));
      if (determinant(base) < 0.0) {
        continue;
      }
      for (const Mat& spin : spins) {
        RigidTransform t;
        t.rotation = multiply(spin, base);
        t.translation = nominal.centroid - t.rotate(scan.centroid);
        candidates.push_back(t);
      }
    }
  }
  // Rank all candidates by their fit as they are on a few points, refine the best on more
  // points and keep the distinct ones, refine those on more still. The winner is chosen on all
  // points, so that small features that break a symmetry (a keyway) decide.
  const auto ranked = [&](const std::vector<RigidTransform>& start, std::size_t count,
                          int iterations, std::size_t keep) {
    const std::span<const Vec> subset = points.subspan(0, std::min(points.size(), count));
    std::vector<std::pair<double, RigidTransform>> scored;
    scored.reserve(start.size());
    for (const RigidTransform& candidate : start) {
      const Fit fit = icp(cad, subset, candidate, iterations, floor_mm);
      scored.emplace_back(fit.score, fit.transform);
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<RigidTransform> best;
    for (const auto& [score, t] : scored) {
      const bool known = std::any_of(best.begin(), best.end(), [&t](const RigidTransform& b) {
        return b.after(t.inverse()).angleDegrees() < 5.0 &&
               norm(b.translation - t.translation) < 1.0;
      });
      if (!known) {
        best.push_back(t);
      }
      if (best.size() == keep) {
        break;
      }
    }
    return best;
  };
  const auto stage1 = ranked(candidates, 200, 0, 16);
  const auto stage2 = ranked(stage1, 300, 8, 8);
  const auto stage3 = ranked(stage2, 2000, 15, 8);
  return ranked(stage3, points.size(), 0, 1).front();
}

Vec toVec(const std::array<float, 3>& p) { return {p[0], p[1], p[2]}; }

std::vector<double> triangleAreas(const IndexedMesh& mesh) {
  std::vector<double> areas(mesh.triangles.size());
  for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
    const auto& t = mesh.triangles[i];
    const Vec a = toVec(mesh.points[t[0]]);
    areas[i] = 0.5 * norm(cross(toVec(mesh.points[t[1]]) - a, toVec(mesh.points[t[2]]) - a));
  }
  return areas;
}

/// Points sampled uniformly by area, in random order (a fixed seed, so fits repeat exactly).
std::vector<Vec> sampleByArea(const IndexedMesh& mesh, const std::vector<double>& areas,
                              std::size_t count) {
  std::vector<double> cumulative(areas.size());
  std::partial_sum(areas.begin(), areas.end(), cumulative.begin());
  if (cumulative.empty() || !(cumulative.back() > 0.0)) {
    throw std::invalid_argument("The surface has no area");
  }
  std::mt19937_64 random(20260928);
  std::uniform_real_distribution<double> uniform(0.0, 1.0);
  std::vector<Vec> samples(count);
  for (Vec& sample : samples) {
    const double pick = uniform(random) * cumulative.back();
    const auto index = static_cast<std::size_t>(std::min<std::ptrdiff_t>(
        std::upper_bound(cumulative.begin(), cumulative.end(), pick) - cumulative.begin(),
        static_cast<std::ptrdiff_t>(cumulative.size()) - 1));
    const auto& t = mesh.triangles[index];
    const double r1 = std::sqrt(uniform(random));
    const double r2 = uniform(random);
    sample = (1.0 - r1) * toVec(mesh.points[t[0]]) + (r1 * (1.0 - r2)) * toVec(mesh.points[t[1]]) +
             (r1 * r2) * toVec(mesh.points[t[2]]);
  }
  return samples;
}

Moments meshMoments(const IndexedMesh& mesh) {
  return surfaceMoments([&](const auto& visit) {
    for (const auto& t : mesh.triangles) {
      visit(toVec(mesh.points[t[0]]), toVec(mesh.points[t[1]]), toVec(mesh.points[t[2]]));
    }
  });
}

Moments meshMoments(const Mesh& mesh) {
  return surfaceMoments([&](const auto& visit) {
    for (const auto& t : mesh.triangles) {
      visit(Vec{t[0][0], t[0][1], t[0][2]}, Vec{t[1][0], t[1][1], t[1][2]},
            Vec{t[2][0], t[2][1], t[2][2]});
    }
  });
}

/// Eigenvector of the largest eigenvalue of a symmetric 4x4 matrix, by Jacobi rotations.
std::array<double, 4> largestEigenvector4(std::array<double, 16> a) {
  std::array<double, 16> v{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  for (int sweep = 0; sweep < 100; ++sweep) {
    double off = 0.0;
    double diagonal = 0.0;
    for (std::size_t r = 0; r < 4; ++r) {
      diagonal += a[5 * r] * a[5 * r];
      for (std::size_t c = r + 1; c < 4; ++c) {
        off += a[4 * r + c] * a[4 * r + c];
      }
    }
    if (off < 1e-30 * diagonal + 1e-300) {
      break;
    }
    for (std::size_t p = 0; p < 4; ++p) {
      for (std::size_t q = p + 1; q < 4; ++q) {
        const double apq = a[4 * p + q];
        if (std::abs(apq) < 1e-300) {
          continue;
        }
        const double theta = (a[4 * q + q] - a[4 * p + p]) / (2.0 * apq);
        const double t =
            (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;
        // a <- J^T a J, v <- v J with the rotation in the (p, q) plane.
        for (std::size_t k = 0; k < 4; ++k) {
          const double akp = a[4 * k + p];
          const double akq = a[4 * k + q];
          a[4 * k + p] = c * akp - s * akq;
          a[4 * k + q] = s * akp + c * akq;
        }
        for (std::size_t k = 0; k < 4; ++k) {
          const double apk = a[4 * p + k];
          const double aqk = a[4 * q + k];
          a[4 * p + k] = c * apk - s * aqk;
          a[4 * q + k] = s * apk + c * aqk;
        }
        for (std::size_t k = 0; k < 4; ++k) {
          const double vkp = v[4 * k + p];
          const double vkq = v[4 * k + q];
          v[4 * k + p] = c * vkp - s * vkq;
          v[4 * k + q] = s * vkp + c * vkq;
        }
      }
    }
  }
  std::size_t best = 0;
  for (std::size_t i = 1; i < 4; ++i) {
    if (a[5 * i] > a[5 * best]) {
      best = i;
    }
  }
  return {v[best], v[4 + best], v[8 + best], v[12 + best]};
}

// ---------------------------------------------------------------------------------------------
// Statistics

double niceCeil(double value) {
  if (value <= 0.0) {
    return 1.0;
  }
  const double power = std::pow(10.0, std::floor(std::log10(value)));
  for (const double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
    if (m * power >= value * (1.0 - 1e-9)) {
      return m * power;
    }
  }
  return 10.0 * power;
}

DeviationStats deviationStats(const IndexedMesh& mesh, const std::vector<float>& deviation,
                              double tolerance) {
  constexpr int kBins = 40;
  DeviationStats s;
  s.vertices = mesh.points.size();
  std::vector<double> weight(mesh.points.size(), 0.0);
  for (const auto& t : mesh.triangles) {
    const auto point = [&mesh](std::uint32_t i) {
      const auto& p = mesh.points[i];
      return Vec{p[0], p[1], p[2]};
    };
    const double area = 0.5 * norm(cross(point(t[1]) - point(t[0]), point(t[2]) - point(t[0])));
    for (const std::uint32_t i : t) {
      weight[i] += area / 3.0;
    }
    s.area_mm2 += area;
  }
  if (s.area_mm2 <= 0.0) {
    return s;
  }
  std::vector<std::uint32_t> order(deviation.size());
  std::iota(order.begin(), order.end(), 0U);
  std::sort(order.begin(), order.end(),
            [&deviation](std::uint32_t a, std::uint32_t b) { return deviation[a] < deviation[b]; });
  s.min_mm = deviation[order.front()];
  s.max_mm = deviation[order.back()];
  double sum = 0.0;
  double sum2 = 0.0;
  for (std::size_t i = 0; i < deviation.size(); ++i) {
    const double d = deviation[i];
    sum += weight[i] * d;
    sum2 += weight[i] * d * d;
    if (d > tolerance) {
      s.above_tolerance += weight[i];
    } else if (d < -tolerance) {
      s.below_tolerance += weight[i];
    } else {
      s.within_tolerance += weight[i];
    }
  }
  s.mean_mm = sum / s.area_mm2;
  s.rms_mm = std::sqrt(sum2 / s.area_mm2);
  s.std_mm = std::sqrt(std::max(0.0, s.rms_mm * s.rms_mm - s.mean_mm * s.mean_mm));
  s.within_tolerance /= s.area_mm2;
  s.above_tolerance /= s.area_mm2;
  s.below_tolerance /= s.area_mm2;
  double cumulative = 0.0;
  std::size_t next = 0;
  for (const std::uint32_t i : order) {
    cumulative += weight[i];
    while (next < kDeviationPercentiles.size() &&
           cumulative >= kDeviationPercentiles[next] / 100.0 * s.area_mm2) {
      s.percentiles_mm[next++] = deviation[i];
    }
  }
  for (; next < kDeviationPercentiles.size(); ++next) {
    s.percentiles_mm[next] = s.max_mm;
  }
  // Range: the 99th percentile of |deviation|, at least twice the tolerance, rounded up.
  std::vector<std::uint32_t> by_size = order;
  std::sort(by_size.begin(), by_size.end(), [&deviation](std::uint32_t a, std::uint32_t b) {
    return std::abs(deviation[a]) < std::abs(deviation[b]);
  });
  double p99 = 0.0;
  cumulative = 0.0;
  for (const std::uint32_t i : by_size) {
    cumulative += weight[i];
    p99 = std::abs(deviation[i]);
    if (cumulative >= 0.99 * s.area_mm2) {
      break;
    }
  }
  s.range_mm = niceCeil(std::max(2.0 * tolerance, p99));
  s.histogram.assign(kBins, 0.0);
  for (std::size_t i = 0; i < deviation.size(); ++i) {
    const double t = (deviation[i] + s.range_mm) / (2.0 * s.range_mm);
    const int bin = std::clamp(static_cast<int>(std::floor(t * kBins)), 0, kBins - 1);
    s.histogram[static_cast<std::size_t>(bin)] += weight[i] / s.area_mm2;
  }
  return s;
}

// ---------------------------------------------------------------------------------------------
// Output

void writePly(const std::filesystem::path& file, const CompareResult& result) {
  const std::size_t n = result.mesh.points.size();
  std::vector<std::uint8_t> colors;
  colors.reserve(n * 3);
  for (std::size_t i = 0; i < n; ++i) {
    for (const std::uint8_t c :
         deviationColor(result.deviation_mm[i], result.tolerance_mm, result.stats.range_mm)) {
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
      "VoxelSieve nominal-actual comparison: scanned surface in mm, deviation from the CAD "
      "surface in mm (positive: more material)");
  ply.add_properties_to_element("vertex", {"x", "y", "z"}, tinyply::Type::FLOAT32, n,
                                bytes(result.mesh.points.data()), tinyply::Type::INVALID, 0);
  ply.add_properties_to_element("vertex", {"deviation"}, tinyply::Type::FLOAT32, n,
                                bytes(result.deviation_mm.data()), tinyply::Type::INVALID, 0);
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

/// Orthographic views of the coloured surface along x, y and z, flat shaded, viewed from the
/// positive side of the axis with the other two axes to the right and up.
void writeImages(const CompareResult& result, const std::filesystem::path& dir) {
  if (result.mesh.points.empty()) {
    return;
  }
  RenderScene scene;
  scene.mesh = &result.mesh;
  scene.vertex_colors.reserve(result.deviation_mm.size());
  for (const float d : result.deviation_mm) {
    scene.vertex_colors.push_back(deviationColor(d, result.tolerance_mm, result.stats.range_mm));
  }
  for (std::size_t k = 0; k < kDeviationViews.size(); ++k) {
    RenderView view;
    view.azimuth_degrees = kDeviationViews[k][0];
    view.elevation_degrees = kDeviationViews[k][1];
    writePng(render(scene, view), dir / ("deviation_view_" + std::to_string(k + 1) + ".png"));
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// RigidTransform

std::array<double, 3> RigidTransform::rotate(const std::array<double, 3>& v) const {
  const Mat& r = rotation;
  return {r[0] * v[0] + r[1] * v[1] + r[2] * v[2], r[3] * v[0] + r[4] * v[1] + r[5] * v[2],
          r[6] * v[0] + r[7] * v[1] + r[8] * v[2]};
}

std::array<double, 3> RigidTransform::apply(const std::array<double, 3>& p) const {
  return rotate(p) + translation;
}

RigidTransform RigidTransform::inverse() const {
  RigidTransform t;
  t.rotation = transpose(rotation);
  t.translation = -1.0 * t.rotate(translation);
  return t;
}

RigidTransform RigidTransform::after(const RigidTransform& first) const {
  RigidTransform t;
  t.rotation = multiply(rotation, first.rotation);
  t.translation = apply(first.translation);
  return t;
}

double RigidTransform::angleDegrees() const {
  const double c = std::clamp((rotation[0] + rotation[4] + rotation[8] - 1.0) / 2.0, -1.0, 1.0);
  return std::acos(c) * 180.0 / std::numbers::pi;
}

std::array<double, 16> RigidTransform::matrix() const {
  return {rotation[0], rotation[1], rotation[2], translation[0],
          rotation[3], rotation[4], rotation[5], translation[1],
          rotation[6], rotation[7], rotation[8], translation[2],
          0.0,         0.0,         0.0,         1.0};
}

RigidTransform RigidTransform::fromMatrix(std::span<const double> values) {
  if (values.size() != 16 && values.size() != 12) {
    throw std::invalid_argument("A transform needs 12 or 16 values (3x4 or 4x4, row-major)");
  }
  RigidTransform t;
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      t.rotation[3 * r + c] = values[4 * r + c];
    }
    t.translation[r] = values[4 * r + 3];
  }
  if (values.size() == 16 && (std::abs(values[12]) > 1e-9 || std::abs(values[13]) > 1e-9 ||
                              std::abs(values[14]) > 1e-9 || std::abs(values[15] - 1.0) > 1e-9)) {
    throw std::invalid_argument("The last row of a rigid transform must be 0 0 0 1");
  }
  const Mat identity = multiply(t.rotation, transpose(t.rotation));
  for (std::size_t i = 0; i < 9; ++i) {
    if (std::abs(identity[i] - (i % 4 == 0 ? 1.0 : 0.0)) > 1e-4) {
      throw std::invalid_argument("The rotation of a transform must be orthonormal");
    }
  }
  if (determinant(t.rotation) < 0.0) {
    throw std::invalid_argument("A transform must not mirror");
  }
  return t;
}

RigidTransform RigidTransform::fromAxisAngle(const std::array<double, 3>& axis, double degrees,
                                             const std::array<double, 3>& translation) {
  const double length = norm(axis);
  if (length <= 0.0) {
    throw std::invalid_argument("The rotation axis must not be zero");
  }
  RigidTransform t;
  t.rotation = rodrigues((degrees * std::numbers::pi / 180.0 / length) * axis);
  t.translation = translation;
  return t;
}

// ---------------------------------------------------------------------------------------------
// MeshDistance

struct MeshDistance::Impl {
  struct Triangle {
    Vec a, b, c;
    Vec normal;  // unit, outward
  };
  struct Node {
    Vec lo, hi;
    std::uint32_t first = 0;  // leaf: first triangle; inner node: left child
    std::uint32_t right = 0;  // inner node: right child
    std::uint32_t count = 0;  // leaf: triangle count; 0 for inner nodes
  };
  std::vector<Triangle> triangles;
  std::vector<Node> nodes;
  double eps2 = 0.0;

  static constexpr std::uint32_t kLeafSize = 4;

  std::uint32_t build(std::uint32_t first, std::uint32_t count) {
    const auto index = static_cast<std::uint32_t>(nodes.size());
    nodes.emplace_back();
    Node node;
    node.lo = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
               std::numeric_limits<double>::max()};
    node.hi = -1.0 * node.lo;
    for (std::uint32_t i = first; i < first + count; ++i) {
      for (const Vec& p : {triangles[i].a, triangles[i].b, triangles[i].c}) {
        for (std::size_t k = 0; k < 3; ++k) {
          node.lo[k] = std::min(node.lo[k], p[k]);
          node.hi[k] = std::max(node.hi[k], p[k]);
        }
      }
    }
    if (count <= kLeafSize) {
      node.first = first;
      node.count = count;
      nodes[index] = node;
      return index;
    }
    const Vec size = node.hi - node.lo;
    const std::size_t axis =
        size[0] >= size[1] && size[0] >= size[2] ? 0 : (size[1] >= size[2] ? 1 : 2);
    const auto begin = triangles.begin() + first;
    std::nth_element(begin, begin + count / 2, begin + count,
                     [axis](const Triangle& l, const Triangle& r) {
                       return l.a[axis] + l.b[axis] + l.c[axis] < r.a[axis] + r.b[axis] + r.c[axis];
                     });
    node.first = build(first, count / 2);
    node.right = build(first + count / 2, count - count / 2);
    nodes[index] = node;
    return index;
  }

  static double boxDistance2(const Node& node, const Vec& p) {
    double d2 = 0.0;
    for (std::size_t k = 0; k < 3; ++k) {
      const double d = std::max({node.lo[k] - p[k], 0.0, p[k] - node.hi[k]});
      d2 += d * d;
    }
    return d2;
  }

  /// Calls visit(triangle index) for every triangle in a node closer than the current bound.
  template <typename Visit>
  void traverse(const Vec& p, double& bound2, Visit visit) const {
    std::array<std::uint32_t, 64> stack{};
    std::size_t top = 0;
    stack[top++] = 0;
    while (top > 0) {
      const Node& node = nodes[stack[--top]];
      if (boxDistance2(node, p) > bound2) {
        continue;
      }
      if (node.count > 0) {
        for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
          visit(i);
        }
        continue;
      }
      std::uint32_t near = node.first;
      std::uint32_t far = node.right;
      if (boxDistance2(nodes[near], p) > boxDistance2(nodes[far], p)) {
        std::swap(near, far);
      }
      stack[top++] = far;
      stack[top++] = near;
    }
  }
};

MeshDistance::MeshDistance(const Mesh& mesh) : impl_(std::make_unique<Impl>()) {
  const double orientation = meshVolumeMm3(mesh) < 0.0 ? -1.0 : 1.0;
  impl_->triangles.reserve(mesh.triangles.size());
  Vec lo{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
         std::numeric_limits<double>::max()};
  Vec hi = -1.0 * lo;
  for (const auto& t : mesh.triangles) {
    Impl::Triangle tri;
    tri.a = {t[0][0], t[0][1], t[0][2]};
    tri.b = {t[1][0], t[1][1], t[1][2]};
    tri.c = {t[2][0], t[2][1], t[2][2]};
    const Vec n = cross(tri.b - tri.a, tri.c - tri.a);
    const double length = norm(n);
    if (!(length > 0.0) || !std::isfinite(length)) {
      continue;  // degenerate
    }
    tri.normal = (orientation / length) * n;
    for (const Vec& p : {tri.a, tri.b, tri.c}) {
      for (std::size_t k = 0; k < 3; ++k) {
        lo[k] = std::min(lo[k], p[k]);
        hi[k] = std::max(hi[k], p[k]);
      }
    }
    impl_->triangles.push_back(tri);
  }
  if (impl_->triangles.empty()) {
    throw std::invalid_argument("The CAD mesh has no triangles");
  }
  if (impl_->triangles.size() > std::numeric_limits<std::uint32_t>::max() / 4) {
    throw std::invalid_argument("The CAD mesh has too many triangles");
  }
  const double diagonal = norm(hi - lo);
  impl_->eps2 = 1e-20 * diagonal * diagonal;
  impl_->nodes.reserve(2 * impl_->triangles.size() / Impl::kLeafSize + 2);
  (void)impl_->build(0, static_cast<std::uint32_t>(impl_->triangles.size()));
}

MeshDistance::MeshDistance(MeshDistance&&) noexcept = default;
MeshDistance& MeshDistance::operator=(MeshDistance&&) noexcept = default;
MeshDistance::~MeshDistance() = default;

MeshDistance::Hit MeshDistance::query(const std::array<double, 3>& p) const {
  const Impl& impl = *impl_;
  double best2 = std::numeric_limits<double>::infinity();
  std::uint32_t best = 0;
  TrianglePoint best_point;
  impl.traverse(p, best2, [&](std::uint32_t i) {
    const auto& t = impl.triangles[i];
    const TrianglePoint q = closestOnTriangle(p, t.a, t.b, t.c);
    const Vec d = p - q.point;
    const double d2 = dot(d, d);
    if (d2 < best2) {
      best2 = d2;
      best = i;
      best_point = q;
    }
  });
  Hit hit;
  hit.closest = best_point.point;
  const Vec offset = p - best_point.point;
  const double distance = std::sqrt(best2);
  Vec normal = impl.triangles[best].normal;
  if (best_point.region != Region::kFace) {
    // On an edge or vertex: the side follows the angle-weighted normals of all triangles that
    // share the closest point.
    Vec sum{};
    double bound2 = best2 + impl.eps2 + 1e-9 * best2;
    impl.traverse(p, bound2, [&](std::uint32_t i) {
      const auto& t = impl.triangles[i];
      const TrianglePoint q = closestOnTriangle(p, t.a, t.b, t.c);
      const Vec d = p - q.point;
      if (dot(d, d) > bound2) {
        return;
      }
      double weight = 1.0;
      if (q.region == Region::kVertex) {
        const std::array<Vec, 3> v{t.a, t.b, t.c};
        const Vec e1 = v[(q.vertex + 1) % 3] - v[q.vertex];
        const Vec e2 = v[(q.vertex + 2) % 3] - v[q.vertex];
        weight = std::acos(std::clamp(dot(e1, e2) / (norm(e1) * norm(e2)), -1.0, 1.0));
      }
      sum = sum + weight * t.normal;
    });
    if (norm(sum) > 0.0) {
      normal = (1.0 / norm(sum)) * sum;
    }
  }
  const double side = dot(offset, normal) < 0.0 ? -1.0 : 1.0;
  hit.distance = side * distance;
  hit.normal = distance > 1e-12 ? (side / distance) * offset : normal;
  return hit;
}

// ---------------------------------------------------------------------------------------------

const char* toString(CompareOptions::Alignment alignment) {
  switch (alignment) {
    case CompareOptions::Alignment::kAuto:
      return "auto";
    case CompareOptions::Alignment::kRefine:
      return "refine";
    case CompareOptions::Alignment::kNone:
      return "none";
  }
  return "auto";
}

CompareOptions::Alignment alignmentFromString(const std::string& name) {
  if (name == "auto") {
    return CompareOptions::Alignment::kAuto;
  }
  if (name == "refine") {
    return CompareOptions::Alignment::kRefine;
  }
  if (name == "none") {
    return CompareOptions::Alignment::kNone;
  }
  throw std::invalid_argument("Unknown alignment '" + name + "' (auto, refine or none)");
}

std::array<std::uint8_t, 3> deviationColor(double deviation_mm, double tolerance_mm,
                                           double range_mm) {
  const double a = std::abs(deviation_mm);
  if (a <= tolerance_mm) {
    return {60, 190, 90};
  }
  const double t =
      std::clamp((a - tolerance_mm) / std::max(range_mm - tolerance_mm, 1e-12), 0.0, 1.0);
  const auto mix = [t](double from, double to) {
    return static_cast<std::uint8_t>(std::lround(from + (to - from) * t));
  };
  if (deviation_mm > 0.0) {
    return {mix(240, 215), mix(225, 30), mix(40, 30)};  // yellow to red
  }
  return {mix(40, 40), mix(205, 60), mix(240, 215)};  // cyan to blue
}

ScanSurface scanSurface(const SurfaceMask& mask, std::size_t max_triangles, bool outer_only) {
  const VoxelSize& v = mask.info().voxel_size;
  IndexedMesh surface = surfaceDisplayMesh(mask, max_triangles, 0.0);
  if (surface.triangles.empty()) {
    throw std::invalid_argument("The scan has no surface");
  }
  for (auto& p : surface.points) {
    for (std::size_t k = 0; k < 3; ++k) {
      p[k] = static_cast<float>(p[k] * v[k]);
    }
  }
  // Connected surfaces by union-find over the triangles.
  std::vector<std::uint32_t> parent(surface.points.size());
  std::iota(parent.begin(), parent.end(), 0U);
  const auto find = [&parent](std::uint32_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  for (const auto& t : surface.triangles) {
    const std::uint32_t a = find(t[0]);
    for (std::size_t k = 1; k < 3; ++k) {
      const std::uint32_t b = find(t[k]);
      if (a != b) {
        parent[b] = a;
      }
    }
  }
  const std::vector<double> triangle_area = triangleAreas(surface);
  std::unordered_map<std::uint32_t, double> component_area;
  for (std::size_t i = 0; i < surface.triangles.size(); ++i) {
    component_area[find(surface.triangles[i][0])] += triangle_area[i];
  }
  ScanSurface result;
  result.components = component_area.size();
  std::uint32_t outer = 0;
  double outer_area = -1.0;
  for (const auto& [root, area] : component_area) {
    if (area > outer_area) {
      outer = root;
      outer_area = area;
    }
  }
  std::vector<std::uint32_t> remap(surface.points.size(),
                                   std::numeric_limits<std::uint32_t>::max());
  for (std::size_t i = 0; i < surface.triangles.size(); ++i) {
    const auto& t = surface.triangles[i];
    if (outer_only && find(t[0]) != outer) {
      result.dropped_area_mm2 += triangle_area[i];
      continue;
    }
    std::array<std::uint32_t, 3> mapped{};
    for (std::size_t k = 0; k < 3; ++k) {
      if (remap[t[k]] == std::numeric_limits<std::uint32_t>::max()) {
        remap[t[k]] = static_cast<std::uint32_t>(result.mesh.points.size());
        result.mesh.points.push_back(surface.points[t[k]]);
      }
      mapped[k] = remap[t[k]];
    }
    result.mesh.triangles.push_back(mapped);
  }
  result.dropped_components = outer_only ? result.components - 1 : 0;
  return result;
}

IndexedMesh indexedMesh(const Mesh& mesh) {
  IndexedMesh indexed;
  std::map<std::array<float, 3>, std::uint32_t> known;
  indexed.triangles.reserve(mesh.triangles.size());
  for (const auto& triangle : mesh.triangles) {
    std::array<std::uint32_t, 3> corners{};
    for (std::size_t k = 0; k < 3; ++k) {
      const auto [it, added] =
          known.try_emplace(triangle[k], static_cast<std::uint32_t>(indexed.points.size()));
      if (added) {
        indexed.points.push_back(triangle[k]);
      }
      corners[k] = it->second;
    }
    indexed.triangles.push_back(corners);
  }
  return indexed;
}

Mesh triangleSoup(const IndexedMesh& mesh) {
  Mesh soup;
  soup.triangles.reserve(mesh.triangles.size());
  for (const auto& t : mesh.triangles) {
    soup.triangles.push_back({mesh.points[t[0]], mesh.points[t[1]], mesh.points[t[2]]});
  }
  return soup;
}

IndexedMesh transformed(IndexedMesh mesh, const RigidTransform& transform) {
  for (auto& p : mesh.points) {
    const Vec q = transform.apply(toVec(p));
    p = {static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2])};
  }
  return mesh;
}

RigidFit fitRigid(std::span<const std::array<double, 3>> from,
                  std::span<const std::array<double, 3>> to) {
  if (from.size() != to.size()) {
    throw std::invalid_argument("Every point needs a partner");
  }
  if (from.size() < 3) {
    throw std::invalid_argument("A rigid fit needs at least three point pairs");
  }
  const auto n = static_cast<double>(from.size());
  Vec a_mean{};
  Vec b_mean{};
  for (std::size_t i = 0; i < from.size(); ++i) {
    a_mean = a_mean + (1.0 / n) * from[i];
    b_mean = b_mean + (1.0 / n) * to[i];
  }
  // The points must span a plane on both sides, else a rotation about their line is free.
  Mat spread_a{};
  Mat spread_b{};
  Mat s{};  // sum of (a - a_mean)(b - b_mean)^T
  for (std::size_t i = 0; i < from.size(); ++i) {
    const Vec a = from[i] - a_mean;
    const Vec b = to[i] - b_mean;
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 3; ++c) {
        s[3 * r + c] += a[r] * b[c];
        spread_a[3 * r + c] += a[r] * a[c];
        spread_b[3 * r + c] += b[r] * b[c];
      }
    }
  }
  for (const Mat& spread : {spread_a, spread_b}) {
    const Vec values = symmetricEigen(spread).second;
    if (!(values[1] > 1e-10 * std::max(values[0], 1e-300))) {
      throw std::invalid_argument("The points of a rigid fit must not lie on a line");
    }
  }
  // Horn (1987): the rotation is the unit quaternion of the largest eigenvalue of N.
  const auto at = [&s](std::size_t r, std::size_t c) { return s[3 * r + c]; };
  const std::array<double, 16> nmat{at(0, 0) + at(1, 1) + at(2, 2),
                                    at(1, 2) - at(2, 1),
                                    at(2, 0) - at(0, 2),
                                    at(0, 1) - at(1, 0),
                                    at(1, 2) - at(2, 1),
                                    at(0, 0) - at(1, 1) - at(2, 2),
                                    at(0, 1) + at(1, 0),
                                    at(2, 0) + at(0, 2),
                                    at(2, 0) - at(0, 2),
                                    at(0, 1) + at(1, 0),
                                    -at(0, 0) + at(1, 1) - at(2, 2),
                                    at(1, 2) + at(2, 1),
                                    at(0, 1) - at(1, 0),
                                    at(2, 0) + at(0, 2),
                                    at(1, 2) + at(2, 1),
                                    -at(0, 0) - at(1, 1) + at(2, 2)};
  const auto q = largestEigenvector4(nmat);
  const double w = q[0];
  const double x = q[1];
  const double y = q[2];
  const double z = q[3];
  const double length2 = w * w + x * x + y * y + z * z;
  RigidFit fit;
  fit.transform.rotation = {
      (w * w + x * x - y * y - z * z) / length2, 2.0 * (x * y - w * z) / length2,
      2.0 * (x * z + w * y) / length2,           2.0 * (x * y + w * z) / length2,
      (w * w - x * x + y * y - z * z) / length2, 2.0 * (y * z - w * x) / length2,
      2.0 * (x * z - w * y) / length2,           2.0 * (y * z + w * x) / length2,
      (w * w - x * x - y * y + z * z) / length2};
  fit.transform.translation = b_mean - fit.transform.rotate(a_mean);
  double sum = 0.0;
  for (std::size_t i = 0; i < from.size(); ++i) {
    const double residual = norm(fit.transform.apply(from[i]) - to[i]);
    fit.residuals_mm.push_back(residual);
    sum += residual * residual;
  }
  fit.rms_mm = std::sqrt(sum / n);
  return fit;
}

SurfaceAlignment alignSurfaces(const IndexedMesh& moving, const IndexedMesh& target,
                               const SurfaceAlignOptions& options) {
  if (moving.triangles.empty() || target.triangles.empty()) {
    throw std::invalid_argument("Both sides of a surface fit need a surface");
  }
  if (options.fit_points < 100) {
    throw std::invalid_argument("At least 100 fit points are needed");
  }
  const MeshDistance distance(triangleSoup(target));
  const std::vector<Vec> samples = sampleByArea(moving, triangleAreas(moving), options.fit_points);
  double floor_mm = options.resolution_mm;
  if (!(floor_mm > 0.0)) {
    // Without a voxel size (two meshes): a ten-thousandth of the target's size.
    Vec lo{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
           std::numeric_limits<double>::max()};
    Vec hi = -1.0 * lo;
    for (const auto& p : target.points) {
      for (std::size_t k = 0; k < 3; ++k) {
        lo[k] = std::min(lo[k], static_cast<double>(p[k]));
        hi[k] = std::max(hi[k], static_cast<double>(p[k]));
      }
    }
    floor_mm = 1e-4 * std::max(norm(hi - lo), 1e-9);
  }
  RigidTransform start;
  if (options.coarse) {
    const TelemetryPhase phase("coarse alignment");
    start = coarseAlignment(distance, samples, meshMoments(moving), meshMoments(target), floor_mm);
  }
  const TelemetryPhase phase("fine alignment");
  const Fit fit = icp(distance, samples, start, 100, floor_mm);
  SurfaceAlignment result;
  result.motion = fit.transform;
  result.rms_mm = fit.rms;
  result.inliers = fit.inliers;
  result.iterations = fit.iterations;
  result.resolution_mm = floor_mm;
  return result;
}

CompareResult compareToCad(const SurfaceMask& mask, const Mesh& cad,
                           const CompareOptions& options) {
  if (!(options.tolerance_mm > 0.0)) {
    throw std::invalid_argument("The tolerance must be positive");
  }
  if (options.fit_points < 100) {
    throw std::invalid_argument("At least 100 fit points are needed");
  }
  CompareResult result;
  result.alignment = options.alignment;
  result.tolerance_mm = options.tolerance_mm;
  result.voxel_size = mask.info().voxel_size;
  result.cad_triangles = cad.triangles.size();
  const VoxelSize& v = result.voxel_size;
  std::optional<TelemetryPhase> phase(std::in_place, "surface mesh");
  const MeshDistance nominal(cad);

  // The scanned surface in mm, without the surfaces of internal voids when asked.
  ScanSurface scanned = scanSurface(mask, options.max_triangles, options.outer_surface_only);
  result.mesh = std::move(scanned.mesh);
  result.components = scanned.components;
  result.dropped_components = scanned.dropped_components;
  result.dropped_area_mm2 = scanned.dropped_area_mm2;
  const std::vector<Vec> samples =
      sampleByArea(result.mesh, triangleAreas(result.mesh), options.fit_points);

  // Alignment, as scan to CAD.
  phase.reset();
  phase.emplace("alignment");
  const double floor_mm = 0.5 * v.minMm();
  RigidTransform scan_to_cad = options.initial.inverse();
  if (options.alignment == CompareOptions::Alignment::kAuto) {
    scan_to_cad =
        coarseAlignment(nominal, samples, meshMoments(result.mesh), meshMoments(cad), floor_mm);
  }
  if (options.alignment != CompareOptions::Alignment::kNone) {
    const Fit fit = icp(nominal, samples, scan_to_cad, 100, floor_mm);
    scan_to_cad = fit.transform;
    result.fit_rms_mm = fit.rms;
    result.fit_inliers = fit.inliers;
    result.fit_iterations = fit.iterations;
  } else {
    const Fit fit = icp(nominal, samples, scan_to_cad, 0, floor_mm);
    result.fit_rms_mm = fit.rms;
    result.fit_inliers = fit.inliers;
  }
  result.cad_to_scan = scan_to_cad.inverse();

  // Deviation of every vertex of the scanned surface.
  phase.reset();
  phase.emplace("deviation");
  result.deviation_mm.resize(result.mesh.points.size());
  tbb::parallel_for(tbb::blocked_range<std::size_t>(0, result.mesh.points.size(), 1024),
                    [&](const tbb::blocked_range<std::size_t>& range) {
                      for (std::size_t i = range.begin(); i != range.end(); ++i) {
                        const auto& p = result.mesh.points[i];
                        result.deviation_mm[i] = static_cast<float>(
                            nominal.query(scan_to_cad.apply({p[0], p[1], p[2]})).distance);
                      }
                    });
  result.stats = deviationStats(result.mesh, result.deviation_mm, options.tolerance_mm);
  return result;
}

nlohmann::json toJson(const CompareResult& result) {
  const DeviationStats& s = result.stats;
  Json percentiles = Json::object();
  for (std::size_t i = 0; i < kDeviationPercentiles.size(); ++i) {
    percentiles[std::to_string(static_cast<int>(kDeviationPercentiles[i]))] = s.percentiles_mm[i];
  }
  return {{"alignment", toString(result.alignment)},
          {"cad_to_scan", result.cad_to_scan.matrix()},
          {"scan_to_cad", result.cad_to_scan.inverse().matrix()},
          {"rotation_deg", result.cad_to_scan.angleDegrees()},
          {"translation_mm", result.cad_to_scan.translation},
          {"fit",
           {{"rms_mm", result.fit_rms_mm},
            {"inliers", result.fit_inliers},
            {"iterations", result.fit_iterations}}},
          {"tolerance_mm", result.tolerance_mm},
          {"voxel_size_mm", result.voxel_size},
          {"cad_triangles", result.cad_triangles},
          {"components", result.components},
          {"dropped_components", result.dropped_components},
          {"dropped_area_mm2", result.dropped_area_mm2},
          {"deviation",
           {{"vertices", s.vertices},
            {"area_mm2", s.area_mm2},
            {"mean_mm", s.mean_mm},
            {"rms_mm", s.rms_mm},
            {"std_mm", s.std_mm},
            {"min_mm", s.min_mm},
            {"max_mm", s.max_mm},
            {"percentiles_mm", percentiles},
            {"within_tolerance", s.within_tolerance},
            {"above_tolerance", s.above_tolerance},
            {"below_tolerance", s.below_tolerance},
            {"range_mm", s.range_mm},
            {"histogram", s.histogram}}}};
}

void writeComparison(const CompareResult& result, const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
  writeJson(dir / "compare.json", toJson(result));
  writePly(dir / "deviation.ply", result);
  writeImages(result, dir);
}

void writeAlignedCad(const CompareResult& result, const Mesh& cad,
                     const std::filesystem::path& file) {
  Mesh aligned;
  aligned.triangles.reserve(cad.triangles.size());
  for (const auto& t : cad.triangles) {
    auto& out = aligned.triangles.emplace_back();
    for (std::size_t k = 0; k < 3; ++k) {
      const Vec p = result.cad_to_scan.apply({t[k][0], t[k][1], t[k][2]});
      out[k] = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
    }
  }
  writeStl(file, aligned);
}

DeviationMesh readDeviationPly(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot open " + file.string());
  }
  DeviationMesh out;
  try {
    tinyply::PlyFile ply;
    if (!ply.parse_header(in)) {
      throw std::runtime_error("no PLY header");
    }
    const auto points = ply.request_properties_from_element("vertex", {"x", "y", "z"});
    const auto deviation = ply.request_properties_from_element("vertex", {"deviation"});
    const auto faces = ply.request_properties_from_element("face", {"vertex_indices"}, 3);
    ply.read(in);
    if (points->t != tinyply::Type::FLOAT32 || deviation->t != tinyply::Type::FLOAT32 ||
        (faces->t != tinyply::Type::INT32 && faces->t != tinyply::Type::UINT32) ||
        deviation->count != points->count) {
      throw std::runtime_error("unexpected property types");
    }
    out.mesh.points.resize(points->count);
    std::memcpy(out.mesh.points.data(), points->buffer.get(), points->buffer.size_bytes());
    out.deviation_mm.resize(deviation->count);
    std::memcpy(out.deviation_mm.data(), deviation->buffer.get(), deviation->buffer.size_bytes());
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
  } catch (const std::exception& e) {
    throw std::runtime_error("Not a deviation PLY written by VoxelSieve: " + file.string() + " (" +
                             e.what() + ")");
  }
  return out;
}

}  // namespace voxelsieve
