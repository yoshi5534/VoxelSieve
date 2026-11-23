#include "voxelsieve/dataset.hpp"

#include <openvdb/io/File.h>
#include <openvdb/io/Stream.h>
#include <openvdb/tools/Composite.h>
#include <openvdb/tools/Morphology.h>
#include <openvdb/tools/Prune.h>
#include <openvdb/tree/LeafManager.h>
#include <tbb/blocked_range.h>
#include <tbb/combinable.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_pipeline.h>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>

#include <algorithm>
#include <atomic>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/iostreams/stream.hpp>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "detail/blocks.hpp"
#include "detail/pages.hpp"
#include "detail/preview.hpp"
#include "detail/sweep.hpp"
#include "detail/transform.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/resources.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/telemetry.hpp"
#include "voxelsieve/uninitialized.hpp"

namespace voxelsieve {
namespace {

using detail::BlockGrid;
using detail::BlockState;
using detail::ceilDiv;
using detail::Histogram;
using detail::kHistogramBins;
using Index3 = std::array<std::int64_t, 3>;

/// 2: grids of any value type (ADR 0021); 1: float grids only, still read.
constexpr int kFormatVersion = 2;

openvdb::Coord toCoord(const Index3& index) {
  return {static_cast<openvdb::Int32>(index[0]), static_cast<openvdb::Int32>(index[1]),
          static_cast<openvdb::Int32>(index[2])};
}

Index3 ceilDiv3(const Index3& a, std::int64_t b) {
  return {ceilDiv(a[0], b), ceilDiv(a[1], b), ceilDiv(a[2], b)};
}

std::size_t product(const Index3& dims) {
  return static_cast<std::size_t>(dims[0] * dims[1] * dims[2]);
}

Index3 unravel(std::size_t index, const Index3& dims) {
  const auto i = static_cast<std::int64_t>(index);
  return {i % dims[0], (i / dims[0]) % dims[1], i / (dims[0] * dims[1])};
}

/// Reports the fraction of `total` steps done through a progress callback, at most once per
/// whole percent, from any thread.
class ProgressCounter {
 public:
  ProgressCounter(const std::function<void(std::string_view, double)>& callback,
                  std::string_view stage, std::size_t total)
      : callback_(callback), stage_(stage), total_(total) {
    report(0);
  }
  void step() {
    if (callback_) {
      report(done_.fetch_add(1) + 1);
    }
  }

 private:
  void report(std::size_t done) {
    if (!callback_) {
      return;
    }
    const std::scoped_lock lock(mutex_);
    // A stage without steps is done at once.
    const double fraction =
        total_ == 0 ? 1.0 : static_cast<double>(done) / static_cast<double>(total_);
    const auto percent = static_cast<int>(fraction * 100.0);
    if (percent > last_percent_) {
      last_percent_ = percent;
      callback_(stage_, fraction);
    }
  }

  const std::function<void(std::string_view, double)>& callback_;
  std::string_view stage_;
  std::size_t total_;
  std::atomic<std::size_t> done_ = 0;
  std::mutex mutex_;
  int last_percent_ = -1;
};

// ---------------------------------------------------------------------------------------------
// Staging: a source with slow random access is copied once, slice by slice, to a raw file. The
// passes then read small regions in any order from the memory-mapped copy. Without it, pass 2
// decodes every slice again for each brick it touches whenever the slices of a brick layer do
// not fit into the source's own cache.

constexpr std::string_view kStagingPrefix = ".voxelsieve-staging-";

/// Removes the staging copies in `dir` of processes that no longer run: a process that was killed
/// or crashed leaves its copy, 2 bytes per voxel of the scan. Copies of running processes stay.
void removeStaleCopies(const std::filesystem::path& dir) {
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
    const std::string name = entry.path().filename().string();
    if (!name.starts_with(kStagingPrefix) || !name.ends_with(".raw")) {
      continue;
    }
    // .voxelsieve-staging-<process>-<stamp>.raw; older copies have no process id.
    const std::string rest = name.substr(kStagingPrefix.size());
    const auto dash = rest.find('-');
    if (dash == std::string::npos) {
      continue;
    }
    std::uint64_t process = 0;
    const auto [end, parsed] = std::from_chars(rest.data(), rest.data() + dash, process);
    if (parsed != std::errc() || end != rest.data() + dash || processRunning(process)) {
      continue;
    }
    std::error_code ignored;
    std::filesystem::remove(entry.path(), ignored);
  }
}

/// Result of pass 1: histogram and the k-th largest grey value of every 8^3 block.
struct BlockStatistics {
  Histogram histogram;
  std::vector<std::uint16_t> block_kth;
  Index3 block_dims{};
};

/// A temporary raw copy of a source, removed on destruction. The slices of the preview are copied
/// first, round by round, coarse to fine, then the rest, so the preview costs no extra reading.
class StagedSource {
 public:
  StagedSource(const VolumeSource& source, const std::filesystem::path& dir,
               const DatasetOptions& options)
      : input_(source),
        dims_(source.dims()),
        narrow_(source.sampleType() == SampleType::kUInt8),
        copied_(static_cast<std::size_t>(dims_[2]), 0),
        progress_(options.progress, "staging", static_cast<std::size_t>(dims_[2])) {
    // In the input's width: 8-bit input takes half the space.
    const std::uint64_t bytes = product(dims_) * (narrow_ ? 1U : 2U);
    std::filesystem::create_directories(dir);
    removeStaleCopies(dir);
    const std::uint64_t available = std::filesystem::space(dir).available;
    if (available < bytes) {
      constexpr double kGb = 1024.0 * 1024.0 * 1024.0;
      throw std::runtime_error(
          "Staging the input needs " + std::to_string(static_cast<double>(bytes) / kGb) +
          " GB in " + dir.string() + ", but only " +
          std::to_string(static_cast<double>(available) / kGb) +
          " GB are free; choose another staging directory or turn staging off");
    }
    // The process id in the name tells a copy in use from one left by a process that ended.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = dir / (std::string(kStagingPrefix) + std::to_string(processId()) + "-" +
                   std::to_string(stamp) + ".raw");
    try {
      // Written with plain positioned writes, not through a read-write mapping: the system bounds
      // the dirty pages of writes, and nothing then ties the decoders to the speed of the disk
      // through page faults (review finding D2). The file has its size at once, so slices
      // written out of order leave no gaps to fill later.
      std::ofstream(path_, std::ios::binary).close();
      std::filesystem::resize_file(path_, bytes);
#if defined(_WIN32)
      // A temporary file: Windows keeps its pages in memory instead of writing them to disk while
      // memory suffices. Written back, the copy stalled the next pass for half a minute with all
      // cores idle. The attribute applies to handles opened after it is set.
      (void)SetFileAttributesW(path_.c_str(), FILE_ATTRIBUTE_TEMPORARY);
#endif
      out_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
      if (!out_) {
        throwWriteError(path_);
      }
    } catch (...) {
      remove();
      throw;
    }
    // Pass 1 on the way (the block maxima need k = 1): every slice passes through memory once.
    if (options.min_material_voxels == 1) {
      block_dims_ = ceilDiv3(dims_, kBlockSize);
      block_max_ = std::vector<std::atomic<std::uint16_t>>(product(block_dims_));
    }
  }
  StagedSource(const StagedSource&) = delete;
  StagedSource& operator=(const StagedSource&) = delete;
  StagedSource(StagedSource&&) = delete;
  StagedSource& operator=(StagedSource&&) = delete;
  ~StagedSource() { remove(); }

  /// Copies the preview slices `round` (indices into `preview.targets()`) and hands each to
  /// `preview` while it is still in memory.
  void copyRound(detail::PreviewBuilder& preview, const std::vector<std::size_t>& round) {
    const auto& targets = preview.targets();
    copy(
        round.size(), [&](std::size_t i) { return targets[round[i]]; },
        [&](std::size_t i, const std::uint16_t* slice) { preview.add(round[i], slice); });
  }

