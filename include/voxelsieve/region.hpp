#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/transform.hpp"

namespace voxelsieve {

/// Points and boxes in mm in the dataset's coordinates (voxel index times voxel size).
using PointMm = std::array<double, 3>;
using BoxMm = std::array<PointMm, 2>;  // min and max corner

/// One solid of an inspection region (ADR 0031).
struct RegionShape {
  enum class Kind : std::uint8_t { kBox, kSphere, kCylinder, kMesh };
  Kind kind = Kind::kBox;
  BoxMm box_mm{};       // box
  PointMm center_mm{};  // sphere
  PointMm start_mm{};   // cylinder: the centres of its end faces
  PointMm end_mm{};
  double radius_mm = 0.0;  // sphere, cylinder
  /// Closed, outward-oriented mesh (STL) and the pose from its coordinates to the dataset's.
  std::filesystem::path mesh_path;
  RigidTransform mesh_pose;
  /// Name of the project object the mesh is, for the report; empty for a file.
  std::string mesh_name;

  [[nodiscard]] static RegionShape box(const BoxMm& box_mm);
  [[nodiscard]] static RegionShape sphere(const PointMm& center_mm, double radius_mm);
  [[nodiscard]] static RegionShape cylinder(const PointMm& start_mm, const PointMm& end_mm,
                                            double radius_mm);
  [[nodiscard]] static RegionShape mesh(const std::filesystem::path& path,
                                        const RigidTransform& pose = {});
};

/// A freely defined inspection region (BDG P 203: ROI): the union of the included solids minus
/// the union of the excluded ones. Without included solids it is the whole part (minus the
/// excluded ones). Copies share the loaded meshes.
class InspectionRegion {
 public:
  InspectionRegion() = default;
  InspectionRegion(const std::vector<RegionShape>& include,
                   const std::vector<RegionShape>& exclude = {});

  /// Reads the region of an inspection zone of an order: "box_mm" ([[x0, y0, z0], [x1, y1, z1]])
  /// and "region", either a list of solids or {"include": [...], "exclude": [...]}. A solid is
  /// {"box_mm": ...}, {"sphere": {"center_mm", "radius_mm"}}, {"cylinder": {"from_mm", "to_mm",
  /// "radius_mm"}} or {"mesh": "file.stl", "pose": row-major 4x4}. Relative mesh paths are taken
  /// from `base_dir`. {"mesh_object": id} names a mesh of a project, which the operations replace
  /// by its file and pose first; here it throws.
  [[nodiscard]] static InspectionRegion fromJson(const nlohmann::json& zone,
                                                 const std::filesystem::path& base_dir = {});

  [[nodiscard]] bool wholePart() const { return include_.empty() && exclude_.empty(); }
  [[nodiscard]] bool contains(const PointMm& p_mm) const;
  /// Share of the volume of an axis-aligned box inside the region: exact where the box lies
  /// inside or outside every solid or the region is made of boxes, else from 4^3 samples.
  [[nodiscard]] double overlap(const BoxMm& box_mm) const;
  /// Bounding box of the included solids; nullopt for the whole part.
  [[nodiscard]] std::optional<BoxMm> bounds() const;
  /// "box from (0, 0, 0) to (10, 5, 2) mm", "whole part without sphere at ...".
  [[nodiscard]] std::string description() const;
  [[nodiscard]] std::vector<RegionShape> include() const;
  [[nodiscard]] std::vector<RegionShape> exclude() const;
  /// The included or excluded solids as one closed mesh each (spheres and cylinders
  /// tessellated), for showing the region next to the part.
  [[nodiscard]] Mesh mesh(bool excluded = false) const;

 private:
  struct Solid;
  std::vector<std::shared_ptr<const Solid>> include_;
  std::vector<std::shared_ptr<const Solid>> exclude_;
};

/// Geometrically defined reference volume of BDG P 203 (PV): a cube unless agreed otherwise.
struct ReferenceVolume {
  enum class Shape : std::uint8_t { kCube, kCuboid, kSphere, kCylinder };
  Shape shape = Shape::kCube;
  /// Edges of a cube or cuboid; the diameter on all axes of a sphere; diameter, diameter and
  /// height (along `axis`) of a cylinder.
  PointMm size_mm{};
  int axis = 2;  // cylinder axis, 0 = x

  /// {"shape": "cube", "edge_mm"}, {"shape": "cuboid", "size_mm": [x, y, z]}, {"shape":
  /// "sphere", "diameter_mm"} or {"shape": "cylinder", "diameter_mm", "height_mm", "axis": "z"}.
  [[nodiscard]] static ReferenceVolume fromJson(const nlohmann::json& json);
  [[nodiscard]] std::string description() const;
  [[nodiscard]] double volumeMm3() const;
};

/// The placement of a reference volume with the highest void fraction.
struct ReferencePlacement {
  double porosity_percent = 0.0;
  PointMm center_mm{};
  /// Volume of the voxels the reference volume covers.
  double volume_mm3 = 0.0;
  /// Whether any placement inside the part and the region covers a void; without one the
  /// porosity is 0.
  bool found = false;
};

/// Highest void fraction of `reference` placed anywhere completely inside the part and `region`
/// (BDG P 203: PV). Counts the pores flagged in `considered` (by index into `result.pores`) and
/// the loosened zones. Placements are tried on a grid of a sixteenth of the reference size near
/// the voids; the part is known per 8^3 block, so a placement must lie in blocks that are full of
/// part, which keeps it up to one block off the surface. Large references are evaluated on cells
/// of several voxels.
[[nodiscard]] ReferencePlacement maxReferencePorosity(const PorosityResult& result,
                                                      const InspectionRegion& region,
                                                      const ReferenceVolume& reference,
                                                      const std::vector<bool>& considered);

/// Part volume inside `region`: the material of the 8^3 blocks weighted by their overlap with the
/// region, plus the pores and zones whose centre lies in it. A block at the surface of the part
/// holds its material on one side only, so where the region's boundary also crosses it (a region
/// from the CAD model ends at the surface), the overlap would count too little: with `dataset`
/// (the analysed one), such blocks count the material of their voxels inside the region instead.
[[nodiscard]] double regionPartVolumeMm3(const PorosityResult& result,
                                         const InspectionRegion& region,
                                         const Dataset* dataset = nullptr);

/// The two pores closest to each other and the material between them (BDG P 203: Gap).
struct PoreGap {
  double gap_mm = std::numeric_limits<double>::infinity();
  int first = 0;  // pore ids
  int second = 0;
};

/// Smallest gap between the voxels of any two of `pores` (indices into `result.pores`), from the
/// pore labels of the result.
[[nodiscard]] PoreGap smallestPoreGap(const PorosityResult& result,
                                      const std::vector<std::size_t>& pores);

}  // namespace voxelsieve
