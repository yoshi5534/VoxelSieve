#pragma once

#include <openvdb/openvdb.h>

#include <cstdint>
#include <filesystem>
#include <optional>

#include "voxelsieve/volume.hpp"

namespace voxelsieve {

/// Edge length of a sieve block in voxels. Matches the OpenVDB leaf size so that every kept
/// block becomes exactly one leaf node.
inline constexpr std::int64_t kBlockSize = 8;

struct SieveOptions {
  /// Grey value separating air from material. Estimated with Otsu's method when unset.
  std::optional<float> threshold;
  /// Voxels of air kept around the part so sub-voxel surface determination still sees air.
  int margin_voxels = 3;
  /// Every n-th voxel per axis is sampled for the threshold histogram.
  int histogram_stride = 4;
  /// A block counts as material once it holds at least this many voxels above the threshold.
  /// Values above 1 make the sieve robust against isolated noise spikes in the air.
  int min_material_voxels = 1;
};

struct SieveStats {
  float threshold = 0.0F;
  /// Mean grey value of the sampled voxels below the threshold.
  float air_level = 0.0F;
  std::int64_t block_count = 0;
  std::int64_t material_block_count = 0;
  std::int64_t outside_air_block_count = 0;
  std::int64_t voxel_count = 0;
  std::int64_t active_voxel_count = 0;
};

struct SieveResult {
  openvdb::FloatGrid::Ptr grid;
  SieveStats stats;
};

/// Otsu threshold over a subsampled grey-value histogram.
[[nodiscard]] float estimateThreshold(const Volume16& volume, int stride);

/// Removes air connected to the volume boundary and keeps the part, its internal voids (pores,
/// closed cavities) and a margin of air around it. See docs/adr/0003-remove-only-outside-air.md.
///
/// Active voxels carry the original grey values; voxel (i, j, k) of the volume maps to VDB index
/// (i, j, k). Inactive voxels hold the background value 0. Threshold, air level and margin are
/// stored as grid metadata.
[[nodiscard]] SieveResult sieve(const Volume16& volume, const SieveOptions& options = {});

/// Writes grids to a .vdb file with Blosc compression when available, otherwise zip.
void writeVdb(const std::filesystem::path& path, const openvdb::GridPtrVec& grids);

}  // namespace voxelsieve
