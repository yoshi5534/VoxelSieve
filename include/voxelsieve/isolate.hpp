#pragma once

// Isolating the part in a dataset (ADR 0026): voxels outside a region of interest and objects
// other than the part, such as a holder or a support that the scan also shows, are removed like
// outside air, so that every later analysis (materials, porosity, surface, volumes) sees the part
// alone.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/source.hpp"

namespace voxelsieve {

/// A plane in level-0 voxel coordinates; isolatePart removes what lies on the side its normal
/// points to, for example a holder below the part.
struct CutPlane {
  std::array<double, 3> point{};
  std::array<double, 3> normal{0.0, 0.0, -1.0};
};

/// A cylinder along a volume axis in level-0 voxel coordinates; isolatePart keeps only its inside,
/// for example to cut a part out of a tube it lies in.
struct RegionCylinder {
  int axis = 2;
  /// Centre in the two other axes, in ascending axis order (x and y for axis z).
  std::array<double, 2> center{};
  double radius = 0.0;
};

struct IsolateOptions {
  /// Level-0 voxels to keep at most (a crop); default: the whole volume.
  std::optional<Box> region;
  /// Planes that cut away what lies on the side of their normal.
  std::vector<CutPlane> cut_planes;
  /// Keep only the inside of this cylinder.
  std::optional<RegionCylinder> cylinder;
  /// Separates objects that touch through necks thinner than about twice this many voxels: the
  /// material is eroded by it before the objects are found, and the kept ones grow back as far
  /// within the material. 0: objects are separated only by air.
  int separate_voxels = 0;
  /// Objects kept: the largest ones, counted in material voxels; 0 keeps every object (only the
  /// region applies). An object is material connected across faces of cells of the search grid,
  /// together with everything it encloses (its voids and pores).
  int keep_objects = 1;
  /// Grey value above which a voxel is material; default: the dataset's threshold.
  std::optional<float> threshold;
  /// The objects are found on a grid of cells of 2^k level-0 voxels per axis, the finest with at
  /// most this many cells. A cell is material when at least 2^k of its voxels are, so a sheet one
  /// voxel thin stays connected while lone noise voxels do not join objects.
  std::int64_t max_cells = std::int64_t{1} << 27;
  /// Called with the stage ("objects", then "bricks" and "levels") and the fraction done.
  std::function<void(std::string_view stage, double fraction)> progress;
};

struct IsolateInfo {
  DatasetInfo dataset;
  /// Level-0 voxels per cell edge of the search grid.
  std::int64_t cell_size = 1;
  /// Objects found in the region, and the material voxels of the kept ones, largest first (with
  /// separate_voxels, of their eroded cores).
  std::int64_t objects = 0;
  std::vector<std::int64_t> kept_material_voxels;
  /// Active voxels of the input that were removed.
  std::int64_t removed_voxels = 0;
};

/// Writes `dataset` without what lies outside the region or belongs to other objects than the
/// largest `keep_objects` to `dir` (which must not exist or be empty). The kept objects keep the
/// dataset's margin of air plus a voxel, rounded up to whole cells, so a holder that touches the
/// part or comes closer than that keeps a stub there. Memory: about 5 bytes per cell.
IsolateInfo isolatePart(const Dataset& dataset, const std::filesystem::path& dir,
                        const IsolateOptions& options = {});

}  // namespace voxelsieve
