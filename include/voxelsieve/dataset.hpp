#pragma once

#include <openvdb/openvdb.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "voxelsieve/source.hpp"

namespace voxelsieve {

/// Bricked, multi-resolution dataset on disk, see docs/adr/0004-out-of-core-bricked-dataset.md.
///
///   <dir>/index.json              metadata and the list of bricks per level
///   <dir>/overview.vdb            whole volume in one grid (coarsest level)
///   <dir>/level<L>/<x>_<y>_<z>.vdb one FloatGrid "density" per brick
///
/// Level-0 voxel (i, j, k) of the scan has VDB index (i, j, k) in every level-0 brick. A level-L
/// voxel covers 2^L level-0 voxels per axis; its value is the mean of its active children.

struct DatasetOptions {
  /// Air/material grey value; Otsu estimate from the full histogram when unset.
  std::optional<float> threshold;
  /// Voxels of air kept around the part.
  int margin_voxels = 3;
  /// Brick edge length in voxels, a multiple of 8. The overview has at most this size.
  std::int64_t brick_size = 256;
};

struct LevelInfo {
  int level = 0;
  std::array<std::int64_t, 3> dims{};
  double voxel_size_mm = 0.0;
  std::vector<std::array<std::int64_t, 3>> bricks;
};

struct DatasetInfo {
  std::array<std::int64_t, 3> dims{};
  double voxel_size_mm = 0.0;
  std::int64_t brick_size = 0;
  float threshold = 0.0F;
  float air_level = 0.0F;
  int margin_voxels = 0;
  std::int64_t active_voxel_count = 0;
  std::vector<LevelInfo> levels;
};

/// Sieves `source` in two streaming passes and writes the dataset to `dir`, which must not exist
/// or be empty. Memory use is bounded by the per-block maxima (2 bytes per 8^3 block) and a few
/// bricks per thread, independent of the volume size.
DatasetInfo writeDataset(const VolumeSource& source, const std::filesystem::path& dir,
                         const DatasetOptions& options = {});

[[nodiscard]] DatasetInfo readDatasetInfo(const std::filesystem::path& dir);

[[nodiscard]] std::filesystem::path brickPath(const std::filesystem::path& dir, int level,
                                              const std::array<std::int64_t, 3>& brick);

/// Opens one brick. With `delay_load`, voxel values are read from the memory-mapped file on first
/// access.
[[nodiscard]] openvdb::FloatGrid::Ptr readBrick(const std::filesystem::path& file,
                                                bool delay_load = true);

}  // namespace voxelsieve
