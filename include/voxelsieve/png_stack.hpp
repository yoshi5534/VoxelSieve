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

/// PNG image stacks as volume source (ADR 0042): a directory of slices or a ZIP archive of
/// them, read without extracting it, decoded with libpng. Slices are sorted by name with numbers
/// compared by value, as TIFF stacks (ADR 0011). 8- and 16-bit grey images keep their values;
/// colour images are converted to grey. PNG files give no voxel size that CT software writes
/// reliably, so it comes from the options, else the name of the directory or archive
/// ("..._50um"), else 1 mm.
struct PngStackOptions {
  /// Folder of the slices below the directory or inside the archive. Empty: the only folder with
  /// PNG files; with several, the one with the most slices whose name does not look like labels.
  std::string folder;
  std::optional<VoxelSize> voxel_size;
  /// Memory for decoded slices.
  std::size_t cache_bytes = std::size_t{1} << 30;
};

class PngStackSource final : public VolumeSource {
 public:
  explicit PngStackSource(const std::filesystem::path& path, const PngStackOptions& options = {});
  ~PngStackSource() override;

  [[nodiscard]] std::array<std::int64_t, 3> dims() const override;
  [[nodiscard]] VoxelSize voxelSize() const override;
  /// 8 bit when the first slice is.
  [[nodiscard]] SampleType sampleType() const override;
  /// Always: every slice is decoded as a whole.
  [[nodiscard]] bool slowRandomAccess() const override { return true; }
  void releaseMemory() const override;
  [[nodiscard]] std::string identity() const override;
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

  /// Whether the voxel size came from the options or a name; else 1 mm is assumed.
  [[nodiscard]] bool voxelSizeKnown() const;
  [[nodiscard]] int bitDepth() const;
  /// Folder the slices come from ("" for the top level).
  [[nodiscard]] const std::string& folder() const;
  /// Folders with PNG files that were not chosen, such as label volumes.
  [[nodiscard]] const std::vector<std::string>& otherFolders() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Whether a directory holds PNG slices (at least two), directly or one folder down, or a ZIP
/// archive does. Looks at a bounded number of entries.
[[nodiscard]] bool containsPngSlices(const std::filesystem::path& path);

}  // namespace voxelsieve