  /// Copies the slices not copied yet; the copy is then what `source` reads. With `preview`, each
  /// is handed to it as it passes: added when a preview slice shows it, else counted.
  void finish(detail::PreviewBuilder* preview = nullptr) {
    copy(
        static_cast<std::size_t>(dims_[2]),
        [](std::size_t z) { return static_cast<std::int64_t>(z); },
        [preview](std::size_t z, const std::uint16_t* slice) {
          if (preview == nullptr) {
            return;
          }
          const auto& targets = preview->targets();
          const auto it =
              std::lower_bound(targets.begin(), targets.end(), static_cast<std::int64_t>(z));
          if (it != targets.end() && *it == static_cast<std::int64_t>(z)) {
            preview->add(static_cast<std::size_t>(it - targets.begin()), slice);
          } else {
            preview->count(slice);
          }
        });
    out_.close();
    if (out_.fail()) {
      throwWriteError(path_);
    }
    wide_.clear();  // the slice buffers of the threads
    narrow_buffers_.clear();
    raw_ = std::make_unique<MappedRawSource>(
        path_,
        RawLayout{dims_, input_.voxelSize(), narrow_ ? SampleType::kUInt8 : SampleType::kUInt16,
                  std::endian::native, std::uint64_t{0}});
  }

  [[nodiscard]] const VolumeSource& source() const { return *raw_; }

  /// Pass 1 of the copied slices, taken while they were in memory: nothing when k > 1.
  [[nodiscard]] std::optional<BlockStatistics> statistics() const {
    if (block_max_.empty()) {
      return std::nullopt;
    }
    BlockStatistics stats;
    stats.block_dims = block_dims_;
    stats.block_kth.reserve(block_max_.size());
    for (const auto& value : block_max_) {
      stats.block_kth.push_back(value.load());
    }
    stats.histogram.assign(kHistogramBins, 0);
    histograms_.combine_each([&](const Histogram& local) {
      for (std::size_t i = 0; i < kHistogramBins; ++i) {
        stats.histogram[i] += local[i];
      }
    });
    return stats;
  }

 private:
  /// Copies slices `slice(0)` to `slice(count - 1)` that were not copied before, calling
  /// `visit(k, data)` for each before its pages are let go.
  template <typename Slice, typename Visit>
  void copy(std::size_t count, const Slice& slice, const Visit& visit) {
    const auto slice_voxels = static_cast<std::size_t>(dims_[0] * dims_[1]);
    const auto copy_slice = [&](std::size_t k) {
      const std::int64_t z = slice(k);
      if (copied_[static_cast<std::size_t>(z)] != 0) {
        return;
      }
      const Box box{{0, 0, z}, {dims_[0], dims_[1], z + 1}};
      std::vector<std::uint16_t>& wide = wide_.local();
      wide.resize(slice_voxels);
      input_.readRegion(box, wide);
      visit(k, wide.data());
      collect(z, wide);
      const char* data = reinterpret_cast<const char*>(wide.data());
      std::size_t bytes = slice_voxels * sizeof(std::uint16_t);
      if (narrow_) {
        // Read in 16 bit, stored in 8: every value of an 8-bit source is at most 255.
        std::vector<std::uint8_t>& narrow = narrow_buffers_.local();
        narrow.resize(slice_voxels);
        std::transform(wide.begin(), wide.end(), narrow.begin(),
                       [](std::uint16_t v) { return static_cast<std::uint8_t>(v); });
        data = reinterpret_cast<const char*>(narrow.data());
        bytes = slice_voxels;
      }
      {
        const std::scoped_lock lock(write_mutex_);
        out_.seekp(static_cast<std::streamoff>(static_cast<std::uint64_t>(z) * bytes));
        out_.write(data, static_cast<std::streamsize>(bytes));
        if (!out_) {
          throwWriteError(path_);
        }
      }
      copied_[static_cast<std::size_t>(z)] = 1;
      progress_.step();
    };
    if (input_.sequentialAccess()) {
      // A compressed stream decodes on one thread anyway; in order, it is read once.
      for (std::size_t k = 0; k < count; ++k) {
        copy_slice(k);
      }
      return;
    }
    // One slice per task: each slice is decoded exactly once, in parallel, as many at once as the
    // work memory holds (a slice and its 8-bit copy per task, ADR 0034).
    detail::runInFlight(count, tasksInMemory(slice_voxels * 3), copy_slice);
  }

  /// Adds a slice to the histogram and the block maxima of pass 1.
  void collect(std::int64_t z, const std::vector<std::uint16_t>& slice) {
    if (block_max_.empty()) {
      return;
    }
    Histogram& histogram = histograms_.local();
    std::vector<std::uint16_t>& row_max = row_max_.local();
    const std::int64_t bz = z / kBlockSize;
    for (std::int64_t by = 0; by < block_dims_[1]; ++by) {
      row_max.assign(static_cast<std::size_t>(block_dims_[0]), 0);
      for (std::int64_t y = by * kBlockSize; y < std::min((by + 1) * kBlockSize, dims_[1]); ++y) {
        const std::uint16_t* line = slice.data() + y * dims_[0];
        for (std::int64_t x = 0; x < dims_[0]; ++x) {
          ++histogram[line[x]];
          std::uint16_t& current = row_max[static_cast<std::size_t>(x / kBlockSize)];
          current = std::max(current, line[x]);
        }
      }
      // Slices of one block layer run at once: the maximum is merged atomically.
      for (std::int64_t bx = 0; bx < block_dims_[0]; ++bx) {
        std::atomic<std::uint16_t>& target =
            block_max_[static_cast<std::size_t>(bx + block_dims_[0] * (by + block_dims_[1] * bz))];
        std::uint16_t seen = target.load();
        const std::uint16_t value = row_max[static_cast<std::size_t>(bx)];
        while (value > seen && !target.compare_exchange_weak(seen, value)) {
        }
      }
    }
  }

  void remove() {
    raw_.reset();
    if (out_.is_open()) {
      out_.close();
    }
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  const VolumeSource& input_;
  Index3 dims_;
  bool narrow_ = false;  // 8-bit input, staged in 8 bit
  tbb::enumerable_thread_specific<std::vector<std::uint16_t>> wide_;
  tbb::enumerable_thread_specific<std::vector<std::uint8_t>> narrow_buffers_;
  // One flag per slice; each is written by the one task that copies the slice.
  std::vector<std::uint8_t> copied_;
  ProgressCounter progress_;
  std::filesystem::path path_;
  std::fstream out_;
  std::mutex write_mutex_;
  // Pass 1 while staging (k = 1).
  Index3 block_dims_{};
  std::vector<std::atomic<std::uint16_t>> block_max_;
  mutable tbb::combinable<Histogram> histograms_{[] { return Histogram(kHistogramBins, 0); }};
  tbb::enumerable_thread_specific<std::vector<std::uint16_t>> row_max_;
  std::unique_ptr<MappedRawSource> raw_;
};

// ---------------------------------------------------------------------------------------------
// Pass 1: histogram and the k-th largest grey value per 8^3 block, k = min_material_voxels.
// A block holds at least k voxels above the threshold exactly when its k-th largest value is
// above it, so the threshold may still be chosen after the pass and the result matches the
// voxel count of the in-memory sieve. Only one value per block is kept (2 bytes); the k largest
// values live in per-task heaps for one row of blocks.

/// Lets the pages of a staged copy go now and then while it is read; the passes read each part
/// of it about once, so pages read long ago are only memory taken from others.
class PageRelease {
 public:
  PageRelease(const VolumeSource* staged, std::size_t every) : staged_(staged), every_(every) {}
  void step() {
    if (staged_ != nullptr && ++count_ % every_ == 0) {
      staged_->releaseMemory();
    }
  }

