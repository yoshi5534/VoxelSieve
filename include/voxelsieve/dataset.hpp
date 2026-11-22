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
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "voxelsieve/preview.hpp"
#include "voxelsieve/progress.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/vdb.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// Bricked, multi-resolution dataset on disk, see docs/adr/0004-out-of-core-bricked-dataset.md.
///
///   <dir>/index.json              metadata and the list of bricks per level
///   <dir>/overview.vdb            whole volume in one grid (coarsest level)
///   <dir>/level<L>/<x>_<y>_<z>.vdb one grid "density" per brick
///
/// Level-0 voxel (i, j, k) of the scan has VDB index (i, j, k) in every level-0 brick. A level-L
/// voxel covers 2^L level-0 voxels per axis; its value is the mean of its active children,
/// rounded to the nearest integer in datasets of integer grey values. All grids of a dataset hold
/// one value type (ADR 0021): the input's (`uint16`, or `uint8` for 8-bit input), or `float`.

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
  /// first copied slice by slice to a temporary raw file in the input's width (1 or 2 bytes per
  /// voxel), so that every slice is decoded once instead of once per brick. The copy is written
  /// with plain writes, as many slices at once as the work memory holds; with
  /// `min_material_voxels` 1, pass 1 (histogram, block maxima) is taken while the slices pass, so
  /// the copy is read once, by pass 2. Off: they are read directly.
  bool stage_slow_sources = true;
  /// Directory of that temporary file; empty: the output directory. It is removed at the end.
  std::filesystem::path staging_dir;
  /// Type of the stored grey values; unset: the input's sample width (`VolumeSource::sampleType`),
  /// so the grids hold exactly the input's values (ADR 0021). `kFloat` writes float grids for
  /// viewers such as Blender and Houdini, which do not know VoxelSieve's integer grids. `kUInt8`
  /// requires 8-bit input: grey values are never quantised.
  std::optional<ValueType> value_type;
  /// Called before the passes with a first look at the input (ADR 0020): a few whole slices
  /// spread over the volume (`preview_options`), their histogram and a threshold estimate. A
  /// staged source copies those slices first, then the other slices of the preview coarse to fine
  /// with a sharper preview after each round, then the rest, with a last call once the histogram
  /// holds every slice; every slice is read once. Other sources get only the first look, read
  /// directly, which is cheap for them. Called from the thread of writeDataset.
  std::function<void(const ImportPreview& preview)> preview;
  PreviewOptions preview_options;
  /// Called as the sieve advances, with the stage ("staging": copying a slow source, "histogram":
  /// pass 1 over the input, "bricks": pass 2, "levels": the coarser levels) and the fraction of
  /// that stage done, 0 to 1.
  /// Called from one thread at a time, at most once per whole percent of a stage.
  std::function<void(std::string_view stage, double fraction)> progress;
  /// An import that was interrupted leaves a journal in the output directory (import.json, the
  /// result of pass 1 and the finished bricks). Written again into that directory with the same
  /// input size and options, it resumes: pass 1 and the finished bricks are skipped (a slow
  /// source is staged again). Off, the directory must be empty. The journal of another input (a
  /// file with another path, size or time of change) or of other options is an error, unless
  /// `replace_other_import` is set: then it is removed and the import starts again, for callers
  /// that own the directory, such as the step of a project.
  bool resume = true;
  bool replace_other_import = false;
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
  /// Type of the grey values in every grid; datasets written before ADR 0021 hold float.
  ValueType value_type = ValueType::kFloat;
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

class Dataset;

/// Computes the new level-0 grey values of one brick of a derived dataset into `values`, which has
/// one element per voxel of `box`, the brick's box, x fastest. Only the voxels that are active in
/// the input brick are kept; a NaN value removes the voxel as well (it becomes outside air), and a
/// brick without any voxel left is not written.
using BrickValues = std::function<void(const std::array<std::int64_t, 3>& brick, const Box& box,
                                       std::vector<float>& values)>;

