#include "voxelsieve/synthetic.hpp"

#include <openvdb/openvdb.h>
#include <openvdb/tools/Interpolation.h>
#include <openvdb/tools/MeshToVolume.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include "detail/noise.hpp"

namespace voxelsieve {
namespace {

/// Separable Gaussian blur of `values` (the voxels of `region`, x fastest) with `sigma` voxels,
/// truncated at `radius`. Outside the volume the border voxel is repeated; inside it, the region
/// must reach `radius` beyond the voxels whose result is used.
void blurRegion(std::vector<double>& values, const Box& region, double sigma, std::int64_t radius) {
  std::vector<double> weights(static_cast<std::size_t>(2 * radius + 1));
  double total = 0.0;
  for (std::int64_t i = -radius; i <= radius; ++i) {
    const double w = std::exp(-0.5 * static_cast<double>(i * i) / (sigma * sigma));
    weights[static_cast<std::size_t>(i + radius)] = w;
    total += w;
  }
  for (double& w : weights) {
    w /= total;
  }
  const std::array<std::int64_t, 3> size{region.size(0), region.size(1), region.size(2)};
  const std::array<std::int64_t, 3> stride{1, size[0], size[0] * size[1]};
  std::vector<double> line;
  std::vector<double> result;
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const std::size_t u = axis == 0 ? 1 : 0;
    const std::size_t v = 3 - axis - u;
    const std::int64_t n = size[axis];
    line.resize(static_cast<std::size_t>(n));
    result.resize(static_cast<std::size_t>(n));
    for (std::int64_t b = 0; b < size[v]; ++b) {
      for (std::int64_t a = 0; a < size[u]; ++a) {
        const std::int64_t start = a * stride[u] + b * stride[v];
        for (std::int64_t i = 0; i < n; ++i) {
          line[static_cast<std::size_t>(i)] =
              values[static_cast<std::size_t>(start + i * stride[axis])];
        }
        for (std::int64_t i = 0; i < n; ++i) {
          double sum = 0.0;
          for (std::int64_t k = -radius; k <= radius; ++k) {
            // Clamp to the region; at the volume border that repeats the border voxel, elsewhere
            // the halo keeps the clamped samples away from the voxels that are used.
            const std::int64_t j = std::clamp<std::int64_t>(i + k, 0, n - 1);
            sum +=
                weights[static_cast<std::size_t>(k + radius)] * line[static_cast<std::size_t>(j)];
          }
          result[static_cast<std::size_t>(i)] = sum;
        }
        for (std::int64_t i = 0; i < n; ++i) {
          values[static_cast<std::size_t>(start + i * stride[axis])] =
              result[static_cast<std::size_t>(i)];
        }
      }
    }
  }
}

using Vec3 = std::array<double, 3>;

constexpr double kHalfBandVoxels = 3.0;
/// Sub-samples per axis for voxels touched by a defect.
constexpr int kSuperSampling = 3;
/// Longest axis of the coarse distance field used for depth queries.
constexpr double kCoarseResolution = 128.0;
constexpr int kLunkerMonteCarloSamples = 200000;
constexpr double kSphereVolumeFactor = 4.0 / 3.0 * std::numbers::pi;

double distance(const Vec3& a, const Vec3& b) {
  const double dx = a[0] - b[0];
  const double dy = a[1] - b[1];
  const double dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double sphereVolume(double radius) { return kSphereVolumeFactor * radius * radius * radius; }

/// Deterministic random sequence that is identical on every platform.
class Random {
 public:
  explicit Random(std::uint64_t seed) : seed_(detail::splitMix64(seed ^ 0x5EED5EEDULL)) {}
  double uniform() { return detail::uniformNoise(seed_, counter_++); }
  double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }
  Vec3 inBox(const Vec3& lo, const Vec3& hi) {
    return {uniform(lo[0], hi[0]), uniform(lo[1], hi[1]), uniform(lo[2], hi[2])};
  }
  /// Uniform point inside a ball.
  Vec3 inBall(const Vec3& center, double radius) {
    while (true) {
      const Vec3 p{uniform(-1.0, 1.0), uniform(-1.0, 1.0), uniform(-1.0, 1.0)};
      if (p[0] * p[0] + p[1] * p[1] + p[2] * p[2] <= 1.0) {
        return {center[0] + radius * p[0], center[1] + radius * p[1], center[2] + radius * p[2]};
      }
    }
  }

 private:
  std::uint64_t seed_;
  std::uint64_t counter_ = 0;
};

/// Sparse uniform grid of small spheres. Each sphere is registered in every cell that its
/// bounding box, grown by `reach`, overlaps, so one cell lookup finds all spheres within `reach`
/// of a point.
class SphereGrid {
 public:
  SphereGrid(double cell_size, double reach) : cell_(cell_size), reach_(reach) {}

