#pragma once

// Builds the import preview (ADR 0020) from whole slices, shared by readImportPreview and the
// streaming sieve, which hands in the slices it copies anyway.

#include <tbb/combinable.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "detail/blocks.hpp"
#include "voxelsieve/preview.hpp"

namespace voxelsieve::detail {

class PreviewBuilder {
 public:
  PreviewBuilder(const std::array<std::int64_t, 3>& dims, const VoxelSize& voxel_size,
                 const ValueMapping& mapping, const PreviewOptions& options);

  /// The slice of the input that each slice of the preview shows once it is read, ascending.
  [[nodiscard]] const std::vector<std::int64_t>& targets() const { return targets_; }

  /// The preview slices to read round by round, coarse to fine: the first look (about
  /// `PreviewOptions::slices`), then three times as many in each round, until all. Each round
  /// lists only the indices into `targets()` that earlier rounds did not read.
  [[nodiscard]] std::vector<std::vector<std::size_t>> rounds() const;

  /// Adds slice `targets()[k]`: dims[0] * dims[1] grey values, x fastest. May be called from
  /// several threads at once for different `k`.
  void add(std::size_t k, const std::uint16_t* slice);

  /// Counts a slice that no preview slice shows in the histogram only. Thread-safe.
  void count(const std::uint16_t* slice);

  /// Slices added or counted so far.
  [[nodiscard]] std::int64_t slicesRead() const { return slices_read_.load(); }

  /// The preview of the slices added so far. Not while slices are added.
  [[nodiscard]] ImportPreview snapshot();

 private:
  ImportPreview preview_;
  std::vector<std::int64_t> targets_;
  // One flag per preview slice; each is written by the one task that adds its slice.
  std::vector<std::uint8_t> added_;
  std::int64_t first_look_ = 0;
  std::atomic<std::int64_t> slices_read_{0};
  tbb::combinable<Histogram> histograms_;
};

}  // namespace voxelsieve::detail
