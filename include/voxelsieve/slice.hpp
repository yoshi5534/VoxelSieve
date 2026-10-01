#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// Overlay classes of a slice pixel. Material m of a material volume is kMaterial + m.
enum class SliceOverlay : std::uint8_t { kNone = 0, kPore = 1, kZone = 2, kMaterial = 16 };

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
/// the overlay; a pixel of a coarser level is marked if its footprint holds any. With
/// `materials`, other pixels carry the material of the first level-0 voxel of their footprint.
[[nodiscard]] SliceImage readSlice(const Dataset& dataset, const SliceRequest& request,
                                   const PorosityResult* porosity = nullptr,
                                   const MaterialVolume* materials = nullptr);

/// A plane sampled through a dataset in any orientation (ADR 0018): pixel (x, y) lies at
/// `origin_mm + x * du_mm + y * dv_mm` in the dataset's own coordinates, level-0 voxel index times
/// the pitch per axis. Each pixel takes the nearest voxel of `level`. Other objects are shown in
/// the slice of a volume this way, wherever they lie.
struct PlaneRequest {
  std::array<double, 3> origin_mm{};
  std::array<double, 3> du_mm{};
  std::array<double, 3> dv_mm{};
  std::int64_t width = 256;
  std::int64_t height = 256;
  int level = 0;
  /// Trilinear interpolation between the eight nearest voxels instead of the nearest one, for
  /// planes that do not run along the voxels. Removed air counts as the dataset's air level.
  bool linear = false;
};

inline constexpr std::uint8_t kPlaneOutside = 0;
inline constexpr std::uint8_t kPlaneVoid = 1;
inline constexpr std::uint8_t kPlaneMaterial = 2;

struct PlaneImage {
  std::int64_t width = 0;
  std::int64_t height = 0;
  /// The material threshold of the dataset.
  float threshold = 0.0F;
  /// Grey values, x fastest; the air level where `inside` is 0.
  std::vector<float> grey;
  /// What the pixel hit: kPlaneMaterial for an active voxel at or above the threshold of the
  /// dataset, kPlaneVoid for one below it (air kept around the part, pores), kPlaneOutside for
  /// removed air and places outside the volume.
  std::vector<std::uint8_t> inside;
};

[[nodiscard]] PlaneImage samplePlane(const Dataset& dataset, const PlaneRequest& request);

/// The coarsest level whose voxels are at most `pixel_mm` along their finest axis, so a plane
/// sampled with pixels of that size neither skips voxels nor reads more than it shows.
[[nodiscard]] int levelForPixel(const DatasetInfo& info, double pixel_mm);

/// Where the plane `coordinate[axis] == position` cuts the triangles: one segment per cut
/// triangle as four floats (u0, v0, u1, v1) along the in-plane axes (`sliceAxes`).
[[nodiscard]] std::vector<float> cutMesh(const IndexedMesh& mesh, int axis, double position);
[[nodiscard]] std::vector<float> cutMesh(std::span<const std::array<float, 3>> points,
                                         std::span<const std::array<std::uint32_t, 3>> triangles,
                                         int axis, double position);

/// The whole volume, or a region of it, at one level, as 8-bit values for 3D display in the
/// browser.
struct VolumePreview {
  int level = 0;
  /// First voxel of the region in level voxels; 0 for the whole volume.
  std::array<std::int64_t, 3> origin{};
  std::array<std::int64_t, 3> dims{};
  VoxelSize voxel_size;
  /// Grey values mapped to 0..255 between `low` (0) and `high` (255).
  float low = 0.0F;
  float high = 0.0F;
  std::vector<std::uint8_t> grey;     // x fastest
  std::vector<std::uint8_t> overlay;  // SliceOverlay per voxel
};

struct VolumeRequest {
  /// Largest number of voxels per axis.
  std::int64_t max_size = 256;
  /// Level-0 voxels to read; the whole volume when unset. The 3D view reads the part near the
  /// camera this way at a finer level than the whole volume.
  std::optional<Box> region;
  /// Grey values mapped to 0 and 255; the 0.5 and 99.5 percentiles when unset. A region takes
  /// the window of the whole volume so both show the same values alike.
  std::optional<std::array<float, 2>> window;
};

/// Reads the finest level at which the region (by default the volume) is at most
/// `request.max_size` voxels along every axis (the coarsest level if none is that small).
[[nodiscard]] VolumePreview readVolumePreview(const Dataset& dataset, const VolumeRequest& request,
                                              const PorosityResult* porosity = nullptr,
                                              const MaterialVolume* materials = nullptr);
[[nodiscard]] VolumePreview readVolumePreview(const Dataset& dataset, std::int64_t max_size,
                                              const PorosityResult* porosity = nullptr,
                                              const MaterialVolume* materials = nullptr);

}  // namespace voxelsieve