  void insert(const Sphere& sphere) {
    const auto index = static_cast<std::uint32_t>(spheres_.size());
    spheres_.push_back(sphere);
    const double half = sphere.radius_mm + reach_;
    std::array<std::int64_t, 3> lo{};
    std::array<std::int64_t, 3> hi{};
    for (std::size_t i = 0; i < 3; ++i) {
      lo[i] = cellOf(sphere.center_mm[i] - half);
      hi[i] = cellOf(sphere.center_mm[i] + half);
    }
    for (std::int64_t z = lo[2]; z <= hi[2]; ++z) {
      for (std::int64_t y = lo[1]; y <= hi[1]; ++y) {
        for (std::int64_t x = lo[0]; x <= hi[0]; ++x) {
          cells_[key(x, y, z)].push_back(index);
        }
      }
    }
  }

  /// Spheres whose surface may lie within `reach` of `point`.
  [[nodiscard]] const std::vector<std::uint32_t>* near(const Vec3& point) const {
    const auto it = cells_.find(key(cellOf(point[0]), cellOf(point[1]), cellOf(point[2])));
    return it == cells_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] const Sphere& sphere(std::uint32_t index) const { return spheres_[index]; }

 private:
  [[nodiscard]] std::int64_t cellOf(double coordinate) const {
    return static_cast<std::int64_t>(std::floor(coordinate / cell_));
  }
  static std::uint64_t key(std::int64_t x, std::int64_t y, std::int64_t z) {
    constexpr std::int64_t kOffset = std::int64_t{1} << 20;
    constexpr std::uint64_t kMask = (std::uint64_t{1} << 21U) - 1;
    return (static_cast<std::uint64_t>(x + kOffset) & kMask) |
           ((static_cast<std::uint64_t>(y + kOffset) & kMask) << 21U) |
           ((static_cast<std::uint64_t>(z + kOffset) & kMask) << 42U);
  }

