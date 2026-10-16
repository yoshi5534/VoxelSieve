#pragma once

// Measurements of the objects of a material segmentation: each connected piece of a material, such
// as a separated part, a bone or an organ, with its volume, surface, centroid and lengths.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "voxelsieve/materials.hpp"

namespace voxelsieve {

struct MeasureOptions {
  /// Materials whose objects are measured; empty: every material. 0 measures enclosed air: the
  /// voids that do not reach the border of the volume, such as a swim bladder or a cavity.
  std::vector<int> material_ids;
  /// Objects smaller than this many level-0 voxels are left out (noise, specks).
  std::int64_t min_voxels = 27;
  /// Objects measured at most, the largest first.
  int max_objects = 100;
  /// A direction along the volume's x, y and z axes (need not be normalised) to measure every
  /// object's length along, such as [0, 0, 1] for its length along z; default: none.
  std::optional<std::array<double, 3>> axis;
  /// A material volume is sampled at every 2nd, 4th, ... voxel until it has at most this many
  /// samples; the measurements then refer to that grid (MeasureInfo::stride).
  std::int64_t max_samples = std::int64_t{1} << 26;
};

/// One connected object of a material (6-connected: voxels that share a face).
struct MeasuredObject {
  int material = 0;
  std::string material_name;
  std::int64_t voxels = 0;  // level-0 voxels (samples times stride^3)
  double volume_mm3 = 0.0;
  /// Area of the object's surface (marching cubes on the voxels, so curved surfaces are not
  /// overestimated as voxel faces would be).
  double surface_mm2 = 0.0;
  std::array<double, 3> centroid_mm{};
  std::array<double, 3> min_mm{};
  std::array<double, 3> max_mm{};
  /// Principal axes (unit vectors, longest first) and the object's length along each.
  std::array<std::array<double, 3>, 3> principal_axes{};
  std::array<double, 3> principal_lengths_mm{};
  /// Length along MeasureOptions::axis, if given.
  std::optional<double> axis_length_mm;
  /// Whether the object reaches the border of the volume, where it may be cut off.
  bool touches_border = false;
};

struct MeasureInfo {
  std::int64_t stride = 1;
  /// Objects found before min_voxels and max_objects applied.
  std::int64_t objects_found = 0;
  std::vector<MeasuredObject> objects;  // largest first
};

/// Finds and measures the objects of the materials of `volume`. Coordinates are in mm in the
/// volume's own coordinates, voxel centres at i * pitch like the material meshes.
[[nodiscard]] MeasureInfo measureObjects(const MaterialVolume& volume,
                                         const MeasureOptions& options = {});

/// The same for labels in memory: one material id per voxel, x fastest.
[[nodiscard]] MeasureInfo measureObjects(std::span<const std::uint8_t> labels,
                                         const std::array<std::int64_t, 3>& dims,
                                         const VoxelSize& voxel_size,
                                         const std::vector<Material>& materials,
                                         const MeasureOptions& options = {});

/// Writes the objects as a table: one row per object, comma separated, with a header row.
void writeObjectsCsv(const std::filesystem::path& file, const MeasureInfo& info);

}  // namespace voxelsieve
