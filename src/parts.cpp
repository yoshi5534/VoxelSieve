#include "voxelsieve/parts.hpp"

#include <openvdb/openvdb.h>
#include <openvdb/tools/SignedFloodFill.h>
#include <openvdb/tools/VolumeToMesh.h>
#include <tbb/blocked_range.h>
#include <tbb/combinable.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

namespace voxelsieve {
namespace {

using Vec3 = std::array<double, 3>;

// Signed distance primitives and operators in mm, negative inside.

double box(const Vec3& p, const Vec3& center, const Vec3& half) {
  double outside = 0.0;
  double inside = -std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < 3; ++i) {
    const double q = std::abs(p[i] - center[i]) - half[i];
    outside += std::max(q, 0.0) * std::max(q, 0.0);
    inside = std::max(inside, q);
  }
  return std::sqrt(outside) + std::min(inside, 0.0);
}

double roundBox(const Vec3& p, const Vec3& center, const Vec3& half, double radius) {
  return box(p, center, {half[0] - radius, half[1] - radius, half[2] - radius}) - radius;
}

/// Capped cylinder along the coordinate axis `axis`.
double cylinder(const Vec3& p, const Vec3& center, std::size_t axis, double radius, double half) {
  const std::size_t u = (axis + 1) % 3;
  const std::size_t v = (axis + 2) % 3;
  const double dr = std::hypot(p[u] - center[u], p[v] - center[v]) - radius;
  const double dh = std::abs(p[axis] - center[axis]) - half;
  return std::min(std::max(dr, dh), 0.0) + std::hypot(std::max(dr, 0.0), std::max(dh, 0.0));
}

/// Ring around the z axis between the radii `inner` and `outer` and the heights `z0` and `z1`.
double ring(const Vec3& p, double inner, double outer, double z0, double z1) {
  const double r = std::hypot(p[0], p[1]);
  return std::max({r - outer, inner - r, std::abs(p[2] - (z0 + z1) / 2.0) - (z1 - z0) / 2.0});
}

/// Distance to the plane through `point`; positive on the side `normal` points to.
double plane(const Vec3& p, const Vec3& point, Vec3 normal) {
  const double length = std::hypot(normal[0], normal[1], normal[2]);
  double d = 0.0;
  for (std::size_t i = 0; i < 3; ++i) {
    d += (p[i] - point[i]) * normal[i] / length;
  }
  return d;
}

/// Union with a fillet of about `k` mm (polynomial smooth minimum).
double smoothUnion(double a, double b, double k) {
  const double h = std::max(k - std::abs(a - b), 0.0) / k;
  return std::min(a, b) - h * h * k / 4.0;
}

double subtract(double a, double b) { return std::max(a, -b); }

/// Subtraction that rounds the new edges by about `k` mm.
double smoothSubtract(double a, double b, double k) { return -smoothUnion(-a, b, k); }

/// Open gearbox housing: rounded body with a cavity, a flange with bolt holes, gusset ribs under
/// the flange, a boss with a cross bore into the cavity and two domes with blind holes inside.
double housing(const Vec3& p) {
  double d = roundBox(p, {0.0, 0.0, 0.0}, {20.0, 14.0, 10.0}, 3.0);
  d = smoothUnion(d, roundBox(p, {0.0, 0.0, 8.5}, {25.0, 19.0, 1.5}, 1.5), 1.5);
  const Vec3 mirrored{p[0], std::abs(p[1]), p[2]};
  for (const double x : {-12.0, 0.0, 12.0}) {
    // From the body wall to the flange edge, slanting towards the bottom.
    const double rib = std::max(box(mirrored, {x, 16.25, -0.5}, {0.9, 2.75, 7.5}),
                                plane(mirrored, {0.0, 18.5, 7.0}, {0.0, 15.0, -4.5}));
    d = smoothUnion(d, rib, 1.0);
  }
  d = smoothUnion(d, cylinder(p, {21.5, 0.0, -2.0}, 0, 5.0, 3.5), 1.5);
  d = smoothSubtract(d, roundBox(p, {0.0, 0.0, 3.0}, {17.0, 11.0, 10.0}, 2.0), 0.8);
  for (const double x : {-10.0, 10.0}) {
    d = smoothUnion(d, cylinder(p, {x, 0.0, -5.0}, 2, 3.0, 3.0), 1.2);
    d = subtract(d, cylinder(p, {x, 0.0, -3.5}, 2, 1.2, 2.5));
  }
  d = subtract(d, cylinder(p, {20.0, 0.0, -2.0}, 0, 2.5, 6.0));
  for (const double sx : {-1.0, 1.0}) {
    for (const double sy : {-1.0, 1.0}) {
      d = subtract(d, cylinder(p, {sx * 22.5, sy * 16.5, 8.5}, 2, 1.6, 3.0));
    }
  }
  return d;
}

/// Angle bracket: base plate and upright with a fillet between them, two gussets, two slots in
/// the base and three holes in the upright.
double bracket(const Vec3& p) {
  double d = roundBox(p, {0.0, 0.0, 2.0}, {20.0, 12.0, 2.0}, 1.0);
  d = smoothUnion(d, roundBox(p, {0.0, -10.0, 14.0}, {20.0, 2.0, 14.0}, 1.0), 3.0);
  const Vec3 mirrored{std::abs(p[0]), p[1], p[2]};
  const double gusset = std::max(box(mirrored, {16.0, -1.0, 12.0}, {1.5, 9.0, 10.0}),
                                 plane(mirrored, {0.0, -8.0, 24.0}, {0.0, 20.0, 18.0}));
  d = smoothUnion(d, gusset, 1.5);
  for (const double x : {-7.0, 7.0}) {
    // Slot along y: a capsule in the xy plane, through the base plate.
    const double y = std::clamp(p[1], 0.0, 7.0);
    const double slot = std::max(std::hypot(p[0] - x, p[1] - y) - 2.2, std::abs(p[2] - 2.0) - 3.0);
    d = subtract(d, slot);
  }
  d = subtract(d, cylinder(p, {0.0, -10.0, 17.0}, 1, 4.5, 4.0));
  for (const double x : {-11.0, 11.0}) {
    d = subtract(d, cylinder(p, {x, -10.0, 23.0}, 1, 1.6, 4.0));
  }
  return d;
}

/// Wheel hub: web between hub and rim with five windows (spokes), a bore with a keyway and a
/// bolt circle of five holes.
double hub(const Vec3& p) {
  double d = ring(p, 0.0, 20.0, -1.0, 1.0);
  d = smoothUnion(d, ring(p, 17.0, 22.0, -4.0, 6.0), 1.5);
  d = smoothUnion(d, ring(p, 0.0, 8.0, -4.0, 10.0), 2.0);
  constexpr int kSpokes = 5;
  for (int i = 0; i < kSpokes; ++i) {
    const double window = 2.0 * std::numbers::pi * (i + 0.5) / kSpokes;
    d = smoothSubtract(
        d, cylinder(p, {12.5 * std::cos(window), 12.5 * std::sin(window), 0.0}, 2, 3.8, 3.0), 0.6);
    const double bolt = 2.0 * std::numbers::pi * (i + 0.5) / kSpokes;
    d = subtract(d, cylinder(p, {6.0 * std::cos(bolt), 6.0 * std::sin(bolt), 3.0}, 2, 0.9, 8.0));
  }
  d = subtract(d, ring(p, 0.0, 4.0, -5.0, 11.0));
  d = subtract(d, box(p, {4.5, 0.0, 3.0}, {1.0, 1.0, 8.0}));
  return d;
}

/// Distance in a plane from (`x`, `y`) to the segment from `a` to `b`.
double segment(double x, double y, const std::array<double, 2>& a, const std::array<double, 2>& b) {
  const double vx = b[0] - a[0];
  const double vy = b[1] - a[1];
  const double t = std::clamp(((x - a[0]) * vx + (y - a[1]) * vy) / (vx * vx + vy * vy), 0.0, 1.0);
  return std::hypot(x - a[0] - t * vx, y - a[1] - t * vy);
}

/// `p` turned about the z axis by -`angle` (in degrees), so that direction `angle` becomes +x.
Vec3 turned(const Vec3& p, double angle) {
  const double a = angle * std::numbers::pi / 180.0;
  return {p[0] * std::cos(a) + p[1] * std::sin(a), -p[0] * std::sin(a) + p[1] * std::cos(a), p[2]};
}

/// Bell housing (clutch housing) between engine and gearbox: a thin bell of a skirt and a cone,
/// the engine flange with eight bolt lugs at uneven spacing, the gearbox flange with six bolt
/// bosses, a guide sleeve for the release bearing, outer ribs, the mount of the starter motor
/// with its bore and an inspection window. The thick lugs, bosses and the starter mount are the
/// hot spots where a real casting shrinks last.
double bellHousing(const Vec3& p) {
  const double r = std::hypot(p[0], p[1]);
  // The bell as a wall of 2.5 mm around a profile in (r, z): skirt, cone and shoulder.
  double d = std::min({segment(r, p[2], {30.5, 1.25}, {30.5, 9.0}),
                       segment(r, p[2], {30.5, 9.0}, {20.0, 28.0}),
                       segment(r, p[2], {20.0, 28.0}, {13.5, 31.0})}) -
             1.25;
  d = smoothUnion(d, ring(p, 26.5, 33.0, 0.0, 4.25), 1.2);
  constexpr std::array<double, 8> kLugs{0.0, 45.0, 90.0, 140.0, 180.0, 225.0, 270.0, 320.0};
  for (const double angle : kLugs) {
    d = smoothUnion(d, cylinder(turned(p, angle), {34.5, 0.0, 2.5}, 2, 2.75, 2.5), 1.5);
  }
  d = smoothUnion(d, ring(p, 8.0, 20.0, 30.0, 33.5), 1.5);
  d = smoothUnion(d, ring(p, 8.0, 11.0, 22.0, 36.0), 1.5);
  for (int k = 0; k < 6; ++k) {
    d = smoothUnion(d, cylinder(turned(p, 30.0 + 60.0 * k), {17.0, 0.0, 32.5}, 2, 3.0, 3.5), 1.0);
  }
  // Ribs from the engine flange up the cone, between the lugs.
  for (const double angle : {22.5, 67.5, 115.0, 202.5, 247.5, 295.0, 340.0}) {
    const Vec3 q = turned(p, angle);
    // Outside of the bell only: beyond the skirt or the cone, whichever lies farther in.
    const double outside = std::min(29.5 - q[0], -plane(q, {29.5, 0.0, 9.0}, {19.0, 0.0, 10.5}));
    const double rib = std::max({std::abs(q[1]) - 0.9, outside, q[2] - 30.5, 2.0 - q[2],
                                 plane(q, {33.5, 0.0, 3.5}, {27.5, 0.0, 13.0})});
    d = smoothUnion(d, rib, 1.0);
  }
  // Starter motor mount: a heavy boss across the wall with a dowel boss beside it.
  const Vec3 starter = turned(p, 160.0);
  d = smoothUnion(d, cylinder(starter, {27.5, 0.0, 12.0}, 0, 6.5, 7.5), 2.0);
  const Vec3 dowel = turned(p, 128.0);
  d = smoothUnion(d, cylinder(dowel, {28.0, 0.0, 8.0}, 0, 3.2, 4.0), 1.5);
  d = subtract(d, ring(p, 0.0, 7.5, 10.0, 40.0));
  d = subtract(d, cylinder(starter, {26.0, 0.0, 12.0}, 0, 4.2, 12.0));
  d = subtract(d, cylinder(dowel, {28.0, 0.0, 8.0}, 0, 1.4, 6.0));
  for (const double angle : kLugs) {
    d = subtract(d, cylinder(turned(p, angle), {34.5, 0.0, 2.5}, 2, 1.5, 4.0));
  }
  for (int k = 0; k < 6; ++k) {
    d = subtract(d, cylinder(turned(p, 30.0 + 60.0 * k), {17.0, 0.0, 31.0}, 2, 1.1, 7.0));
  }
  // Inspection window through the cone.
  const double window = std::max({std::abs(p[1]) - 5.0, std::abs(p[2] - 17.0) - 4.0, 15.0 - p[0]});
  return smoothSubtract(d, window, 0.8);
}

using DistanceFunction = double (*)(const Vec3&);

struct PartEntry {
  SamplePartInfo info;
  DistanceFunction distance;
};

const std::vector<PartEntry>& parts() {
  static const std::vector<PartEntry> entries = {
      {{"housing",
        "Open gearbox housing with flange, bolt holes, gusset ribs, a boss with a cross bore and "
        "inner domes with blind holes",
        {{-25.0, -19.0, -10.0}, {25.0, 19.0, 10.0}}},
       &housing},
      {{"bracket",
        "Angle bracket with fillet, two gussets, slots in the base and holes",
        {{-20.0, -12.0, 0.0}, {20.0, 12.0, 28.0}}},
       &bracket},
      {{"hub",
        "Wheel hub with rim, five spokes, a bore with keyway and a bolt circle",
        {{-22.0, -22.0, -4.0}, {22.0, 22.0, 10.0}}},
       &hub},
      {{"bellhousing",
        "Bell housing between engine and gearbox with flanges, bolt lugs and bosses, ribs, a "
        "starter motor mount and an inspection window",
        {{-37.25, -37.25, 0.0}, {37.25, 37.25, 36.0}}},
       &bellHousing},
  };
  return entries;
}

const PartEntry& part(std::string_view name) {
  for (const PartEntry& entry : parts()) {
    if (entry.info.name == name) {
      return entry;
    }
  }
  throw std::invalid_argument("Unknown sample part '" + std::string(name) + "'");
}

/// Distance clipped to the bounding box: fillets between faces in one plane would otherwise bulge
/// out of it.
double clippedDistance(const PartEntry& entry, const Vec3& p) {
  const Bounds& b = entry.info.bounds;
  const Vec3 center{(b.min[0] + b.max[0]) / 2.0, (b.min[1] + b.max[1]) / 2.0,
                    (b.min[2] + b.max[2]) / 2.0};
  const Vec3 half{(b.max[0] - b.min[0]) / 2.0, (b.max[1] - b.min[1]) / 2.0,
                  (b.max[2] - b.min[2]) / 2.0};
  return std::max(entry.distance(p), box(p, center, half));
}

}  // namespace