  double cell_;
  double reach_;
  std::vector<Sphere> spheres_;
  std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells_;
};

/// Antiderivative of sqrt(rho^2 - t^2) for |t| <= rho.
double chordIntegral(double t, double rho) {
  return 0.5 * (t * std::sqrt(std::max(rho * rho - t * t, 0.0)) +
                rho * rho * std::asin(std::clamp(t / rho, -1.0, 1.0)));
}

/// Area of the disk of radius `rho` around the origin with X <= x and Y <= y.
double diskQuadrantArea(double x, double y, double rho) {
  if (x <= -rho || y <= -rho) {
    return 0.0;
  }
  x = std::min(x, rho);
  if (y >= rho) {
    // Everything left of x.
    return 2.0 * (chordIntegral(x, rho) - chordIntegral(-rho, rho));
  }
  // Where |t| >= w the chord lies entirely above y (y < 0) or below y (y > 0).
  const double w = std::sqrt(rho * rho - y * y);
  const auto chord = [&](double a, double b) {  // integral of the chord clipped at y, |t| < w
    return b > a ? y * (b - a) + chordIntegral(b, rho) - chordIntegral(a, rho) : 0.0;
  };
  const auto full = [&](double a, double b) {  // integral of the full chord length
    return b > a ? 2.0 * (chordIntegral(b, rho) - chordIntegral(a, rho)) : 0.0;
  };
  double area = chord(-w, std::min(x, w));
  if (y > 0.0) {
    area += full(-rho, std::min(x, -w)) + full(w, x);
  }
  return area;
}

/// Exact volume of a sphere inside an axis-aligned box: the disk-rectangle area of each slice is
/// exact, integrated over z with Gauss-Legendre on the slices the sphere covers.
double sphereBoxOverlap(const Sphere& sphere, const Vec3& lo, const Vec3& hi) {
  const auto& c = sphere.center_mm;
  const double r = sphere.radius_mm;
  const double z0 = std::max(lo[2], c[2] - r);
  const double z1 = std::min(hi[2], c[2] + r);
  if (z1 <= z0) {
    return 0.0;
  }
  const double x0 = lo[0] - c[0];
  const double x1 = hi[0] - c[0];
  const double y0 = lo[1] - c[1];
  const double y1 = hi[1] - c[1];
  // 8-point Gauss-Legendre on 4 sub-intervals.
  constexpr std::array<double, 4> kNodes{0.1834346424956498, 0.5255324099163290, 0.7966664774136267,
                                         0.9602898564975363};
  constexpr std::array<double, 4> kWeights{0.3626837833783620, 0.3137066458778873,
                                           0.2223810344533745, 0.1012285362903763};
  constexpr int kIntervals = 4;
  const double step = (z1 - z0) / kIntervals;
  double volume = 0.0;
  for (int interval = 0; interval < kIntervals; ++interval) {
    const double mid = z0 + (interval + 0.5) * step;
    for (std::size_t n = 0; n < kNodes.size(); ++n) {
      for (const double sign : {-1.0, 1.0}) {
        const double z = mid + sign * kNodes[n] * step / 2.0 - c[2];
        const double rho = std::sqrt(std::max(r * r - z * z, 0.0));
        if (rho <= 0.0) {
          continue;
        }
        const double area = diskQuadrantArea(x1, y1, rho) - diskQuadrantArea(x0, y1, rho) -
                            diskQuadrantArea(x1, y0, rho) + diskQuadrantArea(x0, y0, rho);
        volume += kWeights[n] * area * step / 2.0;
      }
    }
  }
  return volume;
}

struct Ring {
  double radius_voxels = 0.0;
  double amplitude = 0.0;
};

}  // namespace

std::string toString(DefectType type) {
  return type == DefectType::kLunker ? "lunker" : "loosening";
}

struct SyntheticScan::Impl {
  SyntheticSpec spec;
  Bounds bounds;
  double mesh_volume_mm3 = 0.0;
  Vec3 origin{};
  std::array<std::int64_t, 3> dims{};
  double coarse_voxel_mm = 0.0;
  double reach_mm = 0.0;           // how far from a voxel centre a defect can change its grey value
  openvdb::FloatGrid::Ptr fine;    // narrow-band signed distance in mm
  openvdb::FloatGrid::Ptr coarse;  // signed distance with a full interior, for depth
  std::vector<Defect> defects;
  std::unique_ptr<SphereGrid> micro_pores;
  std::vector<Ring> rings;
  std::vector<std::string> warnings;

  /// Per-thread read access to the distance fields.
  struct Evaluator {
    const Impl& impl;
    openvdb::FloatGrid::ConstAccessor fine;
    openvdb::FloatGrid::ConstAccessor coarse;

    explicit Evaluator(const Impl& owner)
        : impl(owner),
          fine(owner.fine->getConstAccessor()),
          coarse(owner.coarse->getConstAccessor()) {}

