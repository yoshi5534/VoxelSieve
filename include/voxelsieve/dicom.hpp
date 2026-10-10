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
#include "voxelsieve/transform.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// DICOM image stacks as volume source (ADR 0019): one file per slice, as CT scanners and
/// VGStudio projects store them, read with DCMTK. Slices are sorted along the slice normal by
/// their image position, else by instance number. Grey values are kept exactly: unsigned samples
/// as they are, signed ones shifted by 32768, and the rescale slope and intercept go into the
/// value mapping (ADR 0015), so that value = intercept + slope * stored sample.

struct DicomStackOptions {
  /// Series to read (Series Instance UID) when the input holds several. Empty: the one with the
  /// most slices.
  std::string series;
  /// Overrides the voxel size of the files.
  std::optional<VoxelSize> voxel_size;
  /// Memory for decoded slices.
  std::size_t cache_bytes = std::size_t{256} << 20U;
};

class DicomStackSource final : public VolumeSource {
 public:
  /// A directory of slice files (not searched recursively) or a single slice file.
  explicit DicomStackSource(const std::filesystem::path& path,
                            const DicomStackOptions& options = {});
  /// The slices of one stack, in any order.
  explicit DicomStackSource(const std::vector<std::filesystem::path>& files,
                            const DicomStackOptions& options = {});
  ~DicomStackSource() override;

  [[nodiscard]] std::array<std::int64_t, 3> dims() const override;
  /// Given, else from the files (pixel spacing and the distance of the slice positions).
  [[nodiscard]] VoxelSize voxelSize() const override;
  /// value = intercept + slope * (grey - 32768) for signed samples, intercept + slope * grey for
  /// unsigned ones.
  [[nodiscard]] ValueMapping valueMapping() const override;
  /// 8 bit for unsigned samples of at most 8 bits stored.
  [[nodiscard]] SampleType sampleType() const override;
  /// True: every slice is a file that is parsed as a whole.
  [[nodiscard]] bool slowRandomAccess() const override { return true; }
  void releaseMemory() const override;
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

  /// Voxel size from the files; nothing when they give no pixel spacing.
  [[nodiscard]] std::optional<VoxelSize> fileVoxelSize() const;
  /// Where the volume lies in the patient (or part) coordinates of the files, from voxel
  /// coordinates (index times pitch, voxel centres at integers) to mm. The identity when the
  /// files give no image position and orientation.
  [[nodiscard]] RigidTransform filePose() const;
  /// The slice files in slice order (z = 0 first).
  [[nodiscard]] const std::vector<std::filesystem::path>& files() const;
  [[nodiscard]] const std::string& seriesUid() const;
  /// Series that were found besides the one read, with their slice counts.
  [[nodiscard]] const std::vector<std::pair<std::string, std::size_t>>& otherSeries() const;
  /// Bits stored per sample and whether samples are signed.
  [[nodiscard]] int bitsStored() const;
  [[nodiscard]] bool isSigned() const;
  /// Files in a directory that were skipped because DCMTK could not read them as an image.
  [[nodiscard]] std::size_t skippedFiles() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Whether `path` looks like a DICOM file: the "DICM" marker after the preamble, or one of the
/// usual extensions (.dcm, .dicom, .dic, .ima) for files written without a preamble.
[[nodiscard]] bool isDicomFile(const std::filesystem::path& path);

/// Whether a directory holds files that look like DICOM slices.
[[nodiscard]] bool containsDicom(const std::filesystem::path& dir);

}  // namespace voxelsieve