const std::vector<SamplePartInfo>& samplePartInfos() {
  static const std::vector<SamplePartInfo> infos_of_parts = [] {
    std::vector<SamplePartInfo> infos;
    for (const PartEntry& entry : parts()) {
      infos.push_back(entry.info);
    }
    return infos;
  }();
  return infos_of_parts;
}

double samplePartDistance(std::string_view name, const std::array<double, 3>& point_mm,
                          double scale) {
  return scale * clippedDistance(part(name),
                                 {point_mm[0] / scale, point_mm[1] / scale, point_mm[2] / scale});
}

Mesh samplePartMesh(std::string_view name, const SamplePartOptions& options) {
  const PartEntry& entry = part(name);
  if (options.scale <= 0.0 || options.resolution_mm < 0.0) {
    throw std::invalid_argument("scale must be > 0 and resolution >= 0");
  }
  openvdb::initialize();
  const double scale = options.scale;
  const Bounds& bounds = entry.info.bounds;
  double longest = 0.0;
  for (std::size_t i = 0; i < 3; ++i) {
    longest = std::max(longest, scale * (bounds.max[i] - bounds.min[i]));
  }
  const double h = options.resolution_mm > 0.0 ? options.resolution_mm : longest / 200.0;
  // Two cells of margin so the surface is closed on every side.
  Vec3 origin{};
  std::array<std::int64_t, 3> dims{};
  for (std::size_t i = 0; i < 3; ++i) {
    origin[i] = scale * bounds.min[i] - 2.0 * h;
    dims[i] = static_cast<std::int64_t>(std::ceil(scale * (bounds.max[i] - bounds.min[i]) / h)) + 5;
  }
  // Evaluate only 8^3 blocks that can reach the narrow band: the distance changes by at most
  // the distance moved, so a block whose centre is farther than its half diagonal plus the band
  // holds no band voxel.
  constexpr float kHalfBand = 3.0F;
  constexpr std::int64_t kBlock = 8;
  const std::array<std::int64_t, 3> blocks{(dims[0] + kBlock - 1) / kBlock,
                                           (dims[1] + kBlock - 1) / kBlock,
                                           (dims[2] + kBlock - 1) / kBlock};
  const auto distance = [&](double x, double y, double z) {
    const Vec3 p{origin[0] + x * h, origin[1] + y * h, origin[2] + z * h};
    return scale * clippedDistance(entry, {p[0] / scale, p[1] / scale, p[2] / scale}) / h;
  };
  using Sample = std::pair<openvdb::Coord, float>;
  tbb::combinable<std::vector<Sample>> samples;
  const double reach = std::numbers::sqrt3 * kBlock / 2.0 + kHalfBand;
  tbb::parallel_for(
      tbb::blocked_range<std::int64_t>(0, blocks[0] * blocks[1] * blocks[2]),
      [&](const auto& range) {
        auto& local = samples.local();
        for (std::int64_t b = range.begin(); b < range.end(); ++b) {
          const std::array<std::int64_t, 3> first{(b % blocks[0]) * kBlock,
                                                  ((b / blocks[0]) % blocks[1]) * kBlock,
                                                  (b / (blocks[0] * blocks[1])) * kBlock};
          const double half = (kBlock - 1) / 2.0;
          if (std::abs(distance(static_cast<double>(first[0]) + half,
                                static_cast<double>(first[1]) + half,
                                static_cast<double>(first[2]) + half)) > reach) {
            continue;
          }
          for (std::int64_t z = first[2]; z < std::min(first[2] + kBlock, dims[2]); ++z) {
            for (std::int64_t y = first[1]; y < std::min(first[1] + kBlock, dims[1]); ++y) {
              for (std::int64_t x = first[0]; x < std::min(first[0] + kBlock, dims[0]); ++x) {
                const double d = distance(static_cast<double>(x), static_cast<double>(y),
                                          static_cast<double>(z));
                if (std::abs(d) < kHalfBand) {
                  local.emplace_back(
                      openvdb::Coord(static_cast<int>(x), static_cast<int>(y), static_cast<int>(z)),
                      static_cast<float>(d));
                }
              }
            }
          }
        }
      });

  // Narrow-band level set in index space; the flood fill sets the sign of the far voxels.
  auto grid = openvdb::FloatGrid::create(kHalfBand);
  grid->setGridClass(openvdb::GRID_LEVEL_SET);
  {
    auto accessor = grid->getAccessor();
    samples.combine_each([&](const std::vector<Sample>& list) {
      for (const auto& [coord, value] : list) {
        accessor.setValue(coord, value);
      }
    });
  }
  openvdb::tools::signedFloodFill(grid->tree());

  std::vector<openvdb::Vec3s> points;
  std::vector<openvdb::Vec3I> triangles;
  std::vector<openvdb::Vec4I> quads;
  openvdb::tools::volumeToMesh(*grid, points, triangles, quads, 0.0);
  Mesh mesh;
  mesh.triangles.reserve(triangles.size() + 2 * quads.size());
  const auto vertex = [&](std::uint32_t index) {
    const openvdb::Vec3s& q = points[index];
    return std::array<float, 3>{static_cast<float>(origin[0] + q.x() * h),
                                static_cast<float>(origin[1] + q.y() * h),
                                static_cast<float>(origin[2] + q.z() * h)};
  };
  for (const openvdb::Vec3I& t : triangles) {
    mesh.triangles.push_back({vertex(t[0]), vertex(t[1]), vertex(t[2])});
  }
  for (const openvdb::Vec4I& q : quads) {
    mesh.triangles.push_back({vertex(q[0]), vertex(q[1]), vertex(q[2])});
    mesh.triangles.push_back({vertex(q[0]), vertex(q[2]), vertex(q[3])});
  }
  // volumeToMesh winds level-set surfaces clockwise seen from outside.
  if (meshVolumeMm3(mesh) < 0.0) {
    for (auto& triangle : mesh.triangles) {
      std::swap(triangle[1], triangle[2]);
    }
  }
  return mesh;
}

}  // namespace voxelsieve