    [[nodiscard]] double meshDistance(const Vec3& p) {
      const openvdb::Vec3d index = impl.fine->transform().worldToIndex({p[0], p[1], p[2]});
      return openvdb::tools::BoxSampler::sample(fine, index);
    }
    [[nodiscard]] double meshDistanceAt(const openvdb::Coord& voxel) {
      return fine.getValue(voxel);
    }
    /// Distance below the surface, from the coarse field; 0 outside the material.
    [[nodiscard]] double depth(const Vec3& p) {
      const openvdb::Vec3d index = impl.coarse->transform().worldToIndex({p[0], p[1], p[2]});
      return std::max(0.0, -static_cast<double>(openvdb::tools::BoxSampler::sample(coarse, index)));
    }
    /// Signed distance to the nearest lunker lobe, or +infinity.
    [[nodiscard]] double lunkerDistance(const Vec3& p) const {
      double result = std::numeric_limits<double>::infinity();
      for (const Defect& d : impl.defects) {
        // The enclosing sphere bounds the distance to every lobe from below.
        if (d.type != DefectType::kLunker || distance(p, d.center_mm) - d.radius_mm >= result) {
          continue;
        }
        for (const Sphere& s : d.spheres) {
          result = std::min(result, distance(p, s.center_mm) - s.radius_mm);
        }
      }
      return result;
    }
    /// Loosening pores that may reach within `reach_mm` of `point`, or nullptr.
    [[nodiscard]] const std::vector<std::uint32_t>* microPores(const Vec3& point) const {
      return impl.micro_pores ? impl.micro_pores->near(point) : nullptr;
    }
    /// Signed distance to the nearest void (lunker or loosening pore), or +infinity.
    [[nodiscard]] double voidDistance(const Vec3& p) const {
      double result = lunkerDistance(p);
      if (const auto* candidates = microPores(p)) {
        for (const std::uint32_t i : *candidates) {
          const Sphere& s = impl.micro_pores->sphere(i);
          result = std::min(result, distance(p, s.center_mm) - s.radius_mm);
        }
      }
      return result;
    }
    [[nodiscard]] double signedDistance(const Vec3& p) {
      return std::max(meshDistance(p), -voidDistance(p));
    }

    /// Material fraction of a voxel in [0, 1].
    [[nodiscard]] double fraction(std::int64_t x, std::int64_t y, std::int64_t z) {
      const double v = impl.spec.voxel_size_mm;
      const openvdb::Coord voxel(static_cast<int>(x), static_cast<int>(y), static_cast<int>(z));
      const double d_mesh = meshDistanceAt(voxel);
      double result = std::clamp(0.5 - d_mesh / v, 0.0, 1.0);
      if (result <= 0.0) {
        return 0.0;  // air: defects lie inside the material only
      }
      const Vec3 center = impl.voxelCenter(x, y, z);
      if (lunkerDistance(center) <= impl.reach_mm) {
        // Lunker lobes overlap, so supersample with a correspondingly narrower ramp.
        const double sub = v / kSuperSampling;
        double sum = 0.0;
        for (int k = 0; k < kSuperSampling; ++k) {
          for (int j = 0; j < kSuperSampling; ++j) {
            for (int i = 0; i < kSuperSampling; ++i) {
              const Vec3 p{center[0] + (i - 1) * sub, center[1] + (j - 1) * sub,
                           center[2] + (k - 1) * sub};
              const double d = std::max(meshDistance(p), -lunkerDistance(p));
              sum += std::clamp(0.5 - d / sub, 0.0, 1.0);
            }
          }
        }
        result = sum / (kSuperSampling * kSuperSampling * kSuperSampling);
      }
      // Loosening pores lie inside the material and touch neither each other nor a lunker, so
      // their exact overlap with the voxel can be subtracted. This keeps pores below the voxel
      // size volume-true.
      if (const auto* candidates = microPores(center)) {
        const double h = v / 2.0;
        const Vec3 lo{center[0] - h, center[1] - h, center[2] - h};
        const Vec3 hi{center[0] + h, center[1] + h, center[2] + h};
        for (const std::uint32_t i : *candidates) {
          result -= sphereBoxOverlap(impl.micro_pores->sphere(i), lo, hi) / (v * v * v);
        }
      }
      return std::max(result, 0.0);
    }
  };

