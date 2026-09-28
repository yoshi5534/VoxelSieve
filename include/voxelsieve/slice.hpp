#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// Overlay classes of a slice pixel.
enum class SliceOverlay : std::uint8_t { kNone = 0, kPore = 1, kZone = 2 };

/// A rectangle of a slice through a dataset level (ADR 0008). The slice is normal to `axis`; its
/// pixels run along the in-plane axes u and v (`sliceAxes`), u fastest.
struct SliceRequest {
  int axis = 2;
  /// Slice position in level-0 voxels along `axis`.
  std::int64_t index = 0;
  int level = 0;
  /// First pixel and size in level voxels along u and v. Pixels outside the volume show air.
  std::array<std::int64_t, 2> origin{0, 0};
  std::array<std::int64_t, 2> size{256, 256};
};

struct SliceImage {
  std::int64_t width = 0;
  std::int64_t height = 0;
  /// Grey values, u fastest; removed air and pixels outside the volume hold the air level.
  std::vector<float> grey;
  /// SliceOverlay per pixel: pores and loosened zones within the pixel's footprint.
  std::vector<std::uint8_t> overlay;
};

/// In-plane axes (u, v) of a slice normal to `axis`: z -> (x, y), y -> (x, z), x -> (y, z).
[[nodiscard]] std::array<int, 2> sliceAxes(int axis);

/// Reads a slice rectangle. Only the bricks it touches are loaded, so the cost depends on the
/// rectangle and level, not on the dataset size. With `porosity`, pores and zones are marked in
/// the overlay; a pixel of a coarser level is marked if its footprint holds any.
[[nodiscard]] SliceImage readSlice(const Dataset& dataset, const SliceRequest& request,
                                   const PorosityResult* porosity = nullptr);

/// The whole volume at one level, as 8-bit values for 3D display in the browser.
struct VolumePreview {
  int level = 0;
  std::array<std::int64_t, 3> dims{};
  VoxelSize voxel_size;
  /// Grey values mapped to 0..255 between `low` (0) and `high` (255).
  float low = 0.0F;
  float high = 0.0F;
  std::vector<std::uint8_t> grey;     // x fastest
  std::vector<std::uint8_t> overlay;  // SliceOverlay per voxel
};

/// Reads the finest level whose largest dimension is at most `max_size` (the coarsest level if
/// none is that small). The window spans the 0.5 to 99.5 percentiles of the grey values.
[[nodiscard]] VolumePreview readVolumePreview(const Dataset& dataset, std::int64_t max_size,
                                              const PorosityResult* porosity = nullptr);

}  // namespace voxelsieve