 private:
  const VolumeSource* staged_;
  std::size_t every_;
  std::atomic<std::size_t> count_{0};
};

BlockStatistics collectBlockStatistics(const VolumeSource& source, int k,
                                       const DatasetOptions& options, PageRelease& release) {
  constexpr std::int64_t kB = kBlockSize;
  const Index3 dims = source.dims();
  BlockStatistics stats;
  stats.block_dims = ceilDiv3(dims, kB);
  stats.block_kth.assign(product(stats.block_dims), 0);
  const auto heap_size = static_cast<std::size_t>(k);

  tbb::combinable<Histogram> histograms([] { return Histogram(kHistogramBins, 0); });
  const std::int64_t rows = stats.block_dims[1] * stats.block_dims[2];
  ProgressCounter progress(options.progress, "histogram", static_cast<std::size_t>(rows));
  // One task per row of blocks: an 8 x 8 x dims[0] region, so memory per task stays small.
  tbb::parallel_for(tbb::blocked_range<std::int64_t>(0, rows), [&](const auto& range) {
    Histogram& histogram = histograms.local();
    std::vector<std::uint16_t> buffer;
    // Min-heaps of the k largest values seen so far, one per block of the row.
    std::vector<std::uint16_t> heaps;
    for (std::int64_t row = range.begin(); row != range.end(); ++row) {
      const std::int64_t by = row % stats.block_dims[1];
      const std::int64_t bz = row / stats.block_dims[1];
      const Box box{{0, by * kB, bz * kB},
                    {dims[0], std::min((by + 1) * kB, dims[1]), std::min((bz + 1) * kB, dims[2])}};
      buffer.resize(static_cast<std::size_t>(box.voxelCount()));
      source.readRegion(box, buffer);
      release.step();
      std::uint16_t* block_kth = &stats.block_kth[static_cast<std::size_t>(
          stats.block_dims[0] * (by + stats.block_dims[1] * bz))];
      progress.step();
      std::size_t offset = 0;
      const std::int64_t lines = box.size(1) * box.size(2);
      if (heap_size == 1) {  // the common case: the block maximum
        for (std::int64_t line = 0; line < lines; ++line) {
          for (std::int64_t x = 0; x < dims[0]; ++x) {
            const std::uint16_t value = buffer[offset++];
            ++histogram[value];
            std::uint16_t& current = block_kth[x / kB];
            current = std::max(current, value);
          }
        }
        continue;
      }
      heaps.assign(static_cast<std::size_t>(stats.block_dims[0]) * heap_size, 0);
      for (std::int64_t line = 0; line < lines; ++line) {
        for (std::int64_t x = 0; x < dims[0]; ++x) {
          const std::uint16_t value = buffer[offset++];
          ++histogram[value];
          std::uint16_t* heap = &heaps[static_cast<std::size_t>(x / kB) * heap_size];
          if (value > heap[0]) {
            std::pop_heap(heap, heap + heap_size, std::greater<>());
            heap[heap_size - 1] = value;
            std::push_heap(heap, heap + heap_size, std::greater<>());
          }
        }
      }
      for (std::int64_t bx = 0; bx < stats.block_dims[0]; ++bx) {
        block_kth[bx] = heaps[static_cast<std::size_t>(bx) * heap_size];
      }
    }
  });

  stats.histogram.assign(kHistogramBins, 0);
  histograms.combine_each([&](const Histogram& local) {
    for (std::size_t i = 0; i < kHistogramBins; ++i) {
      stats.histogram[i] += local[i];
    }
  });
  return stats;
}

BlockGrid classifyBlocks(const BlockStatistics& stats, float threshold,
                         const AirAxes& outside_air_axes) {
  BlockGrid blocks;
  blocks.dims = stats.block_dims;
  blocks.states.resize(stats.block_kth.size());
  std::transform(stats.block_kth.begin(), stats.block_kth.end(), blocks.states.begin(),
                 [threshold](std::uint16_t kth) {
                   return static_cast<float>(kth) > threshold ? BlockState::kMaterial
                                                              : BlockState::kAir;
                 });
  detail::floodFillOutsideAir(blocks, outside_air_axes);
  return blocks;
}

// ---------------------------------------------------------------------------------------------
// Pass 2: level-0 bricks.

void setGridProperties(openvdb::GridBase& grid, int level, const VoxelSize& voxel_size) {
  grid.setTransform(detail::voxelTransform(voxel_size, level));
  grid.setGridClass(openvdb::GRID_FOG_VOLUME);
  grid.setName("density");
  grid.insertMeta("voxelsieve_level", openvdb::Int32Metadata(level));
}

/// Builds one level-0 brick, or returns nullptr when the sieve keeps nothing inside it. `values`
/// holds the brick's grey values while it is built; reused from brick to brick, its memory is not
/// handed back to the system and faulted in again for every brick.
template <class Grid>
typename Grid::Ptr buildBrick(const VolumeSource& source, const BlockGrid& blocks,
                              const Index3& brick, const DatasetOptions& options,
                              UninitializedVector<std::uint16_t>& values) {
  using Value = typename Grid::ValueType;
  const Index3 dims = source.dims();
  const std::int64_t blocks_per_brick = options.brick_size / kBlockSize;
  const std::int64_t halo = ceilDiv(options.margin_voxels, kBlockSize);

  // Kept blocks in the brick plus a halo, so the margin can grow in from neighbouring bricks.
  auto grid = Grid::create(Value{0});
  auto& tree = grid->tree();
  bool any_kept = false;
  Index3 lo{};
  Index3 hi{};
  for (std::size_t i = 0; i < 3; ++i) {
    lo[i] = std::max<std::int64_t>(brick[i] * blocks_per_brick - halo, 0);
    hi[i] = std::min((brick[i] + 1) * blocks_per_brick + halo, blocks.dims[i]);
  }
  const openvdb::Coord volume_max = toCoord({dims[0] - 1, dims[1] - 1, dims[2] - 1});
  for (std::int64_t bz = lo[2]; bz < hi[2]; ++bz) {
    for (std::int64_t by = lo[1]; by < hi[1]; ++by) {
      for (std::int64_t bx = lo[0]; bx < hi[0]; ++bx) {
        if (!blocks.kept(bx, by, bz)) {
          continue;
        }
        any_kept = true;
        const openvdb::Coord min = toCoord({bx * kBlockSize, by * kBlockSize, bz * kBlockSize});
        const openvdb::Coord max = openvdb::Coord::minComponent(
            min.offsetBy(static_cast<openvdb::Int32>(kBlockSize - 1)), volume_max);
        tree.fill(openvdb::CoordBBox(min, max), Value{0}, /*active=*/true);
      }
    }
  }
  if (!any_kept) {
    return nullptr;
  }
  if (options.margin_voxels > 0) {
    openvdb::tools::dilateActiveValues(tree, options.margin_voxels, openvdb::tools::NN_FACE_EDGE,
                                       openvdb::tools::EXPAND_TILES);
  }

  Box box;
  for (std::size_t i = 0; i < 3; ++i) {
    box.min[i] = brick[i] * options.brick_size;
    box.max[i] = std::min((brick[i] + 1) * options.brick_size, dims[i]);
  }
  tree.clip(openvdb::CoordBBox(toCoord(box.min),
                               toCoord({box.max[0] - 1, box.max[1] - 1, box.max[2] - 1})));
  tree.voxelizeActiveTiles(/*threaded=*/false);
  if (tree.activeVoxelCount() == 0) {
    return nullptr;
  }

  // Isolated while `values` is in use: a thread waiting for TBB tasks in here must not start
  // another brick, which would take the same buffer. Isolating the whole brick instead left most
  // cores idle while threads waited in the parallel parts of OpenVDB above.
  tbb::this_task_arena::isolate([&] {
    values.resize(static_cast<std::size_t>(box.voxelCount()));
    source.readRegion(box, values);
    if constexpr (std::is_same_v<Value, std::uint8_t>) {
      // 8-bit grids hold 8-bit input only (writeDataset checks the source's sample type).
      if (const auto it =
              std::find_if(values.begin(), values.end(), [](std::uint16_t v) { return v > 255; });
          it != values.end()) {
        throw std::runtime_error("Grey value " + std::to_string(*it) +
                                 " of an 8-bit input does not fit into 8 bits");
      }
    }
    openvdb::tree::LeafManager<typename Grid::TreeType> leaves(tree);
    leaves.foreach (
        [&](typename Grid::TreeType::LeafNodeType& leaf, std::size_t) {
          for (auto it = leaf.beginValueOn(); it; ++it) {
            const openvdb::Coord c = it.getCoord();
            const auto index = static_cast<std::size_t>(
                (c.x() - box.min[0]) +
                box.size(0) * ((c.y() - box.min[1]) + box.size(1) * (c.z() - box.min[2])));
            it.setValue(static_cast<Value>(values[index]));
          }
        },
        /*threaded=*/false);
  });
  return grid;
}

GreyGrid buildBrick(ValueType type, const VolumeSource& source, const BlockGrid& blocks,
                    const Index3& brick, const DatasetOptions& options,
                    UninitializedVector<std::uint16_t>& values) {
  switch (type) {
    case ValueType::kUInt16:
      return buildBrick<UInt16Grid>(source, blocks, brick, options, values);
    case ValueType::kUInt8:
      return buildBrick<UInt8Grid>(source, blocks, brick, options, values);
    case ValueType::kFloat:
      break;
  }
  return buildBrick<openvdb::FloatGrid>(source, blocks, brick, options, values);
}

// ---------------------------------------------------------------------------------------------
// Coarser levels.

/// Integer grid with the topology of `mean` and its values rounded to the nearest integer.
template <class Grid>
GreyGrid roundedGrid(const openvdb::FloatGrid& mean) {
  using Value = typename Grid::ValueType;
  auto grid = Grid::create(
      std::make_shared<typename Grid::TreeType>(mean.tree(), Value{0}, openvdb::TopologyCopy()));
  auto accessor = grid->getAccessor();
  for (auto it = mean.cbeginValueOn(); it; ++it) {
    accessor.setValueOnly(it.getCoord(), static_cast<Value>(std::lround(*it)));
  }
  return grid;
}

/// Mean of the active children of each voxel of the coarser level that `child`, one brick of the
/// finer level, covers. The brick edge is even, so every coarse voxel takes its children from one
/// brick, and the parts from the up to 2x2x2 bricks below a coarse brick never overlap. Integer
/// types store the mean rounded to the nearest integer (ADR 0021).
GreyGrid downsample(GreyGrid child, ValueType type) {
  using Leaf = openvdb::FloatTree::LeafNodeType;
  auto sum = openvdb::FloatGrid::create(0.0F);
  auto count = openvdb::FloatGrid::create(0.0F);
  auto sum_acc = sum->getAccessor();
  auto count_acc = count->getAccessor();
  child.visit([&](auto& grid) {
    grid.tree().voxelizeActiveTiles();
    // A leaf of 8^3 voxels falls into one octant of a parent leaf, so each child leaf looks up
    // its two parent leaves once and then adds voxel by voxel.
    for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
      const openvdb::Coord origin = leaf->origin();
      const openvdb::Coord parent_origin(origin.x() >> 1, origin.y() >> 1, origin.z() >> 1);
      Leaf* sum_leaf = sum_acc.touchLeaf(parent_origin);
      Leaf* count_leaf = count_acc.touchLeaf(parent_origin);
      for (auto it = leaf->cbeginValueOn(); it; ++it) {
        const openvdb::Coord c = it.getCoord();
        const openvdb::Index n =
            Leaf::coordToOffset(openvdb::Coord(c.x() >> 1, c.y() >> 1, c.z() >> 1));
        sum_leaf->setValueOn(n, sum_leaf->getValue(n) + static_cast<float>(*it));
        count_leaf->setValueOn(n, count_leaf->getValue(n) + 1.0F);
      }
    }
  });
  for (auto leaf = sum->tree().beginLeaf(); leaf; ++leaf) {
    const Leaf* count_leaf = count_acc.probeConstLeaf(leaf->origin());
    for (auto it = leaf->beginValueOn(); it; ++it) {
      it.setValue(*it / count_leaf->getValue(it.pos()));
    }
  }
  switch (type) {
    case ValueType::kUInt16:
      return roundedGrid<UInt16Grid>(*sum);
    case ValueType::kUInt8:
      return roundedGrid<UInt8Grid>(*sum);
    case ValueType::kFloat:
      break;
  }
  return sum;
}