  [[nodiscard]] Vec3 voxelCenter(std::int64_t x, std::int64_t y, std::int64_t z) const {
    const double v = spec.voxel_size_mm;
    return {origin[0] + static_cast<double>(x) * v, origin[1] + static_cast<double>(y) * v,
            origin[2] + static_cast<double>(z) * v};
  }

  [[nodiscard]] double smallestExtent() const {
    return std::min({bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1],
                     bounds.max[2] - bounds.min[2]});
  }

  [[nodiscard]] double ringOffset(std::int64_t x, std::int64_t y) const {
    if (rings.empty()) {
      return 0.0;
    }
    constexpr double kWidth = 0.7;  // voxels
    const double cx = static_cast<double>(dims[0] - 1) / 2.0;
    const double cy = static_cast<double>(dims[1] - 1) / 2.0;
    const double r = std::hypot(static_cast<double>(x) - cx, static_cast<double>(y) - cy);
    double offset = 0.0;
    for (const Ring& ring : rings) {
      const double t = (r - ring.radius_voxels) / kWidth;
      if (std::abs(t) < 4.0) {
        offset += ring.amplitude * std::exp(-t * t);
      }
    }
    return offset;
  }

  void buildDistanceFields(const Mesh& mesh) {
    // Merge identical vertices so that the triangles form a connected surface.
    std::map<std::array<float, 3>, std::uint32_t> index_of;
    std::vector<openvdb::Vec3s> points;
    std::vector<openvdb::Vec3I> triangles;
    triangles.reserve(mesh.triangles.size());
    for (const auto& triangle : mesh.triangles) {
      openvdb::Vec3I indices;
      for (std::size_t i = 0; i < 3; ++i) {
        const auto [it, inserted] =
            index_of.emplace(triangle[i], static_cast<std::uint32_t>(points.size()));
        if (inserted) {
          points.emplace_back(triangle[i][0], triangle[i][1], triangle[i][2]);
        }
        indices[static_cast<int>(i)] = it->second;
      }
      triangles.push_back(indices);
    }

    const double v = spec.voxel_size_mm;
    auto fine_transform = openvdb::math::Transform::createLinearTransform(v);
    fine_transform->postTranslate({origin[0], origin[1], origin[2]});
    fine = openvdb::tools::meshToLevelSet<openvdb::FloatGrid>(*fine_transform, points, triangles,
                                                              static_cast<float>(kHalfBandVoxels));

    double longest = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
      longest = std::max(longest, bounds.max[i] - bounds.min[i]);
    }
    coarse_voxel_mm = std::max(v, longest / kCoarseResolution);
    auto coarse_transform = openvdb::math::Transform::createLinearTransform(coarse_voxel_mm);
    coarse_transform->postTranslate({origin[0], origin[1], origin[2]});
    const auto interior = static_cast<float>(longest / coarse_voxel_mm + 4.0);
    coarse = openvdb::tools::meshToSignedDistanceField<openvdb::FloatGrid>(
        *coarse_transform, points, triangles, std::vector<openvdb::Vec4I>(),
        static_cast<float>(kHalfBandVoxels), interior);
  }

  [[nodiscard]] bool overlapsDefect(const Vec3& center, double radius) const {
    return std::any_of(defects.begin(), defects.end(), [&](const Defect& d) {
      return distance(center, d.center_mm) < radius + d.radius_mm + 2.0 * spec.voxel_size_mm;
    });
  }

  /// Random centre whose depth leaves room for a ball of `radius`, or nothing after many tries.
  std::optional<Vec3> placeBall(Random& random, double radius, double min_depth) {
    Evaluator eval(*this);
    for (int attempt = 0; attempt < 2000; ++attempt) {
      const Vec3 c = random.inBox(bounds.min, bounds.max);
      if (eval.depth(c) > min_depth && !overlapsDefect(c, radius)) {
        return c;
      }
    }
    return std::nullopt;
  }

  void placeLunkers(Random& random) {
    const double radius =
        spec.lunker_radius_mm > 0.0 ? spec.lunker_radius_mm : 0.05 * smallestExtent();
    for (int n = 0; n < spec.lunker_count; ++n) {
      const auto center = placeBall(random, radius, radius + 1.5 * coarse_voxel_mm);
      if (!center) {
        warnings.push_back("Placed only " + std::to_string(n) + " of " +
                           std::to_string(spec.lunker_count) + " lunkers of radius " +
                           std::to_string(radius) + " mm; the part is too thin");
        return;
      }
      Defect defect{DefectType::kLunker, *center, radius, {}, 0.0};
      // A core sphere plus lobes that stay inside the enclosing radius: an irregular cavity.
      defect.spheres.push_back({*center, 0.6 * radius});
      const int lobes = 2 + static_cast<int>(random.uniform() * 4.0);
      for (int i = 0; i < lobes; ++i) {
        const double r = random.uniform(0.3, 0.55) * radius;
        defect.spheres.push_back({random.inBall(*center, radius - r), r});
      }
      // Void volume of the union of spheres, by Monte Carlo in the enclosing cube.
      int inside = 0;
      const Vec3 lo{(*center)[0] - radius, (*center)[1] - radius, (*center)[2] - radius};
      const Vec3 hi{(*center)[0] + radius, (*center)[1] + radius, (*center)[2] + radius};
      for (int i = 0; i < kLunkerMonteCarloSamples; ++i) {
        const Vec3 p = random.inBox(lo, hi);
        if (std::any_of(defect.spheres.begin(), defect.spheres.end(),
                        [&](const Sphere& s) { return distance(p, s.center_mm) < s.radius_mm; })) {
          ++inside;
        }
      }
      defect.void_volume_mm3 = std::pow(2.0 * radius, 3) * inside / kLunkerMonteCarloSamples;
      defects.push_back(std::move(defect));
    }
  }

  void placeLoosening(Random& random) {
    const double v = spec.voxel_size_mm;
    const double zone =
        spec.loosening_radius_mm > 0.0 ? spec.loosening_radius_mm : 0.1 * smallestExtent();
    const double pore =
        spec.loosening_pore_radius_mm > 0.0 ? spec.loosening_pore_radius_mm : 0.6 * v;
    micro_pores = std::make_unique<SphereGrid>(2.0 * (pore + reach_mm), reach_mm);
    Evaluator eval(*this);
    for (int n = 0; n < spec.loosening_count; ++n) {
      const auto center = placeBall(random, zone, 0.5 * zone);
      if (!center) {
        warnings.push_back("Placed only " + std::to_string(n) + " of " +
                           std::to_string(spec.loosening_count) + " loosening zones of radius " +
                           std::to_string(zone) + " mm; the part is too thin");
        return;
      }
      Defect defect{DefectType::kLoosening, *center, zone, {}, 0.0};
      const double target = spec.loosening_porosity * sphereVolume(zone);
      const auto wanted = static_cast<std::size_t>(target / sphereVolume(pore));
      for (std::size_t attempt = 0; attempt < 4 * wanted && defect.spheres.size() < wanted;
           ++attempt) {
        const Vec3 c = random.inBall(*center, zone);
        // Fully inside the material (the fine field is exact near the surface) ...
        if (eval.meshDistance(c) > -(pore + v)) {
          continue;
        }
        // ... and not touching another pore or a lunker.
        if (eval.voidDistance(c) < pore + 0.25 * v) {
          continue;
        }
        const Sphere sphere{c, pore};
        micro_pores->insert(sphere);
        defect.spheres.push_back(sphere);
      }
      defect.void_volume_mm3 = static_cast<double>(defect.spheres.size()) * sphereVolume(pore);
      defects.push_back(std::move(defect));
    }
  }

  void placeRings(Random& random) {
    const double max_radius =
        std::hypot(static_cast<double>(dims[0]) / 2.0, static_cast<double>(dims[1]) / 2.0);
    for (int i = 0; i < spec.ring_count; ++i) {
      // Box-Muller from the same sequence, so the amplitudes are platform independent.
      const double u1 = 1.0 - random.uniform();
      const double u2 = random.uniform();
      const double normal = std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi * u2);
      rings.push_back({random.uniform(2.0, max_radius), spec.ring_strength * normal});
    }
  }
};