struct DerivedDatasetOptions {
  /// Type of the stored grey values; unset: the input's. Integer types store the computed values
  /// rounded to the nearest integer and clamped to the type's range, like the coarser levels.
  /// `kUInt8` requires 8-bit input.
  std::optional<ValueType> value_type;
  /// How the stored values relate to what they measure (value = offset + scale × stored), for
  /// derived values in other units, such as a thickness in mm stored in 16 bit; `values` computes
  /// the stored values. Unset: the input's.
  std::optional<ValueMapping> value_mapping;
  /// Called with the stage ("bricks", then "levels") and the fraction of it done, 0 to 1, from
  /// one thread at a time.
  std::function<void(std::string_view stage, double fraction)> progress;
  /// Names the processing and its parameters, for resuming (ADR 0034). Set, the run keeps a
  /// journal in `dir`, and a run that was interrupted resumes when it is started again into the
  /// same directory with the same input, value type and key: the bricks it finished are kept,
  /// `values` is not called for them, but `finished` is. Empty: `dir` must not exist or be empty.
  std::string resume_key;
  /// Called for each brick that a resumed run kept from before, with its box, before the other
  /// bricks; processing that sums up its values reads them from the brick written before.
  std::function<void(const std::array<std::int64_t, 3>& brick, const Box& box)> finished;
};

/// Writes a dataset with the bricks, active voxels and metadata of `input` and level-0 grey values
/// from `values`, for processing that keeps the part but changes its grey values, such as
/// denoising (ADR 0025). `values` is called for one brick after the other, with zeroed values
/// (it may work in parallel inside); building and writing the bricks runs in parallel behind it.
/// The coarser levels are then built from the new values. `dir` must not exist or be empty,
/// unless it holds an interrupted run to resume (`resume_key`). Memory use is bounded by a few
/// bricks, independent of the volume size.
DatasetInfo writeDerivedDataset(const Dataset& input, const std::filesystem::path& dir,
                                const BrickValues& values,
                                const DerivedDatasetOptions& options = {});

[[nodiscard]] DatasetInfo readDatasetInfo(const std::filesystem::path& dir);

[[nodiscard]] std::filesystem::path brickPath(const std::filesystem::path& dir, int level,
                                              const std::array<std::int64_t, 3>& brick);

/// Opens one brick, of any value type. With `delay_load`, voxel values are read from the
/// memory-mapped file on first access.
[[nodiscard]] GreyGrid readBrick(const std::filesystem::path& file, bool delay_load = true);

/// Counters of the brick cache of a `Dataset`.
struct CacheStats {
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::size_t bricks = 0;  // bricks currently cached
  std::size_t bytes = 0;   // memory of the cached bricks
  std::size_t pinned = 0;  // cached bricks that callers hold (pinned), which are not evicted
  std::size_t pinned_bytes = 0;
};

/// How the brick cache loads bricks.
enum class BrickLoading : std::uint8_t {
  /// The whole brick is read; best when an algorithm visits most of its voxels.
  kFull,
  /// Only the topology is read at first and the voxel values of an 8^3 leaf on first access, from
  /// the memory-mapped file; best for sparse access such as slices. The cache charges such a brick
  /// the memory of all its leaves when it loads it, so the budget holds however many are read.
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
  /// Changes the budget; bricks over a smaller one are evicted at once, except the most recently
  /// used.
  void setBudget(std::size_t budget_bytes);
  /// Bricks and bytes cached for all datasets; hits and misses are counted per dataset.
  [[nodiscard]] CacheStats stats() const;

 private:
  friend class Dataset;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// How `Dataset::forEachBrick` works through the bricks (ADR 0034).
struct BrickSweep {
  /// Voxels around its brick that a call reads as well (through `readRegion`, `sample` or a
  /// `Sampler`). The bricks they fall into are pinned during the call, so neighbouring calls
  /// find them in the cache instead of reading them again.
  std::int64_t halo = 0;
  /// Memory one call needs besides the bricks. It sets how many calls run at once
  /// (`tasksInMemory`); 0: one per thread.
  std::uint64_t task_bytes = 0;
  /// Checked before each brick, which counts as a step when it is done (review step 5.1).
  ProgressToken progress;
};

/// Read access to a dataset written by `writeDataset`, for volumes much larger than memory.
///
/// Opening reads only index.json. Bricks are loaded on demand into an LRU cache with a memory
/// budget; the most recently used brick is always kept, even if it alone exceeds the budget.
/// A returned brick is pinned while the caller holds it (ADR 0034): it stays in the cache and
/// counts against the budget; only bricks that no caller holds are evicted. Datasets opened
/// without a cache of their own share one cache (`sharedCache`), so the datasets an operation
/// opens add no budgets.
///
/// Voxel coordinates are per level: level-L voxel (i, j, k) covers level-0 voxels
/// [i * 2^L, (i + 1) * 2^L) on each axis. All methods may be called from several threads.
class Dataset {
 public:
  using BrickPtr = std::shared_ptr<const GreyGrid>;
  using BrickFunction =
      std::function<void(const std::array<std::int64_t, 3>& brick, const GreyGrid& grid)>;