/// One grid of the disjoint `parts` of the same type; no grid when there are none.
GreyGrid combineParts(std::vector<GreyGrid>& parts) {
  GreyGrid result;
  for (GreyGrid& part : parts) {
    if (!part) {
      continue;
    }
    if (!result) {
      result = part;
      continue;
    }
    result.visit([&](auto& grid) {
      part.visit([&](const auto& other) {
        if constexpr (std::is_same_v<std::decay_t<decltype(grid)>, std::decay_t<decltype(other)>>) {
          openvdb::tools::compReplace(grid, other);
        }
      });
    });
  }
  return result;
}

void writeBrick(const std::filesystem::path& path, const GreyGrid& grid) {
  writeVdb(path, {grid.basePtr()});
}

nlohmann::json toJson(const DatasetInfo& info) {
  nlohmann::json levels = nlohmann::json::array();
  for (const LevelInfo& level : info.levels) {
    levels.push_back({{"level", level.level},
                      {"dims", level.dims},
                      {"voxel_size_mm", level.voxel_size},
                      {"bricks", level.bricks}});
  }
  nlohmann::json json = {{"format", "voxelsieve-dataset"},
                         {"version", kFormatVersion},
                         {"value_type", std::string(valueTypeName(info.value_type))},
                         {"dims", info.dims}};
  writeVoxelSize(json, info.voxel_size);
  json.update({{"brick_size", info.brick_size},
               {"threshold", info.threshold},
               {"air_level", info.air_level},
               {"margin_voxels", info.margin_voxels},
               {"min_material_voxels", info.min_material_voxels},
               {"active_voxel_count", info.active_voxel_count},
               {"overview", "overview.vdb"},
               {"levels", levels}});
  if (info.outside_air_axes != kAllAxes) {
    json["outside_air_axes"] = airAxesName(info.outside_air_axes);
  }
  if (info.value_type != ValueType::kFloat) {
    json["coarse_levels"] = "rounded mean";
  }
  if (!info.value_mapping.isIdentity()) {
    json["value_mapping"] = {{"offset", info.value_mapping.offset},
                             {"scale", info.value_mapping.scale}};
  }
  return json;
}

/// Writes the coarser levels of a dataset whose level 0 is written and listed in `info`, until one
/// brick holds the whole volume, then overview.vdb and index.json.
///
/// The coarse bricks form a tree over the level-0 bricks, built depth first and in parallel at
/// every node: a coarse brick is written as soon as the bricks below it are, and handed to its
/// parent in memory, so no level waits for the one before it to finish and only level 0 is read
/// back from disk.
void writeCoarseLevels(const std::filesystem::path& dir, DatasetInfo& info,
                       const std::function<void(std::string_view, double)>& progress) {
  std::vector<LevelInfo> coarse;
  for (const LevelInfo* finer = &info.levels.back();
       std::any_of(finer->dims.begin(), finer->dims.end(),
                   [&](std::int64_t d) { return d > info.brick_size; });
       finer = &coarse.back()) {
    LevelInfo coarser{
        finer->level + 1, ceilDiv3(finer->dims, 2), finer->voxel_size.scaled(2.0), {}};
    std::filesystem::create_directories(brickPath(dir, coarser.level, {0, 0, 0}).parent_path());
    coarse.push_back(std::move(coarser));
  }
  const TelemetryPhase phase("levels");
  const LevelInfo& base = info.levels.front();
  const std::set<Index3> base_bricks(base.bricks.begin(), base.bricks.end());
  ProgressCounter level_progress(progress, "levels", coarse.empty() ? 0 : base.bricks.size());
  std::mutex mutex;

  // Coarse brick `brick` of `level` (1 or more), written and returned; no grid when no brick of
  // level 0 lies below it.
  std::function<GreyGrid(int, const Index3&)> build = [&](int level, const Index3& brick) {
    const Index3 finer_bricks = ceilDiv3(
        level == 1 ? base.dims : coarse[static_cast<std::size_t>(level - 2)].dims, info.brick_size);
    std::vector<Index3> children;
    for (std::int64_t dz = 0; dz < 2; ++dz) {
      for (std::int64_t dy = 0; dy < 2; ++dy) {
        for (std::int64_t dx = 0; dx < 2; ++dx) {
          const Index3 child{2 * brick[0] + dx, 2 * brick[1] + dy, 2 * brick[2] + dz};
          if (child[0] < finer_bricks[0] && child[1] < finer_bricks[1] &&
              child[2] < finer_bricks[2] && (level > 1 || base_bricks.contains(child))) {
            children.push_back(child);
          }
        }
      }
    }
    std::vector<GreyGrid> parts(children.size());
    tbb::parallel_for(std::size_t{0}, children.size(), [&](std::size_t i) {
      GreyGrid child = level == 1 ? readBrick(brickPath(dir, 0, children[i]), /*delay_load=*/false)
                                  : build(level - 1, children[i]);
      if (child) {
        parts[i] = downsample(std::move(child), info.value_type);
      }
      if (level == 1) {
        level_progress.step();
      }
    });
    GreyGrid grid = combineParts(parts);
    if (!grid) {
      return grid;
    }
    setGridProperties(grid.base(), level, info.voxel_size);
    writeBrick(brickPath(dir, level, brick), grid);
    const std::scoped_lock lock(mutex);
    coarse[static_cast<std::size_t>(level - 1)].bricks.push_back(brick);
    return grid;
  };
  if (!coarse.empty()) {
    build(coarse.back().level, {0, 0, 0});
  }
  for (LevelInfo& level : coarse) {
    std::sort(level.bricks.begin(), level.bricks.end());
    info.levels.push_back(std::move(level));
  }

  // Overview: the single brick of the coarsest level, or an empty grid for an empty dataset.
  const LevelInfo& top = info.levels.back();
  if (top.bricks.empty()) {
    const GreyGrid empty = GreyGrid::create(info.value_type);
    setGridProperties(empty.base(), top.level, info.voxel_size);
    writeBrick(dir / "overview.vdb", empty);
  } else {
    // Overwritten: a resumed import may have written it before.
    std::filesystem::copy_file(brickPath(dir, top.level, top.bricks.front()), dir / "overview.vdb",
                               std::filesystem::copy_options::overwrite_existing);
  }

  // Last and whole: a dataset whose write was interrupted has no index.json, and never half of one.
  writeJson(dir / "index.json", toJson(info));
}