SyntheticScan::SyntheticScan(const Mesh& mesh, const SyntheticSpec& spec)
    : impl_(std::make_unique<Impl>()) {
  if (mesh.triangles.empty()) {
    throw std::invalid_argument("Mesh has no triangles");
  }
  if (spec.voxel_size_mm <= 0.0 || spec.padding_mm < 0.0 || spec.blur_sigma_mm < 0.0) {
    throw std::invalid_argument("voxel size must be > 0, padding and blur >= 0");
  }
  openvdb::initialize();
  Impl& impl = *impl_;
  impl.spec = spec;
  impl.bounds = meshBounds(mesh);
  impl.mesh_volume_mm3 = voxelsieve::meshVolumeMm3(mesh);
  if (impl.mesh_volume_mm3 <= 0.0) {
    throw std::invalid_argument("Mesh encloses no volume; is it closed and outward oriented?");
  }
  const double v = spec.voxel_size_mm;
  for (std::size_t i = 0; i < 3; ++i) {
    const double extent = impl.bounds.max[i] - impl.bounds.min[i] + 2.0 * spec.padding_mm;
    impl.dims[i] = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(extent / v)));
    impl.origin[i] = impl.bounds.min[i] - spec.padding_mm + 0.5 * v;
  }
  // Sub-samples lie up to sqrt(3)/2 * (2/3) voxels from the centre, plus half a sub-voxel ramp.
  impl.reach_mm = 1.1 * v;
  impl.buildDistanceFields(mesh);

  Random random(spec.seed);
  impl.placeLunkers(random);
  impl.placeLoosening(random);
  impl.placeRings(random);
}