  /// Budget of the shared cache unless set: a third of the process's memory budget
  /// (`memoryBudgetBytes`, ADR 0034), at least 2 GiB but at most half of it. Halo reads of T
  /// threads need T × 27 bricks at once, which a fixed 1 GiB could not hold.
  [[nodiscard]] static std::size_t defaultCacheBytes();
  /// The cache of the datasets opened without one. It lives while a dataset or a caller holds it;
  /// the next one starts empty.
  [[nodiscard]] static std::shared_ptr<BrickCache> sharedCache();
  /// Sets the budget of the shared cache, now and for later ones; 0 restores the default.
  static void setSharedCacheBytes(std::size_t bytes);

  /// Opens a dataset whose bricks go into the shared cache.
  [[nodiscard]] static Dataset open(const std::filesystem::path& dir,
                                    BrickLoading loading = BrickLoading::kFull);
  /// Opens a dataset with a cache of its own.
  [[nodiscard]] static Dataset open(const std::filesystem::path& dir, std::size_t cache_bytes,
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

  /// Reads single voxels of one level like `sample`, but keeps the brick of the last voxel and an
  /// accessor to it, so that neighbouring voxels skip the cache lookup, which otherwise costs
  /// more than reading the voxel. For one thread at a time; it holds that brick until it moves to
  /// another one. The dataset must outlive it.
  class Sampler {
   public:
    Sampler(const Dataset& dataset, int level);

    /// Grey value of an active voxel; nullopt for removed air and positions outside the volume.
    [[nodiscard]] std::optional<float> operator()(const std::array<std::int64_t, 3>& voxel);

   private:
    const Dataset* dataset_;
    int level_;
    std::array<std::int64_t, 3> dims_;
    std::int64_t brick_size_;
    std::array<std::int64_t, 3> current_{-1, -1, -1};
    BrickPtr brick_;
    std::variant<std::monostate, openvdb::FloatGrid::ConstAccessor, UInt16Grid::ConstAccessor,
                 UInt8Grid::ConstAccessor>
        accessor_;
  };

  /// Copies the voxels of `box` into `out`, x fastest, writing `fill` where no voxel is active.
  /// `box` must lie inside the level and `out.size()` must equal `box.voxelCount()`. Copies leaf
  /// by leaf. The `uint16_t` and `uint8_t` overloads take half or a quarter of the memory of
  /// `float` for algorithms that only compare grey values; they need a dataset of that type or a
  /// narrower one (`uint8` into `uint16`) and throw std::invalid_argument otherwise.
  void readRegion(int level, const Box& box, std::span<float> out, float fill = 0.0F) const;
  void readRegion(int level, const Box& box, std::span<std::uint16_t> out,
                  std::uint16_t fill = 0) const;
  void readRegion(int level, const Box& box, std::span<std::uint8_t> out,
                  std::uint8_t fill = 0) const;

  /// Calls `function` for every stored brick of `level`, in parallel: the bricks along a Morton
  /// curve, so that the bricks in use are a small window that moves through the volume, and
  /// at most as many calls at once as `sweep` allows. Each call runs isolated: while it waits for
  /// its own parallel work, its thread starts no other brick.
  void forEachBrick(int level, const BrickFunction& function, const BrickSweep& sweep) const;
  void forEachBrick(int level, const BrickFunction& function) const;

  /// Calls `function(brick, grid)` for every stored brick of `level` like `forEachBrick`, with
  /// the typed grid (`openvdb::FloatGrid`, `UInt16Grid` or `UInt8Grid`): `function` is a generic
  /// lambda.
  template <class Function>
  void forEachGrid(int level, Function&& function, const BrickSweep& sweep = {}) const {
    forEachBrick(
        level,
        [&](const std::array<std::int64_t, 3>& brick, const GreyGrid& grid) {
          grid.visit([&](const auto& typed) { function(brick, typed); });
        },
        sweep);
  }

  /// Hits and misses of this dataset; bricks and bytes of its cache, which may be shared.
  [[nodiscard]] CacheStats cacheStats() const;
  [[nodiscard]] const std::shared_ptr<BrickCache>& cache() const;

 private:
  struct Impl;
  explicit Dataset(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace voxelsieve