// ---------------------------------------------------------------------------------------------
// Journal of an import, so that an interrupted one resumes: import.json says what was imported,
// pass1.bin holds the result of pass 1 (the k-th largest value of every block and the histogram),
// bricks.log one line per finished level-0 brick with its active voxels. Brick files are written
// through a temporary file, so a listed brick is whole.

constexpr std::string_view kJournalFile = "import.json";
constexpr std::string_view kPass1File = "pass1.bin";
constexpr std::string_view kBricksLog = "bricks.log";
constexpr std::string_view kDerivedJournalFile = "derived.json";
constexpr int kJournalVersion = 1;

/// What must match for an import to resume: the input's size and the options that shape bricks.
nlohmann::json journalKey(const DatasetInfo& info, const DatasetOptions& options) {
  nlohmann::json key = {
      {"dims", info.dims},
      {"brick_size", options.brick_size},
      {"margin_voxels", options.margin_voxels},
      {"min_material_voxels", options.min_material_voxels},
      {"value_type", std::string(valueTypeName(info.value_type))},
      {"outside_air_axes", airAxesName(options.outside_air_axes)},
      {"value_mapping",
       {{"offset", info.value_mapping.offset}, {"scale", info.value_mapping.scale}}}};
  key["threshold"] = options.threshold ? nlohmann::json(*options.threshold) : nlohmann::json();
  writeVoxelSize(key, info.voxel_size);
  return key;
}

struct Journal {
  BlockStatistics stats;
  /// Level-0 bricks with their active voxels (0 when empty), sorted by brick.
  std::vector<std::pair<Index3, std::int64_t>> finished;
  [[nodiscard]] bool isFinished(const Index3& brick) const {
    return std::binary_search(finished.begin(), finished.end(), std::pair{brick, std::int64_t{0}},
                              [](const auto& a, const auto& b) { return a.first < b.first; });
  }
};

void writePass1(const std::filesystem::path& dir, const nlohmann::json& key,
                const BlockStatistics& stats) {
  writeFile(dir / kPass1File, [&](std::ostream& out) {
    out.write(reinterpret_cast<const char*>(stats.block_kth.data()),
              static_cast<std::streamsize>(stats.block_kth.size() * sizeof(std::uint16_t)));
    out.write(reinterpret_cast<const char*>(stats.histogram.data()),
              static_cast<std::streamsize>(stats.histogram.size() * sizeof(Histogram::value_type)));
  });
  writeJson(dir / kJournalFile, {{"format", "voxelsieve-import"},
                                 {"version", kJournalVersion},
                                 {"key", key},
                                 {"block_dims", stats.block_dims},
                                 {"blocks", stats.block_kth.size()},
                                 {"bins", stats.histogram.size()}});
}

std::vector<std::pair<Index3, std::int64_t>> readBrickLog(const std::filesystem::path& file);

/// The journal in `dir`, or none; throws when it was written for another input or options.
std::optional<Journal> readJournal(const std::filesystem::path& dir, const nlohmann::json& key) {
  std::ifstream head_in(dir / kJournalFile);
  if (!head_in) {
    return std::nullopt;
  }
  const nlohmann::json head = nlohmann::json::parse(head_in, nullptr, /*allow_exceptions=*/false);
  if (head.is_discarded() || head.value("format", "") != "voxelsieve-import" ||
      head.value("version", 0) != kJournalVersion) {
    return std::nullopt;
  }
  if (head.at("key") != key) {
    throw std::invalid_argument(
        "The output directory " + dir.string() +
        " holds an interrupted import of another input or with other options; remove it to "
        "start again, or import with the same options to resume");
  }
  Journal journal;
  journal.stats.block_dims = head.at("block_dims").get<Index3>();
  journal.stats.block_kth.resize(head.at("blocks").get<std::size_t>());
  journal.stats.histogram.resize(head.at("bins").get<std::size_t>());
  std::ifstream in(dir / kPass1File, std::ios::binary);
  in.read(reinterpret_cast<char*>(journal.stats.block_kth.data()),
          static_cast<std::streamsize>(journal.stats.block_kth.size() * sizeof(std::uint16_t)));
  in.read(
      reinterpret_cast<char*>(journal.stats.histogram.data()),
      static_cast<std::streamsize>(journal.stats.histogram.size() * sizeof(Histogram::value_type)));
  if (!in || product(journal.stats.block_dims) != journal.stats.block_kth.size()) {
    return std::nullopt;  // pass 1 was not written completely: run it again
  }
  journal.finished = readBrickLog(dir / kBricksLog);
  return journal;
}

/// The bricks listed in a bricks.log with their active voxels, sorted by brick. Lines cut off by
/// an interruption are left out; their bricks are built again.
std::vector<std::pair<Index3, std::int64_t>> readBrickLog(const std::filesystem::path& file) {
  std::vector<std::pair<Index3, std::int64_t>> finished;
  std::ifstream log(file);
  std::string line;
  while (std::getline(log, line)) {
    Index3 brick{};
    std::int64_t active = -1;
    const char* p = line.data();
    const char* end = line.data() + line.size();
    bool ok = true;
    for (std::int64_t* value : {&brick[0], &brick[1], &brick[2], &active}) {
      while (p < end && *p == ' ') {
        ++p;
      }
      const auto [next, error] = std::from_chars(p, end, *value);
      ok = ok && error == std::errc();
      p = next;
    }
    if (ok && active >= 0 && p == end) {
      finished.emplace_back(brick, active);
    }
  }
  // Sorted by brick, a brick listed twice once.
  std::sort(finished.begin(), finished.end());
  finished.erase(std::unique(finished.begin(), finished.end(),
                             [](const auto& a, const auto& b) { return a.first == b.first; }),
                 finished.end());
  return finished;
}