SyntheticScan::~SyntheticScan() = default;

std::array<std::int64_t, 3> SyntheticScan::dims() const { return impl_->dims; }

double SyntheticScan::voxelSizeMm() const { return impl_->spec.voxel_size_mm; }

const SyntheticSpec& SyntheticScan::spec() const { return impl_->spec; }

const std::vector<Defect>& SyntheticScan::defects() const { return impl_->defects; }

std::array<double, 3> SyntheticScan::originMm() const { return impl_->origin; }

double SyntheticScan::meshVolumeMm3() const { return impl_->mesh_volume_mm3; }

const std::vector<std::string>& SyntheticScan::warnings() const { return impl_->warnings; }

double SyntheticScan::signedDistanceMm(const std::array<double, 3>& point_mm) const {
  Impl::Evaluator eval(*impl_);
  return eval.signedDistance(point_mm);
}

void SyntheticScan::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  const Impl& impl = *impl_;
  for (std::size_t i = 0; i < 3; ++i) {
    if (box.min[i] < 0 || box.max[i] > impl.dims[i] || box.min[i] > box.max[i]) {
      throw std::out_of_range("Region outside the synthetic scan");
    }
  }
  if (out.size() != static_cast<std::size_t>(box.voxelCount())) {
    throw std::invalid_argument("Output size does not match the region");
  }
  const SyntheticSpec& spec = impl.spec;
  const double air = spec.air_value;
  const double cupping_depth =
      spec.cupping_depth_mm > 0.0 ? spec.cupping_depth_mm : 0.2 * impl.smallestExtent();
  // The blur needs the sharp values around the box; the volume border is replicated, so every
  // voxel gets the same value however the volume is split into regions.
  const double sigma = spec.blur_sigma_mm / spec.voxel_size_mm;
  const auto halo = sigma > 0.0 ? static_cast<std::int64_t>(std::ceil(3.0 * sigma)) : 0;
  Box region;
  for (std::size_t i = 0; i < 3; ++i) {
    region.min[i] = std::max<std::int64_t>(0, box.min[i] - halo);
    region.max[i] = std::min(impl.dims[i], box.max[i] + halo);
  }
  std::vector<double> sharp(static_cast<std::size_t>(region.voxelCount()));
  Impl::Evaluator eval(impl);
  std::size_t offset = 0;
  for (std::int64_t z = region.min[2]; z < region.max[2]; ++z) {
    for (std::int64_t y = region.min[1]; y < region.max[1]; ++y) {
      for (std::int64_t x = region.min[0]; x < region.max[0]; ++x) {
        double value = air;
        if (const double fraction = eval.fraction(x, y, z); fraction > 0.0) {
          double material = spec.material_value;
          if (spec.cupping > 0.0) {
            const double depth = eval.depth(impl.voxelCenter(x, y, z));
            material *= 1.0 - spec.cupping * (1.0 - std::exp(-depth / cupping_depth));
          }
          value += fraction * (material - air);
        }
        sharp[offset++] = value;
      }
    }
  }
  if (halo > 0) {
    blurRegion(sharp, region, sigma, halo);
  }
  offset = 0;
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
      for (std::int64_t x = box.min[0]; x < box.max[0]; ++x) {
        double value = sharp[static_cast<std::size_t>(
            (x - region.min[0]) +
            region.size(0) * ((y - region.min[1]) + region.size(1) * (z - region.min[2])))];
        value += impl.ringOffset(x, y);
        if (spec.noise_sigma > 0.0) {
          const auto index = static_cast<std::uint64_t>(x + impl.dims[0] * (y + impl.dims[1] * z));
          value += spec.noise_sigma * detail::gaussianNoise(spec.seed, index);
        }
        out[offset++] = static_cast<std::uint16_t>(std::clamp(std::round(value), 0.0, 65535.0));
      }
    }
  }
}

