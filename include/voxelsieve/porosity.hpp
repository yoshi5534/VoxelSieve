#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// Internal void that is resolved as voxels below the air/material threshold.
struct DetectedPore {
  int id = 0;
  std::int64_t voxel_count = 0;  // voxels below the threshold
  /// Void volume from the grey values of the pore and a one-voxel shell around it, so partially
  /// filled edge voxels count with their fraction.
  double volume_mm3 = 0.0;
  /// Void-weighted centre in level-0 voxel coordinates.
  std::array<double, 3> center_voxels{};
  Box bounds;
  /// Diameter of the sphere with the same volume.
  double equivalent_diameter_mm = 0.0;
};

/// Region whose grey values are lowered by voids too small to resolve, such as loosened
/// microstructure (Gefügeauflockerung). Detected per 8^3 block against the material level at the
/// same depth below the surface, so cupping from beam hardening does not show up as a zone.
struct PorosityZone {
  int id = 0;
  std::int64_t block_count = 0;
  double volume_mm3 = 0.0;       // volume of the flagged blocks
  double void_volume_mm3 = 0.0;  // grey-value deficit, including a one-block rim
  /// Deficit-weighted centre in level-0 voxel coordinates.
  std::array<double, 3> center_voxels{};
  Box bounds;
  /// Mean void fraction inside the zone.
  [[nodiscard]] double porosity() const {
    return volume_mm3 > 0.0 ? void_volume_mm3 / volume_mm3 : 0.0;
  }
};

struct PorosityOptions {
  /// Pores with fewer voxels below the threshold are ignored (noise); their voids still count in
  /// zones.
  std::int64_t min_pore_voxels = 3;
  /// Detection limit for zones: a block is flagged when its void fraction exceeds both this value
  /// and `zone_sigma` standard deviations of the block mean.
  double min_zone_void_fraction = 0.01;
  double zone_sigma = 5.0;
  /// Zones with fewer flagged blocks are ignored.
  std::int64_t min_zone_blocks = 2;
};

struct PorosityResult {
  VoxelSize voxel_size;
  float air_level = 0.0F;
  /// Global material grey value (median of fully material block means).
  float material_level = 0.0F;
  /// Standard deviation of the grey value within material (noise).
  double noise_sigma = 0.0;
  /// Volume enclosed by the outer surface of the part, voids included.
  double part_volume_mm3 = 0.0;
  std::vector<DetectedPore> pores;  // largest first
  std::vector<PorosityZone> zones;  // largest void volume first

  /// Pore id per pore voxel, level-0 index space.
  openvdb::Int32Grid::Ptr pore_labels;
  /// Void fraction per 8^3 block (index = block coordinate), active where a zone was flagged.
  openvdb::FloatGrid::Ptr zone_blocks;
  /// Material volume in mm^3 per 8^3 block (index = block coordinate), voids excluded. Used for
  /// the part volume of a region; costs about 4 bytes per block of the part.
  openvdb::FloatGrid::Ptr material_blocks;

  /// Part volume of an axis-aligned box in mm (dataset coordinates, voxel index times voxel
  /// size): the material of the blocks overlapping the box, weighted by the overlap, plus the
  /// pores and zones whose centre lies in the box.
  [[nodiscard]] double partVolumeMm3(const std::array<std::array<double, 3>, 2>& box_mm) const;

  [[nodiscard]] double poreVolumeMm3() const;
  [[nodiscard]] double zoneVoidVolumeMm3() const;
  /// Void volume of pores and zones relative to the part volume.
  [[nodiscard]] double porosity() const;
};

/// Finds internal pores and zones of lowered density in a sieved dataset (level 0). Streams the
/// dataset brick by brick; memory grows with the number of voxels below the threshold, not with
/// the scan size.
[[nodiscard]] PorosityResult analyzePorosity(const Dataset& dataset,
                                             const PorosityOptions& options = {});

[[nodiscard]] nlohmann::json toJson(const PorosityResult& result);

/// Saves a result so it can be loaded again: porosity.json plus analysis.vdb with the label,
/// zone and material grids in index space.
void savePorosityResult(const PorosityResult& result, const std::filesystem::path& dir);
/// Loads a result written by `savePorosityResult`.
[[nodiscard]] PorosityResult loadPorosityResult(const std::filesystem::path& dir);

/// Writes projections along x, y and z as PNG images: the part as a grey thickness image, pores in
/// red and zones in yellow. Images wider than `max_pixels` are downsampled.
void writePorosityImages(const Dataset& dataset, const PorosityResult& result,
                         const std::filesystem::path& dir, std::int64_t max_pixels = 1024);

/// Writes a VDB file with the grid "pores" (void fraction of pore voxels) and the grid "zones"
/// (void fraction of flagged blocks), in the dataset's world space, for viewing next to the part
/// in Blender or Houdini.
void writePorosityVdb(const Dataset& dataset, const PorosityResult& result,
                      const std::filesystem::path& file);

}  // namespace voxelsieve
