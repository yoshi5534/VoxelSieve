#pragma once

#include <openvdb/openvdb.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "voxelsieve/preview.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// Bricked, multi-resolution dataset on disk, see docs/adr/0004-out-of-core-bricked-dataset.md.
///
///   <dir>/index.json              metadata and the list of bricks per level
///   <dir>/overview.vdb            whole volume in one grid (coarsest level)
///   <dir>/level<L>/<x>_<y>_<z>.vdb one FloatGrid "density" per brick
///
/// Level-0 voxel (i, j, k) of the scan has VDB index (i, j, k) in every level-0 brick. A level-L
/// voxel covers 2^L level-0 voxels per axis; its value is the mean of its active children.

struct DatasetOptions {
  /// Air/material grey value; estimated from the full histogram when unset (the valley after
  /// the air peak, else Otsu's split).
  std::optional<float> threshold;
  /// Voxels of air kept around the part.
  int margin_voxels = 3;
  /// Brick edge length in voxels, a multiple of 8. The overview has at most this size.
  std::int64_t brick_size = 256;
  /// A block counts as material once it holds at least this many voxels above the threshold,
  /// 1 to 512. Values above 1 keep isolated noise spikes in the air from counting as material.
  int min_material_voxels = 1;
  /// Where outside air enters (see SieveOptions::outside_air_axes).
  AirAxes outside_air_axes = kAllAxes;
  /// Sources with slow random access (`VolumeSource::slowRandomAccess`, such as TIFF stacks) are
  /// first copied slice by slice to a temporary raw file of 2 bytes per voxel, so that every
  /// slice is decoded once instead of once per brick. Off: they are read directly.
  bool stage_slow_sources = true;
  /// Directory of that temporary file; empty: the output directory. It is removed at the end.
  std::filesystem::path staging_dir;
  /// Called once before the passes with a first look at the input (ADR 0020): a few whole slices
  /// spread over the volume (`preview_options`), their histogram and a threshold estimate. A
  /// staged source copies those slices first and the rest after the call, so every slice is read
  /// once; other sources are read directly, which is cheap for them.
  std::function<void(const ImportPreview& preview)> preview;
  PreviewOptions preview_options;
  /// Called as the sieve advances, with the stage ("staging": copying a slow source, "histogram":
  /// pass 1 over the input, "bricks": pass 2, "levels": the coarser levels) and the fraction of
  /// that stage done, 0 to 1.
  /// Called from one thread at a time, at most once per whole percent of a stage.
  std::function<void(std::string_view stage, double fraction)> progress;
};

struct LevelInfo {
  int level = 0;
  std::array<std::int64_t, 3> dims{};
  VoxelSize voxel_size;
  std::vector<std::array<std::int64_t, 3>> bricks;
};

struct DatasetInfo {
  std::array<std::int64_t, 3> dims{};
  VoxelSize voxel_size;
  std::int64_t brick_size = 0;
  float threshold = 0.0F;
  float air_level = 0.0F;
  int margin_voxels = 0;
  int min_material_voxels = 1;
  AirAxes outside_air_axes = kAllAxes;
  std::int64_t active_voxel_count = 0;
  /// How the grey values relate to the values of the scan (float scans, ADR 0015).
  ValueMapping value_mapping;
  std::vector<LevelInfo> levels;
};

/// Sieves `source` in two streaming passes and writes the dataset to `dir`, which must not exist
/// or be empty. Memory use is bounded by the per-block maxima (2 bytes per 8^3 block) and a few
/// bricks per thread, independent of the volume size.
DatasetInfo writeDataset(const VolumeSource& source, const std::filesystem::path& dir,
                         const DatasetOptions& options = {});

[[nodiscard]] DatasetInfo readDatasetInfo(const std::filesystem::path& dir);

[[nodiscard]] std::filesystem::path brickPath(const std::filesystem::path& dir, int level,
                                              const std::array<std::int64_t, 3>& brick);

/// Opens one brick. With `delay_load`, voxel values are read from the memory-mapped file on first
/// access.
[[nodiscard]] openvdb::FloatGrid::Ptr readBrick(const std::filesystem::path& file,
                                                bool delay_load = true);

/// Counters of the brick cache of a `Dataset`.
struct CacheStats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::size_t bricks = 0;  // bricks currently cached
  std::size_t bytes = 0;   // memory of the cached bricks
};