/// The level-0 brick of a derived dataset: the active voxels of `in` with the values of `grey`
/// (one per voxel of `box`, x fastest), in `type`; NaN removes a voxel.
GreyGrid derivedBrick(const GreyGrid& in, ValueType type, const Box& box,
                      const std::vector<float>& grey) {
  return in.visit([&](const auto& typed) {
    GreyGrid result = GreyGrid::create(type);
    result.visit([&](auto& grid) {
      using Tree = typename std::decay_t<decltype(grid)>::TreeType;
      using Value = typename Tree::ValueType;
      grid.setTree(std::make_shared<Tree>(typed.tree(), Value{0}, openvdb::TopologyCopy()));
      grid.tree().voxelizeActiveTiles();
      for (auto leaf = grid.tree().beginLeaf(); leaf; ++leaf) {
        for (auto it = leaf->beginValueOn(); it; ++it) {
          const openvdb::Coord c = it.getCoord();
          const float value = grey[static_cast<std::size_t>(
              (c.x() - box.min[0]) +
              box.size(0) * ((c.y() - box.min[1]) + box.size(1) * (c.z() - box.min[2])))];
          if (std::isnan(value)) {
            it.setValueOff();
          } else if constexpr (std::is_same_v<Value, float>) {
            it.setValue(value);
          } else {
            constexpr double kMax = std::numeric_limits<Value>::max();
            it.setValue(static_cast<Value>(std::clamp(std::round(double{value}), 0.0, kMax)));
          }
        }
      }
      openvdb::tools::pruneInactive(grid.tree());  // leaves whose voxels were all removed
    });
    return result;
  });
}

/// Appends finished bricks to bricks.log, a whole line at a time.
class BrickLog {
 public:
  explicit BrickLog(const std::filesystem::path& file) : path_(file), out_(file, std::ios::app) {
    if (!out_) {
      throwWriteError(path_);
    }
  }
  void add(const Index3& brick, std::int64_t active) {
    const std::string line = std::to_string(brick[0]) + " " + std::to_string(brick[1]) + " " +
                             std::to_string(brick[2]) + " " + std::to_string(active) + "\n";
    const std::scoped_lock lock(mutex_);
    out_ << line;
    out_.flush();
    if (!out_) {
      throwWriteError(path_);
    }
  }

 private:
  std::filesystem::path path_;
  std::mutex mutex_;
  std::ofstream out_;
};

/// Removes the temporary files of writes that a process which ended left in `dir` and below.
void removeStalePartials(const std::filesystem::path& dir) {
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(dir, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    const std::string name = it->path().filename().string();
    const auto at = name.rfind(".partial-");
    if (at == std::string::npos) {
      continue;
    }
    std::uint64_t pid = 0;
    const char* first = name.data() + at + std::string_view(".partial-").size();
    if (std::from_chars(first, name.data() + name.size(), pid).ec == std::errc() &&
        !processRunning(pid)) {
      std::error_code ignored;
      std::filesystem::remove(it->path(), ignored);
    }
  }
}

}  // namespace

std::filesystem::path brickPath(const std::filesystem::path& dir, int level, const Index3& brick) {
  return dir / ("level" + std::to_string(level)) /
         (std::to_string(brick[0]) + "_" + std::to_string(brick[1]) + "_" +
          std::to_string(brick[2]) + ".vdb");
}

GreyGrid readBrick(const std::filesystem::path& file, bool delay_load) {
  initializeVdb();
  GreyGrid grid;
  if (delay_load) {
    openvdb::io::File vdb(file.string());
#ifndef _WIN32
    // OpenVDB otherwise copies every file below 500 MB to a private temp file before mapping it:
    // a second write and read of each brick, and temp space beside the dataset. Bricks are not
    // changed once written. Windows keeps the copy: a mapped file cannot be deleted there, and the
    // GUI deletes the steps of a history it discards while it may still show their datasets.
    vdb.setCopyMaxBytes(0);
#endif
    vdb.open(/*delayLoad=*/true);
    grid = GreyGrid::fromBase(vdb.readGrid("density"));
    vdb.close();
    // OpenVDB reads the leaves later, as they are touched; the file counts as opened once.
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    countRead(CountedRead::kBrickFile, error ? 0 : size);
  } else {
    // Read in one piece and parsed from memory: from the file, OpenVDB seeks and reads again for
    // every leaf, two system calls per 512 voxels.
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
      throw std::runtime_error("Cannot open " + file.string());
    }
    std::vector<char> bytes(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
      throw std::runtime_error("Cannot read " + file.string());
    }
    countRead(CountedRead::kBrickFile, bytes.size());
    boost::iostreams::stream<boost::iostreams::array_source> stream(bytes.data(), bytes.size());
    openvdb::io::Stream vdb(stream, /*delayLoad=*/false);
    for (const openvdb::GridBase::Ptr& base : *vdb.getGrids()) {
      if (base->getName() == "density") {
        grid = GreyGrid::fromBase(base);
        break;
      }
    }
  }
  if (!grid) {
    throw std::runtime_error("No grey-value grid 'density' in " + file.string());
  }
  return grid;
}

