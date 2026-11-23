#include "voxelsieve/region.hpp"

#include <openvdb/tools/Interpolation.h>
#include <openvdb/tools/MeshToVolume.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>

#include "detail/keys.hpp"
#include "detail/voxel_distance.hpp"
#include "voxelsieve/vdb.hpp"

namespace voxelsieve {

using Json = nlohmann::json;
using openvdb::Coord;

namespace {

constexpr int kBlock = 8;
/// A block of the part counts as full when its material and voids fill this share of it.
constexpr double kFullBlock = 0.95;
/// Mesh solids are sampled on a distance field of this many voxels along their longest extent.
constexpr double kMeshResolution = 256.0;
constexpr float kMeshHalfBand = 3.0F;
/// Reference volumes span at most this many cells per axis, and are moved by a sixteenth of
/// their size.
constexpr std::int64_t kMaxReferenceCells = 64;
constexpr std::int64_t kPlacementSteps = 16;
/// Placements per axis searched in one window.
constexpr std::int64_t kWindowPlacements = 32;

double dot(const PointMm& a, const PointMm& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
PointMm sub(const PointMm& a, const PointMm& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double length(const PointMm& a) { return std::sqrt(dot(a, a)); }

std::string number(double value) {
  std::ostringstream out;
  out.precision(4);
  out << (std::abs(value) < 5e-13 ? 0.0 : value);
  return out.str();
}

std::string point(const PointMm& p) {
  return "(" + number(p[0]) + ", " + number(p[1]) + ", " + number(p[2]) + ")";
}

PointMm pointFromJson(const Json& json, const std::string& what) {
  if (!json.is_array() || json.size() != 3 ||
      !std::all_of(json.begin(), json.end(), [](const Json& c) { return c.is_number(); })) {
    throw std::invalid_argument(what + " must be [x, y, z] in mm");
  }
  return json.get<PointMm>();
}

BoxMm boxFromJson(const Json& json) {
  if (!json.is_array() || json.size() != 2) {
    throw std::invalid_argument("box_mm must be [[x0, y0, z0], [x1, y1, z1]]");
  }
  BoxMm box{pointFromJson(json[0], "box_mm"), pointFromJson(json[1], "box_mm")};
  for (std::size_t a = 0; a < 3; ++a) {
    if (box[0][a] > box[1][a]) {
      std::swap(box[0][a], box[1][a]);
    }
  }
  return box;
}

double positive(const Json& json, const char* key, const std::string& what) {
  if (!json.contains(key) || !json.at(key).is_number() || json.at(key).get<double>() <= 0.0) {
    throw std::invalid_argument(what + " needs a positive " + key);
  }
  return json.at(key).get<double>();
}

/// Signed distance to a box, negative inside.
double boxDistance(const BoxMm& box, const PointMm& p) {
  double outside = 0.0;
  double inside = -std::numeric_limits<double>::infinity();
  for (std::size_t a = 0; a < 3; ++a) {
    const double d = std::max(box[0][a] - p[a], p[a] - box[1][a]);
    outside += std::max(d, 0.0) * std::max(d, 0.0);
    inside = std::max(inside, d);
  }
  return outside > 0.0 ? std::sqrt(outside) : inside;
}

/// Signed distance to a capped cylinder, negative inside.
double cylinderDistance(const RegionShape& s, const PointMm& p) {
  const PointMm axis = sub(s.end_mm, s.start_mm);
  const double height = length(axis);
  const PointMm unit{axis[0] / height, axis[1] / height, axis[2] / height};
  const PointMm rel = sub(p, s.start_mm);
  const double t = dot(rel, unit);
  const PointMm radial{rel[0] - t * unit[0], rel[1] - t * unit[1], rel[2] - t * unit[2]};
  const double dr = length(radial) - s.radius_mm;
  const double dh = std::max(-t, t - height);
  return std::min(std::max(dr, dh), 0.0) + std::hypot(std::max(dr, 0.0), std::max(dh, 0.0));
}

void addTriangle(Mesh& mesh, const PointMm& a, const PointMm& b, const PointMm& c) {
  const auto f = [](const PointMm& p) {
    return std::array<float, 3>{static_cast<float>(p[0]), static_cast<float>(p[1]),
                                static_cast<float>(p[2])};
  };
  mesh.triangles.push_back({f(a), f(b), f(c)});
}

void appendSphere(Mesh& mesh, const PointMm& center, double radius) {
  constexpr int kRings = 16;
  constexpr int kSegments = 32;
  const auto at = [&](int ring, int segment) {
    const double theta = std::numbers::pi * ring / kRings;
    const double phi = 2.0 * std::numbers::pi * segment / kSegments;
    return PointMm{center[0] + radius * std::sin(theta) * std::cos(phi),
                   center[1] + radius * std::sin(theta) * std::sin(phi),
                   center[2] + radius * std::cos(theta)};
  };
  for (int r = 0; r < kRings; ++r) {
    for (int s = 0; s < kSegments; ++s) {
      const PointMm a = at(r, s);
      const PointMm b = at(r + 1, s);
      const PointMm c = at(r + 1, s + 1);
      const PointMm d = at(r, s + 1);
      if (r > 0) {
        addTriangle(mesh, a, b, d);
      }
      if (r + 1 < kRings) {
        addTriangle(mesh, d, b, c);
      }
    }
  }
}

void appendCylinder(Mesh& mesh, const RegionShape& s) {
  constexpr int kSegments = 48;
  const PointMm axis = sub(s.end_mm, s.start_mm);
  const double height = length(axis);
  const PointMm w{axis[0] / height, axis[1] / height, axis[2] / height};
  // Two unit vectors perpendicular to the axis.
  const PointMm helper = std::abs(w[0]) < 0.9 ? PointMm{1.0, 0.0, 0.0} : PointMm{0.0, 1.0, 0.0};
  PointMm u{w[1] * helper[2] - w[2] * helper[1], w[2] * helper[0] - w[0] * helper[2],
            w[0] * helper[1] - w[1] * helper[0]};
  const double lu = length(u);
  u = {u[0] / lu, u[1] / lu, u[2] / lu};
  const PointMm v{w[1] * u[2] - w[2] * u[1], w[2] * u[0] - w[0] * u[2], w[0] * u[1] - w[1] * u[0]};
  const auto rim = [&](const PointMm& base, int segment) {
    const double phi = 2.0 * std::numbers::pi * segment / kSegments;
    const double cu = s.radius_mm * std::cos(phi);
    const double cv = s.radius_mm * std::sin(phi);
    return PointMm{base[0] + cu * u[0] + cv * v[0], base[1] + cu * u[1] + cv * v[1],
                   base[2] + cu * u[2] + cv * v[2]};
  };
  for (int i = 0; i < kSegments; ++i) {
    const PointMm a0 = rim(s.start_mm, i);
    const PointMm a1 = rim(s.start_mm, i + 1);
    const PointMm b0 = rim(s.end_mm, i);
    const PointMm b1 = rim(s.end_mm, i + 1);
    addTriangle(mesh, a0, a1, b1);
    addTriangle(mesh, a0, b1, b0);
    addTriangle(mesh, s.start_mm, a1, a0);
    addTriangle(mesh, s.end_mm, b0, b1);
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Shapes and regions

RegionShape RegionShape::box(const BoxMm& box_mm) {
  RegionShape shape;
  shape.kind = Kind::kBox;
  shape.box_mm = box_mm;
  return shape;
}

RegionShape RegionShape::sphere(const PointMm& center_mm, double radius_mm) {
  RegionShape shape;
  shape.kind = Kind::kSphere;
  shape.center_mm = center_mm;
  shape.radius_mm = radius_mm;
  return shape;
}

RegionShape RegionShape::cylinder(const PointMm& start_mm, const PointMm& end_mm,
                                  double radius_mm) {
  RegionShape shape;
  shape.kind = Kind::kCylinder;
  shape.start_mm = start_mm;
  shape.end_mm = end_mm;
  shape.radius_mm = radius_mm;
  return shape;
}

RegionShape RegionShape::mesh(const std::filesystem::path& path, const RigidTransform& pose) {
  RegionShape shape;
  shape.kind = Kind::kMesh;
  shape.mesh_path = path;
  shape.mesh_pose = pose;
  return shape;
}

/// A shape with what it takes to answer inside/outside quickly.
struct InspectionRegion::Solid {
  explicit Solid(RegionShape s) : shape(std::move(s)) {
    switch (shape.kind) {
      case RegionShape::Kind::kBox:
        bounds = shape.box_mm;
        break;
      case RegionShape::Kind::kSphere:
        for (std::size_t a = 0; a < 3; ++a) {
          bounds[0][a] = shape.center_mm[a] - shape.radius_mm;
          bounds[1][a] = shape.center_mm[a] + shape.radius_mm;
        }
        break;
      case RegionShape::Kind::kCylinder:
        if (length(sub(shape.end_mm, shape.start_mm)) <= 0.0) {
          throw std::invalid_argument("A cylinder of an inspection region needs two end points");
        }
        for (std::size_t a = 0; a < 3; ++a) {
          bounds[0][a] = std::min(shape.start_mm[a], shape.end_mm[a]) - shape.radius_mm;
          bounds[1][a] = std::max(shape.start_mm[a], shape.end_mm[a]) + shape.radius_mm;
        }
        break;
      case RegionShape::Kind::kMesh:
        loadMesh();
        break;
    }
  }

  void loadMesh() {
    initializeVdb();
    const Mesh mesh = readStl(shape.mesh_path);
    if (mesh.triangles.empty()) {
      throw std::invalid_argument("The region mesh " + shape.mesh_path.string() + " is empty");
    }
    std::map<std::array<float, 3>, std::uint32_t> index_of;
    std::vector<openvdb::Vec3s> points;
    std::vector<openvdb::Vec3I> triangles;
    bounds = {PointMm{1e300, 1e300, 1e300}, PointMm{-1e300, -1e300, -1e300}};
    for (const auto& triangle : mesh.triangles) {
      openvdb::Vec3I indices;
      for (std::size_t i = 0; i < 3; ++i) {
        const PointMm p = shape.mesh_pose.apply({triangle[i][0], triangle[i][1], triangle[i][2]});
        const std::array<float, 3> key{static_cast<float>(p[0]), static_cast<float>(p[1]),
                                       static_cast<float>(p[2])};
        const auto [it, inserted] =
            index_of.emplace(key, static_cast<std::uint32_t>(points.size()));
        if (inserted) {
          points.emplace_back(key[0], key[1], key[2]);
          for (std::size_t a = 0; a < 3; ++a) {
            bounds[0][a] = std::min(bounds[0][a], p[a]);
            bounds[1][a] = std::max(bounds[1][a], p[a]);
          }
        }
        indices[static_cast<int>(i)] = it->second;
      }
      triangles.push_back(indices);
    }
    double longest = 0.0;
    for (std::size_t a = 0; a < 3; ++a) {
      longest = std::max(longest, bounds[1][a] - bounds[0][a]);
    }
    const double voxel = std::max(longest / kMeshResolution, 1e-4);
    auto transform = openvdb::math::Transform::createLinearTransform(voxel);
    distance = openvdb::tools::meshToLevelSet<openvdb::FloatGrid>(*transform, points, triangles,
                                                                  kMeshHalfBand);
  }

  /// Signed distance in mm, negative inside. For a mesh, exact near its surface and capped at
  /// the band width beyond.
  [[nodiscard]] double signedDistance(const PointMm& p) const {
    switch (shape.kind) {
      case RegionShape::Kind::kBox:
        return boxDistance(shape.box_mm, p);
      case RegionShape::Kind::kSphere:
        return length(sub(p, shape.center_mm)) - shape.radius_mm;
      case RegionShape::Kind::kCylinder:
        return cylinderDistance(shape, p);
      case RegionShape::Kind::kMesh: {
        if (boxDistance(bounds, p) > 0.0) {
          return boxDistance(bounds, p);
        }
        const auto index = distance->transform().worldToIndex(openvdb::Vec3d(p[0], p[1], p[2]));
        return openvdb::tools::BoxSampler::sample(distance->tree(), index);
      }
    }
    return 1.0;
  }

  [[nodiscard]] bool contains(const PointMm& p) const { return signedDistance(p) < 0.0; }

  /// 1 when the box lies inside, 0 outside, -1 when it may cross the surface.
  [[nodiscard]] int classify(const BoxMm& box) const {
    if (boxDistance(bounds, box[0]) > 0.0 && boxDistance(bounds, box[1]) > 0.0) {
      // Both corners outside the bounds does not mean the box is: test the overlap.
      bool disjoint = false;
      for (std::size_t a = 0; a < 3; ++a) {
        disjoint = disjoint || box[1][a] < bounds[0][a] || box[0][a] > bounds[1][a];
      }
      if (disjoint) {
        return 0;
      }
    }
    if (shape.kind == RegionShape::Kind::kBox) {
      bool inside = true;
      for (std::size_t a = 0; a < 3; ++a) {
        if (box[1][a] < shape.box_mm[0][a] || box[0][a] > shape.box_mm[1][a]) {
          return 0;
        }
        inside = inside && box[0][a] >= shape.box_mm[0][a] && box[1][a] <= shape.box_mm[1][a];
      }
      return inside ? 1 : -1;
    }
    const PointMm center{(box[0][0] + box[1][0]) / 2, (box[0][1] + box[1][1]) / 2,
                         (box[0][2] + box[1][2]) / 2};
    const double half_diagonal = length(sub(box[1], box[0])) / 2.0;
    const double d = signedDistance(center);
    if (shape.kind == RegionShape::Kind::kMesh) {
      // Beyond the band the distance is capped, which says inside or outside but not how far.
      const double band = kMeshHalfBand * distance->voxelSize()[0];
      if (std::abs(d) >= band * 0.999 && half_diagonal >= band * 0.999) {
        return -1;
      }
    }
    if (d <= -half_diagonal) {
      return 1;
    }
    return d >= half_diagonal ? 0 : -1;
  }

  RegionShape shape;
  BoxMm bounds{};
  openvdb::FloatGrid::Ptr distance;  // meshes
};

InspectionRegion::InspectionRegion(const std::vector<RegionShape>& include,
                                   const std::vector<RegionShape>& exclude) {
  for (const RegionShape& shape : include) {
    include_.push_back(std::make_shared<const Solid>(shape));
  }
  for (const RegionShape& shape : exclude) {
    exclude_.push_back(std::make_shared<const Solid>(shape));
  }
}

InspectionRegion InspectionRegion::fromJson(const Json& zone,
                                            const std::filesystem::path& base_dir) {
  const auto shape_from_json = [&](const Json& item) {
    if (!item.is_object()) {
      throw std::invalid_argument("A solid of an inspection region must be an object");
    }
    detail::requireKnownKeys(
        item, {"box_mm", "sphere", "cylinder", "mesh", "mesh_object", "pose", "name"},
        "a solid of an inspection region");
    if (item.contains("box_mm")) {
      return RegionShape::box(boxFromJson(item.at("box_mm")));
    }
    if (item.contains("sphere")) {
      const Json& s = item.at("sphere");
      detail::requireKnownKeys(s, {"center_mm", "radius_mm"}, "a sphere of an inspection region");
      return RegionShape::sphere(pointFromJson(s.value("center_mm", Json()), "sphere center_mm"),
                                 positive(s, "radius_mm", "A sphere"));
    }
    if (item.contains("cylinder")) {
      const Json& c = item.at("cylinder");
      detail::requireKnownKeys(c, {"from_mm", "to_mm", "radius_mm"},
                               "a cylinder of an inspection region");
      return RegionShape::cylinder(pointFromJson(c.value("from_mm", Json()), "cylinder from_mm"),
                                   pointFromJson(c.value("to_mm", Json()), "cylinder to_mm"),
                                   positive(c, "radius_mm", "A cylinder"));
    }
    if (item.contains("mesh")) {
      std::filesystem::path path = item.at("mesh").get<std::string>();
      if (path.is_relative() && !base_dir.empty()) {
        path = base_dir / path;
      }
      RegionShape shape = RegionShape::mesh(
          path, item.contains("pose")
                    ? RigidTransform::fromMatrix(item.at("pose").get<std::vector<double>>())
                    : RigidTransform{});
      shape.mesh_name = item.value("name", std::string());
      return shape;
    }
    if (item.contains("mesh_object")) {
      throw std::invalid_argument(
          "A region names the project object '" + item.at("mesh_object").get<std::string>() +
          "': mesh objects can only be used in a project; give the STL file as \"mesh\"");
    }
    throw std::invalid_argument(
        "A solid of an inspection region is one of box_mm, sphere, cylinder or mesh");
  };
  std::vector<RegionShape> include;
  std::vector<RegionShape> exclude;
  if (zone.contains("box_mm")) {
    include.push_back(RegionShape::box(boxFromJson(zone.at("box_mm"))));
  }
  if (zone.contains("region")) {
    const Json& region = zone.at("region");
    const Json included = region.is_array() ? region : region.value("include", Json::array());
    for (const Json& item : included) {
      include.push_back(shape_from_json(item));
    }
    if (region.is_object()) {
      detail::requireKnownKeys(region, {"include", "exclude"}, "an inspection region");
      for (const Json& item : region.value("exclude", Json::array())) {
        exclude.push_back(shape_from_json(item));
      }
    }
  }
  return {include, exclude};
}

bool InspectionRegion::contains(const PointMm& p_mm) const {
  const auto inside = [&](const auto& solid) { return solid->contains(p_mm); };
  return (include_.empty() || std::any_of(include_.begin(), include_.end(), inside)) &&
         std::none_of(exclude_.begin(), exclude_.end(), inside);
}

double InspectionRegion::overlap(const BoxMm& box_mm) const {
  if (wholePart()) {
    return 1.0;
  }
  if (include_.size() == 1 && exclude_.empty() &&
      include_.front()->shape.kind == RegionShape::Kind::kBox) {
    const BoxMm& b = include_.front()->shape.box_mm;
    double share = 1.0;
    for (std::size_t a = 0; a < 3; ++a) {
      const double extent = box_mm[1][a] - box_mm[0][a];
      const double inside = std::min(box_mm[1][a], b[1][a]) - std::max(box_mm[0][a], b[0][a]);
      share *= extent > 0.0 ? std::clamp(inside / extent, 0.0, 1.0) : (inside >= 0.0 ? 1.0 : 0.0);
    }
    return share;
  }
  bool in_include = include_.empty();
  bool undecided = false;
  for (const auto& solid : include_) {
    const int c = solid->classify(box_mm);
    in_include = in_include || c == 1;
    undecided = undecided || c == -1;
  }
  if (!in_include && !undecided) {
    return 0.0;
  }
  bool clear_of_exclude = true;
  for (const auto& solid : exclude_) {
    const int c = solid->classify(box_mm);
    if (c == 1) {
      return 0.0;
    }
    clear_of_exclude = clear_of_exclude && c == 0;
  }
  if (in_include && clear_of_exclude) {
    return 1.0;
  }
  constexpr int kSamples = 4;
  int hits = 0;
  for (int k = 0; k < kSamples; ++k) {
    for (int j = 0; j < kSamples; ++j) {
      for (int i = 0; i < kSamples; ++i) {
        const std::array<int, 3> index{i, j, k};
        PointMm p{};
        for (std::size_t a = 0; a < 3; ++a) {
          p[a] = box_mm[0][a] + (index[a] + 0.5) / kSamples * (box_mm[1][a] - box_mm[0][a]);
        }
        hits += contains(p) ? 1 : 0;
      }
    }
  }
  return static_cast<double>(hits) / (kSamples * kSamples * kSamples);
}

std::optional<BoxMm> InspectionRegion::bounds() const {
  if (include_.empty()) {
    return std::nullopt;
  }
  BoxMm box = include_.front()->bounds;
  for (const auto& solid : include_) {
    for (std::size_t a = 0; a < 3; ++a) {
      box[0][a] = std::min(box[0][a], solid->bounds[0][a]);
      box[1][a] = std::max(box[1][a], solid->bounds[1][a]);
    }
  }
  return box;
}

std::string InspectionRegion::description() const {
  const auto describe = [](const RegionShape& s) -> std::string {
    switch (s.kind) {
      case RegionShape::Kind::kBox:
        return "box from " + point(s.box_mm[0]) + " to " + point(s.box_mm[1]) + " mm";
      case RegionShape::Kind::kSphere:
        return "sphere around " + point(s.center_mm) + " mm, radius " + number(s.radius_mm) + " mm";
      case RegionShape::Kind::kCylinder:
        return "cylinder from " + point(s.start_mm) + " to " + point(s.end_mm) + " mm, radius " +
               number(s.radius_mm) + " mm";
      case RegionShape::Kind::kMesh:
        return "mesh " + (s.mesh_name.empty() ? s.mesh_path.filename().string() : s.mesh_name);
    }
    return {};
  };
  std::string text;
  if (include_.empty()) {
    text = "whole part";
  }
  for (std::size_t i = 0; i < include_.size(); ++i) {
    text += (i == 0 ? "" : " and ") + describe(include_[i]->shape);
  }
  for (std::size_t i = 0; i < exclude_.size(); ++i) {
    text += (i == 0 ? " without " : " and ") + describe(exclude_[i]->shape);
  }
  return text;
}

std::vector<RegionShape> InspectionRegion::include() const {
  std::vector<RegionShape> shapes;
  shapes.reserve(include_.size());
  for (const auto& solid : include_) {
    shapes.push_back(solid->shape);
  }
  return shapes;
}

std::vector<RegionShape> InspectionRegion::exclude() const {
  std::vector<RegionShape> shapes;
  shapes.reserve(exclude_.size());
  for (const auto& solid : exclude_) {
    shapes.push_back(solid->shape);
  }
  return shapes;
}

Mesh InspectionRegion::mesh(bool excluded) const {
  Mesh mesh;
  for (const auto& solid : excluded ? exclude_ : include_) {
    const RegionShape& s = solid->shape;
    switch (s.kind) {
      case RegionShape::Kind::kBox: {
        const PointMm size = sub(s.box_mm[1], s.box_mm[0]);
        const PointMm center{(s.box_mm[0][0] + s.box_mm[1][0]) / 2,
                             (s.box_mm[0][1] + s.box_mm[1][1]) / 2,
                             (s.box_mm[0][2] + s.box_mm[1][2]) / 2};
        const Mesh box = boxMesh(size, center);
        mesh.triangles.insert(mesh.triangles.end(), box.triangles.begin(), box.triangles.end());
        break;
      }
      case RegionShape::Kind::kSphere:
        appendSphere(mesh, s.center_mm, s.radius_mm);
        break;
      case RegionShape::Kind::kCylinder:
        appendCylinder(mesh, s);
        break;
      case RegionShape::Kind::kMesh:
        for (const auto& triangle : readStl(s.mesh_path).triangles) {
          std::array<PointMm, 3> p{};
          for (std::size_t i = 0; i < 3; ++i) {
            p[i] = s.mesh_pose.apply({triangle[i][0], triangle[i][1], triangle[i][2]});
          }
          addTriangle(mesh, p[0], p[1], p[2]);
        }
        break;
    }
  }
  return mesh;
}

// ---------------------------------------------------------------------------------------------
// Reference volume

ReferenceVolume ReferenceVolume::fromJson(const Json& json) {
  if (!json.is_object()) {
    throw std::invalid_argument("reference_volume must be an object");
  }
  detail::requireKnownKeys(json,
                           {"shape", "edge_mm", "size_mm", "diameter_mm", "height_mm", "axis"},
                           "'reference_volume'");
  ReferenceVolume reference;
  const std::string shape = json.value("shape", "cube");
  if (shape == "cube") {
    const double edge = positive(json, "edge_mm", "A reference cube");
    reference.size_mm = {edge, edge, edge};
  } else if (shape == "cuboid") {
    reference.shape = Shape::kCuboid;
    reference.size_mm = pointFromJson(json.value("size_mm", Json()), "A reference cuboid size_mm");
    if (std::any_of(reference.size_mm.begin(), reference.size_mm.end(),
                    [](double s) { return s <= 0.0; })) {
      throw std::invalid_argument("A reference cuboid needs positive edges");
    }
  } else if (shape == "sphere") {
    reference.shape = Shape::kSphere;
    const double d = positive(json, "diameter_mm", "A reference sphere");
    reference.size_mm = {d, d, d};
  } else if (shape == "cylinder") {
    reference.shape = Shape::kCylinder;
    const double d = positive(json, "diameter_mm", "A reference cylinder");
    const double h = positive(json, "height_mm", "A reference cylinder");
    const std::string axis = json.value("axis", "z");
    if (axis != "x" && axis != "y" && axis != "z") {
      throw std::invalid_argument("The axis of a reference cylinder is x, y or z");
    }
    reference.axis = axis[0] - 'x';
    reference.size_mm = {d, d, d};
    reference.size_mm[static_cast<std::size_t>(reference.axis)] = h;
  } else {
    throw std::invalid_argument("Unknown reference volume shape '" + shape +
                                "': cube, cuboid, sphere or cylinder");
  }
  return reference;
}

std::string ReferenceVolume::description() const {
  switch (shape) {
    case Shape::kCube:
      return "cube of " + number(size_mm[0]) + " mm";
    case Shape::kCuboid:
      return "cuboid of " + number(size_mm[0]) + " × " + number(size_mm[1]) + " × " +
             number(size_mm[2]) + " mm";
    case Shape::kSphere:
      return "sphere of " + number(size_mm[0]) + " mm diameter";
    case Shape::kCylinder: {
      const auto a = static_cast<std::size_t>(axis);
      return "cylinder of " + number(size_mm[(a + 1) % 3]) + " mm diameter and " +
             number(size_mm[a]) + " mm height along " +
             std::string(1, static_cast<char>('x' + axis));
    }
  }
  return {};
}

double ReferenceVolume::volumeMm3() const {
  const double box = size_mm[0] * size_mm[1] * size_mm[2];
  switch (shape) {
    case Shape::kSphere:
      return box * std::numbers::pi / 6.0;
    case Shape::kCylinder:
      return box * std::numbers::pi / 4.0;
    default:
      return box;
  }
}

namespace {

/// A run of cells of a rasterized reference volume along x.
struct Run {
  std::int64_t y = 0;
  std::int64_t z = 0;
  std::int64_t x0 = 0;  // first cell
  std::int64_t x1 = 0;  // one past the last
};

std::vector<Run> rasterize(const ReferenceVolume& reference, const std::array<std::int64_t, 3>& n) {
  const auto inside = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
    const std::array<double, 3> u{(static_cast<double>(x) + 0.5) / static_cast<double>(n[0]) - 0.5,
                                  (static_cast<double>(y) + 0.5) / static_cast<double>(n[1]) - 0.5,
                                  (static_cast<double>(z) + 0.5) / static_cast<double>(n[2]) - 0.5};
    switch (reference.shape) {
      case ReferenceVolume::Shape::kSphere:
        return u[0] * u[0] + u[1] * u[1] + u[2] * u[2] <= 0.25;
      case ReferenceVolume::Shape::kCylinder: {
        double r2 = 0.0;
        for (int a = 0; a < 3; ++a) {
          if (a != reference.axis) {
            r2 += u[static_cast<std::size_t>(a)] * u[static_cast<std::size_t>(a)];
          }
        }
        return r2 <= 0.25;
      }
      default:
        return true;
    }
  };
  std::vector<Run> runs;
  for (std::int64_t z = 0; z < n[2]; ++z) {
    for (std::int64_t y = 0; y < n[1]; ++y) {
      std::int64_t x0 = -1;
      std::int64_t x1 = -1;
      for (std::int64_t x = 0; x < n[0]; ++x) {
        if (inside(x, y, z)) {
          x0 = x0 < 0 ? x : x0;
          x1 = x + 1;
        }
      }
      if (x0 >= 0) {
        runs.push_back({y, z, x0, x1});
      }
    }
  }
  return runs;
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
  return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/// Sum of the void per pore voxel (pore id -> mm^3 per voxel).
std::map<int, double> voidPerVoxel(const PorosityResult& result, const std::vector<bool>* only) {
  std::map<int, double> weights;
  for (std::size_t i = 0; i < result.pores.size(); ++i) {
    const DetectedPore& pore = result.pores[i];
    if ((only == nullptr || (*only)[i]) && pore.voxel_count > 0) {
      weights[pore.id] = pore.volume_mm3 / static_cast<double>(pore.voxel_count);
    }
  }
  return weights;
}

}  // namespace

ReferencePlacement maxReferencePorosity(const PorosityResult& result,
                                        const InspectionRegion& region,
                                        const ReferenceVolume& reference,
                                        const std::vector<bool>& considered) {
  const VoxelSize& v = result.voxel_size;
  const double voxel_volume = v.volumeMm3();
  const double block_volume = kBlock * kBlock * kBlock * voxel_volume;
  if (!result.material_blocks || !result.pore_labels) {
    throw std::invalid_argument(
        "The porosity result has no block grids (analysis.vdb): run the analysis again");
  }

  // Cells of c voxels per axis, so the reference spans at most kMaxReferenceCells of them.
  std::array<std::int64_t, 3> c{};
  std::array<std::int64_t, 3> n{};
  std::array<std::int64_t, 3> step{};
  for (std::size_t a = 0; a < 3; ++a) {
    const auto voxels = std::max<std::int64_t>(1, std::llround(reference.size_mm[a] / v[a]));
    c[a] = std::max<std::int64_t>(1, (voxels + kMaxReferenceCells - 1) / kMaxReferenceCells);
    n[a] = std::max<std::int64_t>(
        1, std::llround(reference.size_mm[a] / (static_cast<double>(c[a]) * v[a])));
    step[a] = std::max<std::int64_t>(1, n[a] / kPlacementSteps);
  }
  const std::vector<Run> runs = rasterize(reference, n);
  std::int64_t mask_cells = 0;
  for (const Run& run : runs) {
    mask_cells += run.x1 - run.x0;
  }
  const double cell_volume = static_cast<double>(c[0] * c[1] * c[2]) * voxel_volume;
  ReferencePlacement best;
  best.volume_mm3 = static_cast<double>(mask_cells) * cell_volume;

  // Voids: the pores to count and the loosened zones, as boxes of voxels.
  const std::map<int, double> weights = voidPerVoxel(result, &considered);
  std::vector<Box> sources;
  for (std::size_t i = 0; i < result.pores.size(); ++i) {
    if (considered[i]) {
      sources.push_back(result.pores[i].bounds);
    }
  }
  for (const PorosityZone& zone : result.zones) {
    sources.push_back(zone.bounds);
  }
  if (sources.empty() || mask_cells == 0) {
    return best;
  }

  // How full of part every block is: material plus all voids in it.
  auto fill = result.material_blocks->deepCopy();
  {
    auto fill_acc = fill->getAccessor();
    const std::map<int, double> all = voidPerVoxel(result, nullptr);
    std::map<Coord, double> pore_void;
    for (auto it = result.pore_labels->cbeginValueOn(); it; ++it) {
      if (const auto w = all.find(*it); w != all.end()) {
        const Coord p = it.getCoord();
        pore_void[Coord(p.x() >> 3, p.y() >> 3, p.z() >> 3)] += w->second;
      }
    }
    for (const auto& [block, volume] : pore_void) {
      fill_acc.setValue(block, fill_acc.getValue(block) + static_cast<float>(volume));
    }
    if (result.zone_blocks) {
      for (auto it = result.zone_blocks->cbeginValueOn(); it; ++it) {
        fill_acc.setValue(it.getCoord(),
                          fill_acc.getValue(it.getCoord()) +
                              static_cast<float>(std::max(0.0F, *it) * block_volume));
      }
    }
  }

  // Windows of kWindowPlacements placements per axis that may cover a void.
  std::array<std::int64_t, 3> extent{};  // cells of placements per window
  for (std::size_t a = 0; a < 3; ++a) {
    extent[a] = kWindowPlacements * step[a];
  }
  std::set<std::array<std::int64_t, 3>> windows;
  for (const Box& source : sources) {
    std::array<std::int64_t, 3> lo{};
    std::array<std::int64_t, 3> hi{};
    for (std::size_t a = 0; a < 3; ++a) {
      const std::int64_t first_cell = floorDiv(source.min[a], c[a]);
      const std::int64_t last_cell = floorDiv(source.max[a] - 1, c[a]);
      lo[a] = floorDiv(first_cell - n[a] + 1, extent[a]);
      hi[a] = floorDiv(last_cell, extent[a]);
    }
    for (std::int64_t z = lo[2]; z <= hi[2]; ++z) {
      for (std::int64_t y = lo[1]; y <= hi[1]; ++y) {
        for (std::int64_t x = lo[0]; x <= hi[0]; ++x) {
          windows.insert({x, y, z});
        }
      }
    }
  }
  const std::vector<std::array<std::int64_t, 3>> window_list(windows.begin(), windows.end());
  std::vector<ReferencePlacement> found(window_list.size());

  tbb::parallel_for(std::size_t{0}, window_list.size(), [&](std::size_t w) {
    // Cells of the window: its placements plus the reference size.
    std::array<std::int64_t, 3> origin{};
    std::array<std::int64_t, 3> size{};
    for (std::size_t a = 0; a < 3; ++a) {
      origin[a] = window_list[w][a] * extent[a];
      size[a] = extent[a] - step[a] + n[a];
    }
    const auto row_length = size[0] + 1;
    const auto rows = size[1] * size[2];
    std::vector<float> voids(static_cast<std::size_t>(row_length * rows), 0.0F);
    std::vector<std::uint16_t> bad(static_cast<std::size_t>(row_length * rows), 0);
    const auto index = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
      return static_cast<std::size_t>((z * size[1] + y) * row_length + x + 1);
    };
    std::array<std::int64_t, 3> voxel_lo{};
    std::array<std::int64_t, 3> voxel_hi{};  // exclusive
    for (std::size_t a = 0; a < 3; ++a) {
      voxel_lo[a] = origin[a] * c[a];
      voxel_hi[a] = (origin[a] + size[a]) * c[a];
    }

    // Cells outside the part or the region.
    const auto fill_acc = fill->getConstAccessor();
    for (std::int64_t z = 0; z < size[2]; ++z) {
      for (std::int64_t y = 0; y < size[1]; ++y) {
        for (std::int64_t x = 0; x < size[0]; ++x) {
          const std::array<std::int64_t, 3> cell{origin[0] + x, origin[1] + y, origin[2] + z};
          bool outside = false;
          std::array<std::int64_t, 3> b0{};
          std::array<std::int64_t, 3> b1{};
          BoxMm box{};
          for (std::size_t a = 0; a < 3; ++a) {
            const std::int64_t first = cell[a] * c[a];
            const std::int64_t last = first + c[a] - 1;
            b0[a] = floorDiv(first, kBlock);
            b1[a] = floorDiv(last, kBlock);
            box[0][a] = (static_cast<double>(first) - 0.5) * v[a];
            box[1][a] = (static_cast<double>(last) + 0.5) * v[a];
          }
          for (std::int64_t bz = b0[2]; bz <= b1[2] && !outside; ++bz) {
            for (std::int64_t by = b0[1]; by <= b1[1] && !outside; ++by) {
              for (std::int64_t bx = b0[0]; bx <= b1[0] && !outside; ++bx) {
                const Coord block(static_cast<int>(bx), static_cast<int>(by), static_cast<int>(bz));
                outside = fill_acc.getValue(block) < kFullBlock * block_volume;
              }
            }
          }
          if (!outside && !region.wholePart()) {
            outside = region.overlap(box) < 0.999;
          }
          bad[index(x, y, z)] = outside ? 1 : 0;
        }
      }
    }

    // Voids of the pores to count, voxel by voxel.
    const auto labels = result.pore_labels->getConstAccessor();
    const auto in_window = [&](const Coord& p) {
      return p.x() >= voxel_lo[0] && p.x() < voxel_hi[0] && p.y() >= voxel_lo[1] &&
             p.y() < voxel_hi[1] && p.z() >= voxel_lo[2] && p.z() < voxel_hi[2];
    };
    const auto add_void = [&](const Coord& p, double volume) {
      const std::int64_t x = floorDiv(p.x(), c[0]) - origin[0];
      const std::int64_t y = floorDiv(p.y(), c[1]) - origin[1];
      const std::int64_t z = floorDiv(p.z(), c[2]) - origin[2];
      voids[index(x, y, z)] += static_cast<float>(volume);
    };
    for (std::int64_t lz = floorDiv(voxel_lo[2], kBlock); lz * kBlock < voxel_hi[2]; ++lz) {
      for (std::int64_t ly = floorDiv(voxel_lo[1], kBlock); ly * kBlock < voxel_hi[1]; ++ly) {
        for (std::int64_t lx = floorDiv(voxel_lo[0], kBlock); lx * kBlock < voxel_hi[0]; ++lx) {
          const Coord leaf_origin(static_cast<int>(lx * kBlock), static_cast<int>(ly * kBlock),
                                  static_cast<int>(lz * kBlock));
          if (const auto* leaf = labels.probeConstLeaf(leaf_origin)) {
            for (auto it = leaf->cbeginValueOn(); it; ++it) {
              if (const auto weight = weights.find(*it);
                  weight != weights.end() && in_window(it.getCoord())) {
                add_void(it.getCoord(), weight->second);
              }
            }
          }
          if (result.zone_blocks) {
            const float deficit = result.zone_blocks->tree().getValue(
                Coord(static_cast<int>(lx), static_cast<int>(ly), static_cast<int>(lz)));
            if (deficit > 0.0F) {
              for (int k = 0; k < kBlock; ++k) {
                for (int j = 0; j < kBlock; ++j) {
                  for (int i = 0; i < kBlock; ++i) {
                    const Coord p = leaf_origin.offsetBy(i, j, k);
                    if (in_window(p)) {
                      add_void(p, deficit * voxel_volume);
                    }
                  }
                }
              }
            }
          }
        }
      }
    }

    // Prefix sums along x.
    for (std::int64_t r = 0; r < rows; ++r) {
      const auto base = static_cast<std::size_t>(r * row_length);
      for (std::int64_t x = 1; x < row_length; ++x) {
        voids[base + static_cast<std::size_t>(x)] += voids[base + static_cast<std::size_t>(x) - 1];
        bad[base + static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(
            bad[base + static_cast<std::size_t>(x)] + bad[base + static_cast<std::size_t>(x) - 1]);
      }
    }

    ReferencePlacement& window_best = found[w];
    window_best.volume_mm3 = best.volume_mm3;
    for (std::int64_t pz = 0; pz < extent[2]; pz += step[2]) {
      for (std::int64_t py = 0; py < extent[1]; py += step[1]) {
        for (std::int64_t px = 0; px < extent[0]; px += step[0]) {
          double sum = 0.0;
          bool valid = true;
          for (const Run& run : runs) {
            const auto base =
                static_cast<std::size_t>(((pz + run.z) * size[1] + py + run.y) * row_length);
            const auto begin = base + static_cast<std::size_t>(px + run.x0);
            const auto end = base + static_cast<std::size_t>(px + run.x1);
            if (bad[end] != bad[begin]) {
              valid = false;
              break;
            }
            sum += voids[end] - voids[begin];
          }
          if (!valid || sum <= 0.0) {
            continue;
          }
          const double porosity = 100.0 * sum / best.volume_mm3;
          if (!window_best.found || porosity > window_best.porosity_percent) {
            window_best.found = true;
            window_best.porosity_percent = porosity;
            const std::array<std::int64_t, 3> placement{origin[0] + px, origin[1] + py,
                                                        origin[2] + pz};
            for (std::size_t a = 0; a < 3; ++a) {
              window_best.center_mm[a] = (static_cast<double>(placement[a] * c[a]) +
                                          static_cast<double>(n[a] * c[a]) / 2.0 - 0.5) *
                                         v[a];
            }
          }
        }
      }
    }
  });
  for (const ReferencePlacement& placement : found) {
    if (placement.found && (!best.found || placement.porosity_percent > best.porosity_percent)) {
      best = placement;
    }
  }
  return best;
}

double regionPartVolumeMm3(const PorosityResult& result, const InspectionRegion& region,
                           const Dataset* dataset) {
  if (region.wholePart()) {
    return result.part_volume_mm3;
  }
  const VoxelSize& v = result.voxel_size;
  const double voxel_volume = v.volumeMm3();
  const double block_volume = kBlock * kBlock * kBlock * voxel_volume;
  const auto bounds = region.bounds();
  const auto center_mm = [&](const std::array<double, 3>& voxels) { return v.toMm(voxels); };
  const double contrast = static_cast<double>(result.material_level) - result.air_level;
  std::vector<float> grey;
  double volume = 0.0;
  if (result.material_blocks) {
    for (auto it = result.material_blocks->cbeginValueOn(); it; ++it) {
      const Coord block = it.getCoord();
      BoxMm box{};
      bool apart = false;
      for (std::size_t a = 0; a < 3; ++a) {
        box[0][a] = (block[a] * static_cast<double>(kBlock) - 0.5) * v[a];
        box[1][a] = box[0][a] + kBlock * v[a];
        apart = apart || (bounds && (box[1][a] < (*bounds)[0][a] || box[0][a] > (*bounds)[1][a]));
      }
      if (apart) {
        continue;
      }
      const double share = region.overlap(box);
      const bool partial = share > 0.0 && share < 1.0;
      // Blocks well below full hold air: at the surface of the part.
      if (partial && dataset != nullptr && contrast > 0.0 && *it < kFullBlock * block_volume) {
        Box voxels;
        for (std::size_t a = 0; a < 3; ++a) {
          voxels.min[a] = std::int64_t{block[a]} * kBlock;
          voxels.max[a] = std::min(voxels.min[a] + kBlock, dataset->info().dims[a]);
        }
        if (voxels.voxelCount() <= 0) {
          continue;
        }
        grey.assign(static_cast<std::size_t>(voxels.voxelCount()), 0.0F);
        dataset->readRegion(0, voxels, grey, result.air_level);
        std::size_t i = 0;
        double material = 0.0;
        for (std::int64_t z = voxels.min[2]; z < voxels.max[2]; ++z) {
          for (std::int64_t y = voxels.min[1]; y < voxels.max[1]; ++y) {
            for (std::int64_t x = voxels.min[0]; x < voxels.max[0]; ++x, ++i) {
              const PointMm p{static_cast<double>(x) * v[0], static_cast<double>(y) * v[1],
                              static_cast<double>(z) * v[2]};
              if (region.contains(p)) {
                material += (grey[i] - result.air_level) / contrast;
              }
            }
          }
        }
        volume += material * voxel_volume;
        continue;
      }
      volume += share * *it;
    }
  }
  for (const DetectedPore& pore : result.pores) {
    if (region.contains(center_mm(pore.center_voxels))) {
      volume += pore.volume_mm3;
    }
  }
  for (const PorosityZone& zone : result.zones) {
    if (region.contains(center_mm(zone.center_voxels))) {
      volume += zone.void_volume_mm3;
    }
  }
  return volume;
}

PoreGap smallestPoreGap(const PorosityResult& result, const std::vector<std::size_t>& pores) {
  PoreGap best;
  if (pores.size() < 2 || !result.pore_labels) {
    return best;
  }
  const VoxelSize& v = result.voxel_size;
  // The voxels of each pore that face another voxel, as a mask per pore.
  std::map<int, openvdb::BoolGrid::Ptr> masks;
  for (const std::size_t i : pores) {
    masks[result.pores[i].id] = openvdb::BoolGrid::create(false);
  }
  const auto labels = result.pore_labels->getConstAccessor();
  const std::array<Coord, 6> faces{Coord(1, 0, 0),  Coord(-1, 0, 0), Coord(0, 1, 0),
                                   Coord(0, -1, 0), Coord(0, 0, 1),  Coord(0, 0, -1)};
  for (auto it = result.pore_labels->cbeginValueOn(); it; ++it) {
    const auto mask = masks.find(*it);
    if (mask == masks.end()) {
      continue;
    }
    const Coord p = it.getCoord();
    if (std::any_of(faces.begin(), faces.end(),
                    [&](const Coord& d) { return labels.getValue(p + d) != *it; })) {
      mask->second->tree().setValueOn(p);
    }
  }
  std::vector<std::size_t> order = pores;
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return result.pores[a].bounds.min[0] < result.pores[b].bounds.min[0];
  });
  const auto bbox = [&](std::size_t i) {
    const Box& b = result.pores[i].bounds;
    return openvdb::CoordBBox(
        Coord(static_cast<int>(b.min[0]), static_cast<int>(b.min[1]), static_cast<int>(b.min[2])),
        Coord(static_cast<int>(b.max[0] - 1), static_cast<int>(b.max[1] - 1),
              static_cast<int>(b.max[2] - 1)));
  };
  for (std::size_t i = 0; i < order.size(); ++i) {
    const auto box_i = bbox(order[i]);
    for (std::size_t j = i + 1; j < order.size(); ++j) {
      const auto box_j = bbox(order[j]);
      // Sorted by their start in x: once the gap in x alone is too large, it only grows.
      const double gap_x =
          std::max(0, box_j.min().x() - box_i.max().x() - 1) * static_cast<double>(v[0]);
      if (gap_x >= best.gap_mm) {
        break;
      }
      if (detail::voxelGapMm(box_i, box_j, v) >= best.gap_mm) {
        continue;
      }
      const int id_i = result.pores[order[i]].id;
      const int id_j = result.pores[order[j]].id;
      const double gap =
          detail::maskGapMm(masks[id_i]->tree(), masks[id_j]->tree(), v, best.gap_mm);
      if (gap < best.gap_mm) {
        best = {gap, std::min(id_i, id_j), std::max(id_i, id_j)};
      }
    }
  }
  return best;
}

}  // namespace voxelsieve
