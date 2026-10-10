#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "voxelsieve/source.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// TIFF image stacks as volume source (ADR 0011): a directory of slices, one multi-page TIFF, or
/// a ZIP archive holding either, read without extracting it. Slices are sorted by name with
/// numbers compared by value (slice2 before slice10). Grey values must be unsigned 8 or 16 bit,
/// or 32 bit up to 65535, so that they are kept exactly. Float slices (32 or 64 bit) are mapped
/// linearly onto 0..65535 over a value range, and the mapping is kept (ADR 0015).

struct TiffStackOptions {
  /// Folder of the slices below the directory or inside the archive. Empty: the only folder with
  /// TIFF files; with several, the one with the most slices whose name does not look like labels
  /// (label, mask, target, seg, gt).
  std::string folder;
  /// Overrides the voxel size of the files (edge length per axis, slice thickness).
  std::optional<VoxelSize> voxel_size;
  /// Float slices: the values mapped to grey 0 and 65535. Empty: estimated from a few slices
  /// spread over the stack (33), widened by 10 % on each side. Values outside are clipped and
  /// counted.
  std::optional<std::array<double, 2>> value_range;
  /// Memory for decoded strips and tiles.
  std::size_t cache_bytes = std::size_t{1} << 30;
};

class TiffStackSource final : public VolumeSource {
 public:
  explicit TiffStackSource(const std::filesystem::path& path, const TiffStackOptions& options = {});
  ~TiffStackSource() override;

  [[nodiscard]] std::array<std::int64_t, 3> dims() const override;
  /// Given, else from the files, else 1 mm (see fileVoxelSize).
  [[nodiscard]] VoxelSize voxelSize() const override;
  /// Identity for integer slices; for float slices value = offset + scale * grey.
  [[nodiscard]] ValueMapping valueMapping() const override;
  /// 8 bit for 8-bit integer slices.
  [[nodiscard]] SampleType sampleType() const override;
  /// True: slices are decoded strip by strip, and a slice in a ZIP archive as a whole.
  [[nodiscard]] bool slowRandomAccess() const override { return true; }
  /// Empties the cache of decoded strips and tiles and drops the pages of a ZIP archive.
  void releaseMemory() const override;
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

  /// Voxel size from the files: pixel width and height from a centimetre resolution or an ImageJ
  /// description, the slice spacing from an ImageJ description ("spacing=") or else the pixel
  /// width. Nothing if the files give no pixel size.
  [[nodiscard]] std::optional<VoxelSize> fileVoxelSize() const;
  [[nodiscard]] int bitsPerSample() const;
  [[nodiscard]] bool isFloat() const;
  /// Float slices: the values mapped to grey 0 and 65535, as given or estimated.
  [[nodiscard]] std::array<double, 2> valueRange() const;
  /// Float values read so far that lay outside the value range or were not finite, each counted
  /// once; they became grey 0 or 65535 (not a number: 0).
  [[nodiscard]] std::uint64_t clippedValues() const;
  /// Folder the slices come from ("" for the top level or a single file).
  [[nodiscard]] const std::string& folder() const;
  /// Folders with TIFF files that were not chosen, such as label volumes.
  [[nodiscard]] const std::vector<std::string>& otherFolders() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Whether `path` looks like a TIFF stack: a .tif/.tiff file, a .zip archive or a directory.
[[nodiscard]] bool isTiffStackPath(const std::filesystem::path& path);

/// Natural order of names: digit runs compare by value.
[[nodiscard]] bool naturalLess(const std::string& a, const std::string& b);

}  // namespace voxelsieve
