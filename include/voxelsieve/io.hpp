#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <ostream>
#include <string_view>

#include "voxelsieve/phantom.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/volume.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// Writes the voxels as headerless little-endian uint16, x fastest.
void writeRaw(const std::filesystem::path& path, const Volume16& volume);

/// Streams `source` to a headerless little-endian uint16 file, a slab of slices at a time, so the
/// volume never has to fit in memory. Slices of a slab are read in parallel.
void writeRaw(const std::filesystem::path& path, const VolumeSource& source);

/// Reads a headerless little-endian uint16 file. Throws std::runtime_error if the file size does
/// not match `dims`.
[[nodiscard]] Volume16 readRaw(const std::filesystem::path& path,
                               const std::array<std::int64_t, 3>& dims,
                               const VoxelSize& voxel_size);

/// Sidecar metadata describing a raw file and, for phantoms, its ground truth.
[[nodiscard]] nlohmann::json phantomToJson(const PhantomSpec& spec);

/// Writes a file with `write`, into a temporary file beside it that replaces `path` only once all
/// of it is written and closed, so `path` is never left half written. When a write fails it
/// throws std::runtime_error naming the file, and a full disk as the reason, and leaves no
/// temporary file behind.
void writeFile(const std::filesystem::path& path, const std::function<void(std::ostream&)>& write);

/// `writeFile` of a text or a block of bytes.
void writeFile(const std::filesystem::path& path, std::string_view bytes);

/// Throws std::runtime_error about a failed write of `path`, naming a full disk as the reason
/// when its device has (almost) no space left.
[[noreturn]] void throwWriteError(const std::filesystem::path& path);

/// Writes JSON with `writeFile`.
void writeJson(const std::filesystem::path& path, const nlohmann::json& json);

}  // namespace voxelsieve
