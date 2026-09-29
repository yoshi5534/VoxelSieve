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
                                                      const std::array<std::int64_t, 3>& brick);

/// Writes materials.json of `info` into `dir`.
void writeMaterialVolumeInfo(const std::filesystem::path& dir, const MaterialVolumeInfo& info);

}  // namespace voxelsieve::detail
