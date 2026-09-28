#pragma once

#include <openvdb/openvdb.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// Surface of the part as a voxel mask with a few bits per voxel that encode the signed distance
/// to the surface, see docs/adr/0009-surface-distance-mask.md.
///
/// Every voxel gets a code of `bits` bits. Code 0 means material farther than `band_voxels` from
/// the surface, the highest code air farther than the band. The codes in between quantise the
/// signed distance inside the band uniformly, negative (material) below the middle and positive
/// (air) above it, so the sign of every voxel is exact. The file stores only 8^3 blocks that
/// contain codes other than 0 or the highest one; everything else costs two bits per block, or
/// nothing for chunks without surface. Chunks are compressed with zstd.

struct SurfaceOptions {
  /// Bits per voxel, 2 to 8.
  int bits = 4;
  /// Half width of the band with distance values, in voxels. Must not exceed the dataset margin.
  double band_voxels = 1.0;
  /// Grey value of the surface; default half way between air and material (ISO 50 %).
  std::optional<float> iso_value;
  /// zstd level 1 to 22; 0 picks 19, which is about 20 % smaller than the fast levels.
  int compression_level = 0;
};

struct SurfaceInfo {
  std::array<std::int64_t, 3> dims{};
  VoxelSize voxel_size;
  int bits = 0;
  double band_voxels = 0.0;
  std::int64_t block_size = 8;
  std::int64_t chunk_size = 0;
  float iso_value = 0.0F;
  float air_level = 0.0F;
  float material_level = 0.0F;

  std::int64_t chunks = 0;
  std::int64_t surface_chunks = 0;    // chunks with at least one surface block
  std::int64_t surface_blocks = 0;    // 8^3 blocks with stored codes
  std::int64_t band_voxel_count = 0;  // voxels with a code strictly inside the band
  /// Material volume from the codes: each voxel counts with clamp(0.5 - d, 0, 1).
  double volume_mm3 = 0.0;
  /// Payload before compression and file size.
  std::uint64_t raw_bytes = 0;
  std::uint64_t file_bytes = 0;

  [[nodiscard]] int maxCode() const { return (1 << bits) - 1; }
  /// Width of one distance step in voxels.
  [[nodiscard]] double stepVoxels() const { return 2.0 * band_voxels / (maxCode() - 1); }
};

[[nodiscard]] nlohmann::json toJson(const SurfaceInfo& info);

/// Quantises a signed distance in voxels. `inside` decides the sign, so a voxel on the surface
/// keeps the side its grey value puts it on.
[[nodiscard]] std::uint8_t encodeSurfaceDistance(double distance_voxels, bool inside, int bits,
                                                 double band_voxels);
/// Signed distance in voxels that a code stands for; the far codes return -band and +band.
[[nodiscard]] float decodeSurfaceDistance(std::uint8_t code, int bits, double band_voxels);

/// Locates the surface of the part in a sieved dataset and writes the distance mask to `file`.
/// Streams the dataset brick by brick (chunk = dataset brick), so memory stays bounded by a few
/// bricks. The surface is the iso-surface of the grey values; distances
/// inside the band are exact Euclidean distances to its triangulation.
SurfaceInfo writeSurface(const Dataset& dataset, const std::filesystem::path& file,
                         const SurfaceOptions& options = {});

[[nodiscard]] SurfaceInfo readSurfaceInfo(const std::filesystem::path& file);

/// Read access to a surface file. Chunks are decoded on demand into an LRU cache. All methods
/// may be called from several threads.
class SurfaceMask {
 public:
  static constexpr std::size_t kDefaultCacheBytes = std::size_t{256} << 20U;

  [[nodiscard]] static SurfaceMask open(const std::filesystem::path& file,
                                        std::size_t cache_bytes = kDefaultCacheBytes);

  SurfaceMask(SurfaceMask&&) noexcept;
  SurfaceMask& operator=(SurfaceMask&&) noexcept;
  SurfaceMask(const SurfaceMask&) = delete;
  SurfaceMask& operator=(const SurfaceMask&) = delete;
  ~SurfaceMask();

  [[nodiscard]] const SurfaceInfo& info() const;

  /// Code of a level-0 voxel; voxels outside the volume are air.
  [[nodiscard]] std::uint8_t code(const std::array<std::int64_t, 3>& voxel) const;
  /// Signed distance of a voxel to the surface in voxels, negative in material, within +-band.
  [[nodiscard]] float distance(const std::array<std::int64_t, 3>& voxel) const;
  /// Codes of `box`, x fastest; `out.size()` must equal `box.voxelCount()`.
  void readCodes(const Box& box, std::span<std::uint8_t> out) const;

  /// Narrow-band level set in the dataset's world space (voxel index times voxel size) with the
  /// decoded distances in mm, for rendering, meshing or export.
  [[nodiscard]] openvdb::FloatGrid::Ptr toLevelSet() const;
  /// Triangle mesh of the zero crossing in mm, dataset world space, outward oriented.
  [[nodiscard]] Mesh toMesh(double adaptivity = 0.0) const;

 private:
  struct Impl;
  explicit SurfaceMask(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

/// Triangle mesh with shared vertices.
struct IndexedMesh {
  std::vector<std::array<float, 3>> points;
  std::vector<std::array<std::uint32_t, 3>> triangles;
};

/// Mesh of the surface for display, in level-0 voxel coordinates (voxel index, not mm). With
/// `adaptivity` > 0, flat regions are meshed with larger triangles (see volumeToMesh); when the
/// mesh still has more than `max_triangles`, the surface is resampled at 2, 4, ... times the voxel
/// size until it fits.
[[nodiscard]] IndexedMesh surfaceDisplayMesh(const SurfaceMask& mask, std::size_t max_triangles,
                                             double adaptivity = 0.25);

/// Writes the middle slice along x, y and z as surface_[xyz].png: material grey, air black and the
/// band coloured from blue (in material) to red (in air). Slices larger than `max_pixels` are
/// sampled down.
void writeSurfaceImages(const SurfaceMask& mask, const std::filesystem::path& dir,
                        std::int64_t max_pixels = 1024);

}  // namespace voxelsieve