nlohmann::json SyntheticScan::toJson() const {
  const Impl& impl = *impl_;
  const SyntheticSpec& s = impl.spec;
  nlohmann::json defects = nlohmann::json::array();
  double void_volume = 0.0;
  for (const Defect& d : impl.defects) {
    nlohmann::json entry = {{"type", toString(d.type)},
                            {"center_mm", d.center_mm},
                            {"radius_mm", d.radius_mm},
                            {"void_volume_mm3", d.void_volume_mm3}};
    if (d.type == DefectType::kLunker) {
      nlohmann::json spheres = nlohmann::json::array();
      for (const Sphere& sphere : d.spheres) {
        spheres.push_back({{"center_mm", sphere.center_mm}, {"radius_mm", sphere.radius_mm}});
      }
      entry["spheres"] = spheres;
    } else {
      entry["pore_count"] = d.spheres.size();
      entry["pore_radius_mm"] = d.spheres.empty() ? 0.0 : d.spheres.front().radius_mm;
      entry["porosity"] = d.void_volume_mm3 / sphereVolume(d.radius_mm);
    }
    void_volume += d.void_volume_mm3;
    defects.push_back(entry);
  }
  return {
      {"format",
       {{"type", "raw"}, {"dtype", "uint16"}, {"endianness", "little"}, {"order", "xyz"}}},
      {"dims", impl.dims},
      {"voxel_size_mm", s.voxel_size_mm},
      {"origin_mm", impl.origin},
      {"synthetic",
       {{"air_value", s.air_value},
        {"material_value", s.material_value},
        {"seed", s.seed},
        {"noise_sigma", s.noise_sigma},
        {"cupping", s.cupping},
        {"blur_sigma_mm", s.blur_sigma_mm},
        {"ring_count", s.ring_count},
        {"ring_strength", s.ring_strength}}},
      {"ground_truth",
       {{"mesh_volume_mm3", impl.mesh_volume_mm3},
        {"void_volume_mm3", void_volume},
        {"material_volume_mm3", impl.mesh_volume_mm3 - void_volume},
        {"porosity", void_volume / impl.mesh_volume_mm3},
        {"defects", defects}}},
  };
}

}  // namespace voxelsieve
