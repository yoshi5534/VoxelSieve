#pragma once

// Builds the import preview (ADR 0020) from whole slices, shared by readImportPreview and the
// streaming sieve, which hands in the slices it copies anyway.

#include <tbb/combinable.h>

#include <array>
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

  /// The slices to hand in, ascending.
  [[nodiscard]] const std::vector<std::int64_t>& slices() const { return preview_.slices; }

  /// Adds slice `slices()[k]`: dims[0] * dims[1] grey values, x fastest. May be called from
  /// several threads at once for different `k`.
  void add(std::size_t k, const std::uint16_t* slice);

  /// The preview, once every slice was added.
  [[nodiscard]] ImportPreview finish();

 private:
  ImportPreview preview_;
  tbb::combinable<Histogram> histograms_;
};

}  // namespace voxelsieve::detail