DatasetInfo writeDataset(const VolumeSource& input, const std::filesystem::path& dir,
                         const DatasetOptions& options) {
  checkVolumeDims(input.dims(), "The volume to import");
  if (options.brick_size <= 0 || options.brick_size % kBlockSize != 0) {
    throw std::invalid_argument("brick_size must be a positive multiple of 8");
  }
  if (options.margin_voxels < 0) {
    throw std::invalid_argument("margin_voxels must be >= 0");
  }
  if (options.min_material_voxels < 1 ||
      options.min_material_voxels > kBlockSize * kBlockSize * kBlockSize) {
    throw std::invalid_argument("min_material_voxels must be between 1 and 512");
  }
  const ValueType native =
      input.sampleType() == SampleType::kUInt8 ? ValueType::kUInt8 : ValueType::kUInt16;
  if (options.value_type == ValueType::kUInt8 && native != ValueType::kUInt8) {
    throw std::invalid_argument(
        "8-bit grids need 8-bit input; 16-bit grey values would be quantised");
  }
  initializeVdb();

  DatasetInfo info;
  info.dims = input.dims();
  info.voxel_size = input.voxelSize();
  info.voxel_size.validate();
  info.value_mapping = input.valueMapping();
  info.brick_size = options.brick_size;
  info.value_type = options.value_type.value_or(native);
  info.margin_voxels = options.margin_voxels;
  info.min_material_voxels = options.min_material_voxels;
  info.outside_air_axes = options.outside_air_axes;

  // An interrupted import of the same input with the same options resumes from its journal;
  // anything else in the directory is an error. Copies and partial files of processes that ended
  // are no content.
  const nlohmann::json journal_key = journalKey(info, options);
  std::optional<Journal> journal;
  if (std::filesystem::exists(dir)) {
    removeStaleCopies(dir);
    removeStalePartials(dir);
  }
  if (std::filesystem::exists(dir) && !std::filesystem::is_empty(dir)) {
    if (options.resume && !std::filesystem::exists(dir / "index.json")) {
      journal = readJournal(dir, journal_key);
    }
    if (!journal) {
      throw std::invalid_argument("Output directory is not empty: " + dir.string());
    }
  }
  const Index3 brick_dims = ceilDiv3(info.dims, options.brick_size);
  const std::size_t remaining =
      product(brick_dims) - (journal ? journal->finished.size() : std::size_t{0});

  // A source read in one pass (gzip) is always copied: read in place, every brick would start
  // the stream again. A resumed import with every brick done reads no more of the input.
  const bool sequential = input.sequentialAccess();
  std::unique_ptr<StagedSource> staged;
  if (remaining > 0 && ((options.stage_slow_sources && input.slowRandomAccess()) || sequential)) {
    staged = std::make_unique<StagedSource>(
        input, options.staging_dir.empty() ? dir : options.staging_dir, options);
  }
  std::optional<detail::PreviewBuilder> builder;
  if (options.preview && staged) {
    builder.emplace(info.dims, info.voxel_size, info.value_mapping, options.preview_options);
  }
  // The preview's slices are the first the staging copy reads, coarse to fine (ADR 0020): the
  // first round is the first look, each further round gives a sharper preview.
  std::vector<std::vector<std::size_t>> rounds;
  if (options.preview && !sequential) {
    const TelemetryPhase phase("preview");
    if (builder) {
      rounds = builder->rounds();
      staged->copyRound(*builder, rounds.front());
      options.preview(builder->snapshot());
    } else {
      options.preview(readImportPreview(input, options.preview_options));
    }
  }
  if (staged) {
    {
      const TelemetryPhase phase("staging");
      for (std::size_t r = 1; builder && r < rounds.size(); ++r) {
        staged->copyRound(*builder, rounds[r]);
        options.preview(builder->snapshot());
      }
      const std::int64_t previewed = builder && !sequential ? builder->slicesRead() : 0;
      // The rest of the slices; the preview takes them as they pass (a stream read in one pass
      // passes all of them here) and so ends with the histogram of the whole volume.
      staged->finish(builder ? &*builder : nullptr);
      // The copy is all the passes read, so its pages should stay in memory, not the input's.
      input.releaseMemory();
      if (builder && builder->slicesRead() > previewed) {
        options.preview(builder->snapshot());
      }
    }
  }
  const VolumeSource& source = staged ? staged->source() : input;

  std::optional<TelemetryPhase> phase(std::in_place, "pass 1 (histogram)");
  // The pages of a mapped input go now and then, as those of the staged copy: the passes read
  // each part about once, and a mapped 300 GB raw file would otherwise push the rest of the
  // machine into paging. Slow sources (TIFF, DICOM) would drop their decoded caches instead.
  const VolumeSource* released = staged || !input.slowRandomAccess() ? &source : nullptr;
  PageRelease rows_release(released, 1024);
  BlockStatistics stats;
  std::optional<BlockStatistics> staged_stats;
  if (staged && !journal) {
    staged_stats = staged->statistics();
  }
  if (journal) {
    stats = std::move(journal->stats);
  } else if (staged_stats) {
    // Taken while the slices were copied: the copy is not read a second time for it (4.2).
    stats = std::move(*staged_stats);
    if (options.progress) {
      options.progress("histogram", 1.0);  // done with the staging
    }
    std::filesystem::create_directories(dir);
    writePass1(dir, journal_key, stats);
  } else {
    stats = collectBlockStatistics(source, options.min_material_voxels, options, rows_release);
    std::filesystem::create_directories(dir);
    writePass1(dir, journal_key, stats);
  }
  const detail::ThresholdResult estimate = detail::airThreshold(stats.histogram);
  info.threshold = options.threshold.value_or(estimate.threshold);
  info.air_level = estimate.air_level;
  const BlockGrid blocks = classifyBlocks(stats, info.threshold, options.outside_air_axes);

  // The bricks need about the kept blocks in the stored width (CT noise hardly compresses), and a
  // seventh more for the coarser levels: say so now rather than after hours of writing.
  if (remaining > 0) {
    const auto kept = static_cast<std::uint64_t>(
        std::count_if(blocks.states.begin(), blocks.states.end(),
                      [](BlockState s) { return s != BlockState::kOutsideAir; }));
    const std::uint64_t value_bytes = info.value_type == ValueType::kUInt8    ? 1
                                      : info.value_type == ValueType::kUInt16 ? 2
                                                                              : 4;
    const double fraction_left =
        static_cast<double>(remaining) / static_cast<double>(product(brick_dims));
    const auto needed = static_cast<std::uint64_t>(
        static_cast<double>(kept * kBlockSize * kBlockSize * kBlockSize * value_bytes) *
        (fraction_left + 1.0 / 7.0));
    std::error_code error;
    const auto space = std::filesystem::space(dir, error);
    if (!error && space.available < needed) {
      constexpr double kGb = 1024.0 * 1024.0 * 1024.0;
      throw std::runtime_error("The dataset needs about " +
                               std::to_string(static_cast<double>(needed) / kGb) + " GB in " +
                               dir.string() + ", but only " +
                               std::to_string(static_cast<double>(space.available) / kGb) +
                               " GB are free; free some space and run the import again to resume");
    }
  }

  // Level 0.
  phase.reset();
  phase.emplace("pass 2 (bricks)");
  LevelInfo level0{0, info.dims, info.voxel_size, {}};
  std::filesystem::create_directories(brickPath(dir, 0, {0, 0, 0}).parent_path());
  std::mutex mutex;
  ProgressCounter brick_progress(options.progress, "bricks", remaining);
  PageRelease bricks_release(released, 16);
  std::optional<BrickLog> log(std::in_place, dir / kBricksLog);
  if (journal) {
    for (const auto& [brick, active] : journal->finished) {
      if (active > 0) {
        level0.bricks.push_back(brick);
        info.active_voxel_count += active;
      }
    }
  }
  tbb::enumerable_thread_specific<UninitializedVector<std::uint16_t>> buffers;
  tbb::parallel_for(std::size_t{0}, product(brick_dims), [&](std::size_t i) {
    const Index3 brick = unravel(i, brick_dims);
    if (journal && journal->isFinished(brick)) {
      return;
    }
    // In a context of its own: once another brick fails, TBB cancels the loops nested in the
    // others too, and OpenVDB's algorithms then leave half-built trees behind. Isolated, every
    // brick in work is finished and listed before the failure reaches the caller.
    tbb::task_group_context context(tbb::task_group_context::isolated);
    tbb::task_group group(context);
    // Isolated as well: waiting for it, this thread takes no other brick, whose buffer it shares.
    tbb::this_task_arena::isolate([&] {
      group.run_and_wait([&] {
        const GreyGrid grid =
            buildBrick(info.value_type, source, blocks, brick, options, buffers.local());
        bricks_release.step();
        if (!grid) {
          log->add(brick, 0);
          brick_progress.step();
          return;
        }
        setGridProperties(grid.base(), 0, info.voxel_size);
        grid.base().insertMeta("voxelsieve_threshold", openvdb::FloatMetadata(info.threshold));
        writeBrick(brickPath(dir, 0, brick), grid);
        // Counted outside the lock: OpenVDB counts with TBB tasks, and a thread waiting for them
        // may run another brick of this loop, which would then block on the lock it holds.
        const auto active = static_cast<std::int64_t>(grid.base().activeVoxelCount());
        log->add(brick, active);
        {
          const std::scoped_lock lock(mutex);
          level0.bricks.push_back(brick);
          info.active_voxel_count += active;
        }
        brick_progress.step();
      });
    });
  });
  log.reset();  // closed, so that it can be removed at the end
  std::sort(level0.bricks.begin(), level0.bricks.end());
  info.levels.push_back(std::move(level0));

  phase.reset();
  writeCoarseLevels(dir, info, options.progress);
  // The dataset is whole: the journal is no longer needed.
  for (const std::string_view name : {kJournalFile, kPass1File, kBricksLog}) {
    std::error_code ignored;
    std::filesystem::remove(dir / name, ignored);
  }
  return info;
}