/// How the brick cache loads bricks.
enum class BrickLoading : std::uint8_t {
  /// The whole brick is read; best when an algorithm visits most of its voxels.
  kFull,
  /// Only the topology is read at first and the voxel values of an 8^3 leaf on first access, from
  /// the memory-mapped file; best for sparse access such as slices. The cache re-measures the
  /// memory of its bricks whenever it loads one, so the budget still holds.
  kOnAccess,
};

/// A brick cache with one memory budget (LRU), which several datasets can share: opening another
/// volume of a project then does not add another budget (ADR 0018). Each dataset takes its
/// bricks out again when it is destroyed.
class BrickCache {
 public:
  explicit BrickCache(std::size_t budget_bytes);
  BrickCache(const BrickCache&) = delete;
  BrickCache& operator=(const BrickCache&) = delete;
  BrickCache(BrickCache&&) = delete;
  BrickCache& operator=(BrickCache&&) = delete;
  ~BrickCache();

  [[nodiscard]] std::size_t budget() const;
  /// Bricks and bytes cached for all datasets; hits and misses are counted per dataset.
  [[nodiscard]] CacheStats stats() const;

 private:
  friend class Dataset;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Read access to a dataset written by `writeDataset`, for volumes much larger than memory.
///
/// Opening reads only index.json. Bricks are loaded on demand into an LRU cache whose memory is
/// bounded by `cache_bytes`; the most recently used brick is always kept, even if it alone exceeds
/// the budget. Returned bricks stay valid while the caller holds them, also after eviction.
///
/// Voxel coordinates are per level: level-L voxel (i, j, k) covers level-0 voxels
/// [i * 2^L, (i + 1) * 2^L) on each axis. All methods may be called from several threads.
class Dataset {
 public:
  using BrickPtr = std::shared_ptr<const openvdb::FloatGrid>;
  using BrickFunction =
      std::function<void(const std::array<std::int64_t, 3>& brick, const openvdb::FloatGrid& grid)>;

  static constexpr std::size_t kDefaultCacheBytes = std::size_t{1} << 30U;

  [[nodiscard]] static Dataset open(const std::filesystem::path& dir,
                                    std::size_t cache_bytes = kDefaultCacheBytes,
                                    BrickLoading loading = BrickLoading::kFull);
  /// Opens a dataset whose bricks go into a cache shared with other datasets.
  [[nodiscard]] static Dataset open(const std::filesystem::path& dir,
                                    std::shared_ptr<BrickCache> cache,
                                    BrickLoading loading = BrickLoading::kFull);

  Dataset(Dataset&&) noexcept;
  Dataset& operator=(Dataset&&) noexcept;
  Dataset(const Dataset&) = delete;
  Dataset& operator=(const Dataset&) = delete;
  ~Dataset();

  [[nodiscard]] const DatasetInfo& info() const;
  [[nodiscard]] const std::filesystem::path& dir() const;
  [[nodiscard]] const LevelInfo& level(int level) const;

  /// Voxel box of a brick at `level`, clipped to the level's dimensions.
  [[nodiscard]] Box brickBox(int level, const std::array<std::int64_t, 3>& brick) const;

  /// False for bricks that were not written because they hold only outside air.
  [[nodiscard]] bool hasBrick(int level, const std::array<std::int64_t, 3>& brick) const;

  /// The brick, loaded through the cache, or nullptr when it was not written.
  [[nodiscard]] BrickPtr brick(int level, const std::array<std::int64_t, 3>& brick) const;

  /// Grey value of an active voxel; nullopt for removed air and positions outside the volume.
  [[nodiscard]] std::optional<float> sample(int level,
                                            const std::array<std::int64_t, 3>& voxel) const;

  /// Copies the voxels of `box` into `out`, x fastest, writing `fill` where no voxel is active.
  /// `box` must lie inside the level and `out.size()` must equal `box.voxelCount()`.
  void readRegion(int level, const Box& box, std::span<float> out, float fill = 0.0F) const;

  /// Calls `function` for every stored brick of `level`, in parallel.
  void forEachBrick(int level, const BrickFunction& function) const;

  /// Hits and misses of this dataset; bricks and bytes of its cache, which may be shared.
  [[nodiscard]] CacheStats cacheStats() const;
  [[nodiscard]] const std::shared_ptr<BrickCache>& cache() const;

 private:
  struct Impl;
  explicit Dataset(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace voxelsieve
