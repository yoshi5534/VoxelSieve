#pragma once

// Volumes described by a text header (ADR 0043): MetaImage (.mhd with a separate data file, .mha
// with the data attached) and NRRD (.nhdr detached, .nrrd attached). The header gives the
// dimensions, the sample type and byte order, the voxel spacing, the position and the data file;
// the voxels are read by the raw sources, plain or decompressed with zlib.

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "voxelsieve/source.hpp"
#include "voxelsieve/transform.hpp"

namespace voxelsieve {

/// What the header of a MetaImage or NRRD file says.
struct HeaderVolume {
  /// "metaimage" or "nrrd".
  std::string format;
  /// Dimensions, sample type, byte order, header size; `compressed_offset` for attached
  /// compressed data.
  RawLayout layout;
  /// The data files: one, or one per slab of slices (a list or a numbered pattern), in order.
  std::vector<std::filesystem::path> files;
  /// gzip- or zlib-compressed data.
  bool compressed = false;
  /// Whether the header gives the spacing; else 1 mm is assumed.
  bool voxel_size_known = false;
  /// Where the volume lies: the centre of voxel 0 and the directions of the axes, in mm. The
  /// identity when the header gives neither, or when the axes are mirrored (`mirrored`).
  RigidTransform pose;
  /// The axes of the header form a left-handed system: the pose keeps only the position.
  bool mirrored = false;
  /// Things the header gives that are not used, for the user (e.g. "4 channels").
  std::vector<std::string> notes;
};

/// Whether `path` is a MetaImage or NRRD header, by its extension (.mhd, .mha, .nhdr, .nrrd)
/// and its first bytes.
[[nodiscard]] bool isHeaderVolumeFile(const std::filesystem::path& path);

/// Reads the header of a MetaImage or NRRD file. Throws std::invalid_argument for headers that
/// are not understood or describe something that is not one grey-value volume (several
/// channels, more than three dimensions, 64-bit integers, text or bzip2 encoding).
[[nodiscard]] HeaderVolume readHeaderVolume(const std::filesystem::path& path);

/// The volume of a header read by readHeaderVolume, with `voxel_size` and `value_range` (32-bit
/// and float samples) overriding the header's when set. Data in several files is read as one
/// volume (ConcatSource), with one value range for all of them.
[[nodiscard]] std::unique_ptr<VolumeSource> openHeaderVolume(
    const HeaderVolume& header, const std::optional<VoxelSize>& voxel_size = std::nullopt,
    const std::optional<std::array<double, 2>>& value_range = std::nullopt);

/// The value range of a source opened by openHeaderVolume or openRawVolume and the count of
/// values clipped so far; nothing for sources whose samples are kept exactly.
struct ClippedValues {
  std::array<double, 2> range{0.0, 65535.0};
  std::uint64_t clipped = 0;
};
[[nodiscard]] std::optional<ClippedValues> rawClippedValues(const VolumeSource& source);

}  // namespace voxelsieve