DatasetInfo writeDerivedDataset(const Dataset& input, const std::filesystem::path& dir,
                                const BrickValues& values, const DerivedDatasetOptions& options) {
  const DatasetInfo& source = input.info();
  if (options.value_type == ValueType::kUInt8 && source.value_type != ValueType::kUInt8) {
    throw std::invalid_argument(
        "8-bit grids need 8-bit input; 16-bit grey values would be quantised");
  }
  initializeVdb();
  DatasetInfo info = source;
  info.value_type = options.value_type.value_or(source.value_type);
  info.value_mapping = options.value_mapping.value_or(source.value_mapping);
  info.active_voxel_count = 0;
  info.levels.assign(1, source.levels.front());
  const LevelInfo& level0 = info.levels.front();

  // A run with a resume key leaves a journal; a later run of the same processing of the same
  // input resumes it and skips the finished bricks (ADR 0034).
  nlohmann::json key;
  if (!options.resume_key.empty()) {
    key = {{"dims", source.dims},
           {"brick_size", source.brick_size},
           {"bricks", level0.bricks.size()},
           {"active_voxel_count", source.active_voxel_count},
           {"value_type", std::string(valueTypeName(info.value_type))},
           {"processing", options.resume_key}};
  }
  std::vector<std::pair<Index3, std::int64_t>> finished;
  if (std::filesystem::exists(dir)) {
    removeStalePartials(dir);
  }
  if (std::filesystem::exists(dir) && !std::filesystem::is_empty(dir)) {
    std::ifstream head_in(dir / kDerivedJournalFile);
    const nlohmann::json head =
        head_in ? nlohmann::json::parse(head_in, nullptr, /*allow_exceptions=*/false)
                : nlohmann::json();
    const bool journal = !key.is_null() && !std::filesystem::exists(dir / "index.json") &&
                         head.is_object() && head.value("format", "") == "voxelsieve-derived";
    if (!journal) {
      throw std::invalid_argument("Output directory is not empty: " + dir.string());
    }
    if (head.at("key") != key) {
      throw std::invalid_argument("The output directory " + dir.string() +
                                  " holds an interrupted run of other processing or of another "
                                  "input; remove it to start again");
    }
    finished = readBrickLog(dir / kBricksLog);
  } else if (!key.is_null()) {
    std::filesystem::create_directories(dir);
    writeJson(dir / kDerivedJournalFile,
              {{"format", "voxelsieve-derived"}, {"version", kJournalVersion}, {"key", key}});
  }

  std::optional<TelemetryPhase> phase(std::in_place, "bricks");
  std::filesystem::create_directories(brickPath(dir, 0, {0, 0, 0}).parent_path());
  std::vector<Index3> kept;
  std::vector<Index3> todo;
  for (const Index3& brick : level0.bricks) {
    const auto it =
        std::lower_bound(finished.begin(), finished.end(), std::pair{brick, std::int64_t{0}},
                         [](const auto& a, const auto& b) { return a.first < b.first; });
    if (it != finished.end() && it->first == brick) {
      if (it->second > 0) {
        kept.push_back(brick);
        info.active_voxel_count += it->second;
      }
      if (options.finished) {
        options.finished(brick, input.brickBox(0, brick));
      }
    } else {
      todo.push_back(brick);
    }
  }
  // Along a Morton curve: processing that reads a halo finds the neighbours in the cache.
  detail::mortonSort(todo);
  ProgressCounter progress(options.progress, "bricks", todo.size());
  std::optional<BrickLog> log;
  if (!key.is_null()) {
    log.emplace(dir / kBricksLog);
  }

  // `values` runs for one brick after the other; building, compressing and writing the bricks
  // runs in parallel behind it, as many as the work memory holds (X8).
  struct Item {
    Index3 brick{};
    Box box;
    std::shared_ptr<std::vector<float>> grey;
  };
  detail::WorkPool<std::vector<float>> pool;
  const auto brick_voxels =
      static_cast<std::uint64_t>(source.brick_size * source.brick_size * source.brick_size);
  std::mutex mutex;
  std::size_t next = 0;
  tbb::parallel_pipeline(
      tasksInMemory(
          brick_voxels *
          (sizeof(float) + 2 * std::uint64_t{info.value_type == ValueType::kFloat ? 4U : 2U})),
      tbb::make_filter<void, Item>(
          tbb::filter_mode::serial_in_order,
          [&](tbb::flow_control& control) -> Item {
            if (next == todo.size()) {
              control.stop();
              return {};
            }
            Item item;
            item.brick = todo[next++];
            item.box = input.brickBox(0, item.brick);
            auto taken = pool.take();
            item.grey = std::shared_ptr<std::vector<float>>(taken.release(), taken.get_deleter());
            // Zeroed like a new vector; reused memory costs no page faults.
            item.grey->assign(static_cast<std::size_t>(item.box.voxelCount()), 0.0F);
            values(item.brick, item.box, *item.grey);
            if (item.grey->size() != static_cast<std::size_t>(item.box.voxelCount())) {
              throw std::logic_error("Derived brick values do not match the brick");
            }
            return item;
          }) &
          tbb::make_filter<Item, void>(tbb::filter_mode::parallel, [&](const Item& item) {
            // In a context of its own, isolated, like the bricks of writeDataset: a failure
            // elsewhere must not cancel OpenVDB's loops halfway through this brick.
            tbb::task_group_context context(tbb::task_group_context::isolated);
            tbb::task_group group(context);
            tbb::this_task_arena::isolate([&] {
              group.run_and_wait([&] {
                const GreyGrid out = derivedBrick(*input.brick(0, item.brick), info.value_type,
                                                  item.box, *item.grey);
                const auto active = static_cast<std::int64_t>(out.base().activeVoxelCount());
                if (active > 0) {
                  setGridProperties(out.base(), 0, info.voxel_size);
                  out.base().insertMeta("voxelsieve_threshold",
                                        openvdb::FloatMetadata(info.threshold));
                  writeBrick(brickPath(dir, 0, item.brick), out);
                }
                if (log) {
                  log->add(item.brick, active);
                }
                {
                  const std::scoped_lock lock(mutex);
                  if (active > 0) {
                    kept.push_back(item.brick);
                    info.active_voxel_count += active;
                  }
                }
                progress.step();
              });
            });
          }));
  log.reset();
  std::sort(kept.begin(), kept.end());
  info.levels.front().bricks = std::move(kept);
  phase.reset();
  writeCoarseLevels(dir, info, options.progress);
  for (const std::string_view name : {kDerivedJournalFile, kBricksLog}) {
    std::error_code ignored;
    std::filesystem::remove(dir / name, ignored);
  }
  return info;
}
namespace {

DatasetInfo datasetInfoFromJson(const nlohmann::json& json, const std::filesystem::path& dir) {
  if (json.at("format") != "voxelsieve-dataset" || json.at("version") < 1 ||
      json.at("version") > kFormatVersion) {
    throw std::runtime_error("Unsupported dataset format in " + dir.string());
  }
  DatasetInfo info;
  info.dims = json.at("dims").get<Index3>();
  checkVolumeDims(info.dims, "dims");
  info.voxel_size = readVoxelSize(json);
  info.brick_size = json.at("brick_size").get<std::int64_t>();
  if (info.brick_size < 8 || info.brick_size % 8 != 0 || info.brick_size > kMaxVolumeEdge) {
    throw std::invalid_argument("brick_size " + std::to_string(info.brick_size));
  }
  info.value_type = parseValueType(json.value("value_type", "float"));
  info.threshold = json.at("threshold").get<float>();
  info.air_level = json.at("air_level").get<float>();
  info.margin_voxels = json.at("margin_voxels").get<int>();
  info.min_material_voxels = json.value("min_material_voxels", 1);  // absent before 0.2
  info.active_voxel_count = json.at("active_voxel_count").get<std::int64_t>();
  if (json.contains("outside_air_axes")) {  // only when not all
    info.outside_air_axes = parseAirAxes(json.at("outside_air_axes").get<std::string>());
  }
  if (json.contains("value_mapping")) {  // float scans only
    info.value_mapping = {json.at("value_mapping").at("offset").get<double>(),
                          json.at("value_mapping").at("scale").get<double>()};
  }
  for (const auto& level : json.at("levels")) {
    info.levels.push_back({level.at("level").get<int>(), level.at("dims").get<Index3>(),
                           level.at("voxel_size_mm").get<VoxelSize>(),
                           level.at("bricks").get<std::vector<Index3>>()});
    const LevelInfo& read = info.levels.back();
    checkVolumeDims(read.dims, "level " + std::to_string(read.level));
    const Index3 bricks = ceilDiv3(read.dims, info.brick_size);
    for (const Index3& brick : read.bricks) {
      for (std::size_t axis = 0; axis < 3; ++axis) {
        if (brick[axis] < 0 || brick[axis] >= bricks[axis]) {
          throw std::invalid_argument("a brick outside level " + std::to_string(read.level));
        }
      }
    }
  }
  if (info.levels.empty()) {
    throw std::invalid_argument("no levels");
  }
  return info;
}

}  // namespace

DatasetInfo readDatasetInfo(const std::filesystem::path& dir) {
  std::ifstream in(dir / "index.json");
  if (!in) {
    throw std::runtime_error("No index.json in " + dir.string());
  }
  // Whatever is wrong with the file, the message names it: a key that is missing or a number out
  // of range would otherwise surface as an error of the JSON library or of an allocation.
  try {
    return datasetInfoFromJson(nlohmann::json::parse(in), dir);
  } catch (const nlohmann::json::exception& error) {
    throw std::runtime_error("Damaged dataset: " + (dir / "index.json").string() + " (" +
                             error.what() + ")");
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error("Damaged dataset: " + (dir / "index.json").string() + " (" +
                             error.what() + ")");
  }
}

}  // namespace voxelsieve
