#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>

#include "voxelsieve/phantom.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/volume.hpp"

namespace voxelsieve {

/// Writes the voxels as headerless little-endian uint16, x fastest.
void writeRaw(const std::filesystem::path& path, const Volume16& volume);

/// Streams `source` to a headerless little-endian uint16 file, a slab of slices at a time, so the
/// volume never has to fit in memory. Slices of a slab are read in parallel.
void writeRaw(const std::filesystem::path& path, const VolumeSource& source);

/// Reads a headerless little-endian uint16 file. Throws std::runtime_error if the file size does
/// not match `dims`.
[[nodiscard]] Volume16 readRaw(const std::filesystem::path& path,
                               const std::array<std::int64_t, 3>& dims, double voxel_size_mm);

/// Sidecar metadata describing a raw file and, for phantoms, its ground truth.
[[nodiscard]] nlohmann::json phantomToJson(const PhantomSpec& spec);

void writeJson(const std::filesystem::path& path, const nlohmann::json& json);

}  // namespace voxelsieve
