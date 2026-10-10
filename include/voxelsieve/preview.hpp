#pragma once

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <vector>

#include "voxelsieve/source.hpp"
#include "voxelsieve/volume.hpp"

namespace voxelsieve {

/// A first look at a volume before it is imported (ADR 0020): a few whole slices spread evenly
/// over the volume, each averaged in-plane. Whole slices because a slice is what every input
/// stores together (a file of a stack, a contiguous part of a raw file), so reading them costs
/// one request each, also on a network share; the import reads them only once (writeDataset).

struct PreviewOptions {
  /// Slices to read, spread evenly along z; all slices when the volume has fewer.
  std::int64_t slices = 64;
  /// Largest edge of the preview within a slice, in voxels: slices are averaged over square
  /// blocks of voxels to at most this size.
  std::int64_t size = 256;
};

struct ImportPreview {
  /// The slices read, averaged in-plane over `pixel_stride` x `pixel_stride` voxels; slice k of
  /// the preview is slice `slices[k]` of the input. The voxel size is that of the preview voxels
  /// (the slice spacing is the mean spacing of the slices read).
  Volume16 volume;
  std::int64_t pixel_stride = 1;
  std::vector<std::int64_t> slices;
  /// Dimensions of the whole input.
  std::array<std::int64_t, 3> source_dims{};
  /// Grey-value histogram (65536 bins) of every voxel of the slices read, at full resolution.
  std::vector<std::uint64_t> histogram;
  /// Air/material threshold and air level estimated from `histogram`, as the import estimates
  /// them from the whole volume.
  float threshold = 0.0F;
  float air_level = 0.0F;
  /// Grey values at 0.5 % and 99.5 % of the histogram, for display.
  std::array<float, 2> window{0.0F, 0.0F};
  ValueMapping value_mapping;

  /// Share of the input's voxels that were read.
  [[nodiscard]] double fractionRead() const;
};

/// Reads the preview of `source` directly. writeDataset makes the same preview while it imports,
/// without reading anything twice (DatasetOptions::preview).
[[nodiscard]] ImportPreview readImportPreview(const VolumeSource& source,
                                              const PreviewOptions& options = {});

/// The slices a preview of a volume `depth` slices deep reads: the middle slice of each of
/// `count` equal parts, in ascending order.
[[nodiscard]] std::vector<std::int64_t> previewSlices(std::int64_t depth, std::int64_t count);

/// A picture of the preview: the central sections normal to z, y and x side by side in true
/// proportions (ADR 0012), grey values between the window, and below them the histogram on a
/// logarithmic scale with the estimated threshold in red. RGB, rows top to bottom.
struct PreviewImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgb;
};
[[nodiscard]] PreviewImage renderPreview(const ImportPreview& preview);
/// renderPreview encoded as PNG.
[[nodiscard]] std::vector<std::uint8_t> previewPng(const ImportPreview& preview);

/// Summary for the protocol and the studio: what was read, the threshold estimate, the window
/// and the histogram in `bins` bins between the lowest and highest grey value that occurs.
[[nodiscard]] nlohmann::json previewSummary(const ImportPreview& preview, int bins = 128);

}  // namespace voxelsieve
