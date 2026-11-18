#pragma once

// Writing material volumes, shared by the threshold and the model segmentation.

#include <array>
#include <cstdint>
#include <filesystem>

#include "voxelsieve/materials.hpp"

namespace voxelsieve::detail {

/// Colour of the material class with 1-based id `id`, from light to dense.
[[nodiscard]] std::array<std::uint8_t, 3> materialColor(int id);

[[nodiscard]] std::filesystem::path materialBrickFile(const std::filesystem::path& dir,
                                                      const std::array<std::int64_t, 3>& brick,
                                                      int level = 0);

/// Writes the label grid of one brick as a UInt8Grid, pruned: leaves of one material become
/// tiles, which keeps uniform material out of the file and out of memory when it is read back.
void writeMaterialBrick(const std::filesystem::path& dir, const std::array<std::int64_t, 3>& brick,
                        openvdb::Int32Grid& grid);

/// Writes the coarser label levels from level 0 (the bricks of `info`) into `dir` and lists their
/// bricks in `info.levels`: each voxel the majority of the 2 × 2 × 2 voxels below it.
void writeMaterialLevels(const std::filesystem::path& dir, MaterialVolumeInfo& info);

/// Writes materials.json of `info` into `dir`.
void writeMaterialVolumeInfo(const std::filesystem::path& dir, const MaterialVolumeInfo& info);

}  // namespace voxelsieve::detail
