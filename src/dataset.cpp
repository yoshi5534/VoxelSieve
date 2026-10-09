#include "voxelsieve/dataset.hpp"

#include <openvdb/io/File.h>
#include <openvdb/tools/Morphology.h>
#include <openvdb/tree/LeafManager.h>
#include <tbb/blocked_range.h>
#include <tbb/combinable.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <boost/iostreams/device/mapped_file.hpp>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>

#include "detail/blocks.hpp"
#include "detail/pages.hpp"
#include "detail/preview.hpp"
#include "detail/transform.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {
namespace {

using detail::BlockGrid;
using detail::BlockState;
using detail::ceilDiv;
using detail::Histogram;
using detail::kHistogramBins;
using Index3 = std::array<std::int64_t, 3>;

constexpr int kFormatVersion = 1;

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

/// Reports the fraction of `total` steps done through DatasetOptions::progress, at most once per
/// whole percent, from any thread.
class ProgressCounter {
 public:
  ProgressCounter(const DatasetOptions& options, std::string_view stage, std::size_t total)
      : callback_(options.progress), stage_(stage), total_(total) {
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

/// A temporary raw copy of a source, removed on destruction. Slices are copied in two rounds: the
/// slices of the preview first, then the rest, so the preview costs no extra reading.
class StagedSource {
 public:
  StagedSource(const VolumeSource& source, const std::filesystem::path& dir,
               const DatasetOptions& options)
      : input_(source),
        dims_(source.dims()),
        copied_(static_cast<std::size_t>(dims_[2]), 0),
        progress_(options, "staging", static_cast<std::size_t>(dims_[2])) {
    const std::uint64_t bytes = product(dims_) * sizeof(std::uint16_t);
    std::filesystem::create_directories(dir);
    const std::uint64_t available = std::filesystem::space(dir).available;
    if (available < bytes) {
      constexpr double kGb = 1024.0 * 1024.0 * 1024.0;
      throw std::runtime_error(
          "Staging the input needs " + std::to_string(static_cast<double>(bytes) / kGb) +
          " GB in " + dir.string() + ", but only " +
          std::to_string(static_cast<double>(available) / kGb) +
          " GB are free; choose another staging directory or turn staging off");
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = dir / (".voxelsieve-staging-" + std::to_string(stamp) + ".raw");
    try {
      boost::iostreams::mapped_file_params params(path_.string());
      params.flags = boost::iostreams::mapped_file::readwrite;
      params.new_file_size = static_cast<boost::iostreams::stream_offset>(bytes);
      file_.open(params);
    } catch (...) {
      remove();
      throw;
    }
  }
  StagedSource(const StagedSource&) = delete;
  StagedSource& operator=(const StagedSource&) = delete;
  StagedSource(StagedSource&&) = delete;
  StagedSource& operator=(StagedSource&&) = delete;
  ~StagedSource() { remove(); }

  /// Copies the slices of the preview and hands each to `preview` while it is still in memory.
  void copyPreview(detail::PreviewBuilder& preview) {
    const auto& slices = preview.slices();
    copy(
        slices.size(), [&slices](std::size_t k) { return slices[k]; },
        [&preview](std::size_t k, const std::uint16_t* slice) { preview.add(k, slice); });
  }

  /// Copies the slices not copied yet; the copy is then what `source` reads.
  void finish() {
    copy(
        static_cast<std::size_t>(dims_[2]),
        [](std::size_t z) { return static_cast<std::int64_t>(z); },
        [](std::size_t, const std::uint16_t*) {});
    file_.close();
    raw_ = std::make_unique<MappedRawSource>(
        path_, RawLayout{dims_, input_.voxelSize(), SampleType::kUInt16, std::endian::native,
                         std::uint64_t{0}});
  }

  [[nodiscard]] const VolumeSource& source() const { return *raw_; }

 private:
  /// Copies slices `slice(0)` to `slice(count - 1)` that were not copied before, calling
  /// `visit(k, data)` for each before its pages are let go.
  template <typename Slice, typename Visit>
  void copy(std::size_t count, const Slice& slice, const Visit& visit) {
    auto* data = reinterpret_cast<std::uint16_t*>(file_.data());
    const auto slice_voxels = static_cast<std::size_t>(dims_[0] * dims_[1]);
    // One slice per task: each slice is decoded exactly once, all cores in parallel.
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, count, 1), [&](const auto& range) {
      for (std::size_t k = range.begin(); k != range.end(); ++k) {
        const std::int64_t z = slice(k);
        if (copied_[static_cast<std::size_t>(z)] != 0) {
          continue;
        }
        std::uint16_t* target = data + static_cast<std::size_t>(z) * slice_voxels;
        input_.readRegion(Box{{0, 0, z}, {dims_[0], dims_[1], z + 1}}, {target, slice_voxels});
        visit(k, target);
        // The slice is written; its pages go to the file cache instead of staying in the
        // process's memory (Windows kept the whole copy in the working set).
        detail::releaseMappedPages(target, slice_voxels * sizeof(std::uint16_t));
        copied_[static_cast<std::size_t>(z)] = 1;
        progress_.step();
      }
    });
  }

  void remove() {
    raw_.reset();
    if (file_.is_open()) {
      file_.close();
    }
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  const VolumeSource& input_;
  Index3 dims_;
  // One flag per slice; each is written by the one task that copies the slice.
  std::vector<std::uint8_t> copied_;
  ProgressCounter progress_;
  std::filesystem::path path_;
  boost::iostreams::mapped_file file_;
  std::unique_ptr<MappedRawSource> raw_;
};

// ---------------------------------------------------------------------------------------------
// Pass 1: histogram and the k-th largest grey value per 8^3 block, k = min_material_voxels.
// A block holds at least k voxels above the threshold exactly when its k-th largest value is
// above it, so the threshold may still be chosen after the pass and the result matches the
// voxel count of the in-memory sieve. Only one value per block is kept (2 bytes); the k largest
// values live in per-task heaps for one row of blocks.

struct BlockStatistics {
  Histogram histogram;
  std::vector<std::uint16_t> block_kth;
  Index3 block_dims{};
};

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
  ProgressCounter progress(options, "histogram", static_cast<std::size_t>(rows));
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

void setGridProperties(openvdb::FloatGrid& grid, int level, const VoxelSize& voxel_size) {
  grid.setTransform(detail::voxelTransform(voxel_size, level));
  grid.setGridClass(openvdb::GRID_FOG_VOLUME);
  grid.setName("density");
  grid.insertMeta("voxelsieve_level", openvdb::Int32Metadata(level));
}

/// Builds one level-0 brick, or returns nullptr when the sieve keeps nothing inside it.
openvdb::FloatGrid::Ptr buildBrick(const VolumeSource& source, const BlockGrid& blocks,
                                   const Index3& brick, const DatasetOptions& options) {
  const Index3 dims = source.dims();
  const std::int64_t blocks_per_brick = options.brick_size / kBlockSize;
  const std::int64_t halo = ceilDiv(options.margin_voxels, kBlockSize);

  // Kept blocks in the brick plus a halo, so the margin can grow in from neighbouring bricks.
  auto grid = openvdb::FloatGrid::create(0.0F);
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
        tree.fill(openvdb::CoordBBox(min, max), 0.0F, /*active=*/true);
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

  std::vector<std::uint16_t> values(static_cast<std::size_t>(box.voxelCount()));
  source.readRegion(box, values);
  openvdb::tree::LeafManager<openvdb::FloatTree> leaves(tree);
  leaves.foreach (
      [&](openvdb::FloatTree::LeafNodeType& leaf, std::size_t) {
        for (auto it = leaf.beginValueOn(); it; ++it) {
          const openvdb::Coord c = it.getCoord();
          const auto index = static_cast<std::size_t>(
              (c.x() - box.min[0]) +
              box.size(0) * ((c.y() - box.min[1]) + box.size(1) * (c.z() - box.min[2])));
          it.setValue(static_cast<float>(values[index]));
        }
      },
      /*threaded=*/false);
  return grid;
}

// ---------------------------------------------------------------------------------------------
// Coarser levels.

/// Mean of the active children of each voxel, from up to 2x2x2 bricks of the finer level.
/// Children are loaded one at a time to bound memory.
openvdb::FloatGrid::Ptr downsample(const std::vector<std::filesystem::path>& children) {
  using Leaf = openvdb::FloatTree::LeafNodeType;
  auto sum = openvdb::FloatGrid::create(0.0F);
  auto count = openvdb::FloatGrid::create(0.0F);
  auto sum_acc = sum->getAccessor();
  auto count_acc = count->getAccessor();
  for (const auto& path : children) {
    const auto child = readBrick(path, /*delay_load=*/false);
    child->tree().voxelizeActiveTiles();
    // A leaf of 8^3 voxels falls into one octant of a parent leaf, so each child leaf looks up
    // its two parent leaves once and then adds voxel by voxel, in the same order as before.
    for (auto leaf = child->tree().cbeginLeaf(); leaf; ++leaf) {
      const openvdb::Coord origin = leaf->origin();
      const openvdb::Coord parent_origin(origin.x() >> 1, origin.y() >> 1, origin.z() >> 1);
      Leaf* sum_leaf = sum_acc.touchLeaf(parent_origin);
      Leaf* count_leaf = count_acc.touchLeaf(parent_origin);
      for (auto it = leaf->cbeginValueOn(); it; ++it) {
        const openvdb::Coord c = it.getCoord();
        const openvdb::Index n =
            Leaf::coordToOffset(openvdb::Coord(c.x() >> 1, c.y() >> 1, c.z() >> 1));
        sum_leaf->setValueOn(n, sum_leaf->getValue(n) + *it);
        count_leaf->setValueOn(n, count_leaf->getValue(n) + 1.0F);
      }
    }
  }
  for (auto leaf = sum->tree().beginLeaf(); leaf; ++leaf) {
    const Leaf* count_leaf = count_acc.probeConstLeaf(leaf->origin());
    for (auto it = leaf->beginValueOn(); it; ++it) {
      it.setValue(*it / count_leaf->getValue(it.pos()));
    }
  }
  return sum;
}

void writeBrick(const std::filesystem::path& path, const openvdb::FloatGrid::Ptr& grid) {
  writeVdb(path, {grid});
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
                         {"value_type", "float"},
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
  if (!info.value_mapping.isIdentity()) {
    json["value_mapping"] = {{"offset", info.value_mapping.offset},
                             {"scale", info.value_mapping.scale}};
  }
  return json;
}

}  // namespace

std::filesystem::path brickPath(const std::filesystem::path& dir, int level, const Index3& brick) {
  return dir / ("level" + std::to_string(level)) /
         (std::to_string(brick[0]) + "_" + std::to_string(brick[1]) + "_" +
          std::to_string(brick[2]) + ".vdb");
}

openvdb::FloatGrid::Ptr readBrick(const std::filesystem::path& file, bool delay_load) {
  openvdb::initialize();
  openvdb::io::File vdb(file.string());
  vdb.open(delay_load);
  auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(vdb.readGrid("density"));
  vdb.close();
  if (!grid) {
    throw std::runtime_error("No float grid 'density' in " + file.string());
  }
  return grid;
}

DatasetInfo writeDataset(const VolumeSource& input, const std::filesystem::path& dir,
                         const DatasetOptions& options) {
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
  if (std::filesystem::exists(dir) && !std::filesystem::is_empty(dir)) {
    throw std::invalid_argument("Output directory is not empty: " + dir.string());
  }
  openvdb::initialize();

  DatasetInfo info;
  info.dims = input.dims();
  info.voxel_size = input.voxelSize();
  info.voxel_size.validate();
  info.value_mapping = input.valueMapping();
  info.brick_size = options.brick_size;
  info.margin_voxels = options.margin_voxels;
  info.min_material_voxels = options.min_material_voxels;
  info.outside_air_axes = options.outside_air_axes;

  std::unique_ptr<StagedSource> staged;
  if (options.stage_slow_sources && input.slowRandomAccess()) {
    staged = std::make_unique<StagedSource>(
        input, options.staging_dir.empty() ? dir : options.staging_dir, options);
  }
  if (options.preview) {
    // The preview's slices are the first the staging copy reads (ADR 0020).
    std::optional<ImportPreview> preview;
    {
      const TelemetryPhase phase("preview");
      if (staged) {
        detail::PreviewBuilder builder(info.dims, info.voxel_size, info.value_mapping,
                                       options.preview_options);
        staged->copyPreview(builder);
        preview = builder.finish();
      } else {
        preview = readImportPreview(input, options.preview_options);
      }
    }
    options.preview(*preview);
  }
  if (staged) {
    const TelemetryPhase phase("staging");
    staged->finish();
    // The copy is all the passes read, so its pages should stay in memory, not the input's.
    input.releaseMemory();
  }
  const VolumeSource& source = staged ? staged->source() : input;

  std::optional<TelemetryPhase> phase(std::in_place, "pass 1 (histogram)");
  PageRelease rows_release(staged ? &source : nullptr, 1024);
  const BlockStatistics stats =
      collectBlockStatistics(source, options.min_material_voxels, options, rows_release);
  const detail::ThresholdResult estimate = detail::airThreshold(stats.histogram);
  info.threshold = options.threshold.value_or(estimate.threshold);
  info.air_level = estimate.air_level;
  const BlockGrid blocks = classifyBlocks(stats, info.threshold, options.outside_air_axes);

  // Level 0.
  phase.reset();
  phase.emplace("pass 2 (bricks)");
  LevelInfo level0{0, info.dims, info.voxel_size, {}};
  const Index3 brick_dims = ceilDiv3(info.dims, options.brick_size);
  std::filesystem::create_directories(brickPath(dir, 0, {0, 0, 0}).parent_path());
  std::mutex mutex;
  ProgressCounter brick_progress(options, "bricks", product(brick_dims));
  PageRelease bricks_release(staged ? &source : nullptr, 16);
  tbb::parallel_for(std::size_t{0}, product(brick_dims), [&](std::size_t i) {
    const Index3 brick = unravel(i, brick_dims);
    auto grid = buildBrick(source, blocks, brick, options);
    bricks_release.step();
    brick_progress.step();
    if (!grid) {
      return;
    }
    setGridProperties(*grid, 0, info.voxel_size);
    grid->insertMeta("voxelsieve_threshold", openvdb::FloatMetadata(info.threshold));
    writeBrick(brickPath(dir, 0, brick), grid);
    // Counted outside the lock: OpenVDB counts with TBB tasks, and a thread waiting for them may
    // run another brick of this loop, which would then block on the lock it holds.
    const auto active = static_cast<std::int64_t>(grid->activeVoxelCount());
    const std::scoped_lock lock(mutex);
    level0.bricks.push_back(brick);
    info.active_voxel_count += active;
  });
  std::sort(level0.bricks.begin(), level0.bricks.end());
  info.levels.push_back(std::move(level0));

  // Coarser levels until one brick holds the whole volume.
  int level_count = 1;  // levels needed in all, for the progress
  for (Index3 d = info.dims;
       std::any_of(d.begin(), d.end(), [&](std::int64_t n) { return n > options.brick_size; });
       d = ceilDiv3(d, 2)) {
    ++level_count;
  }
  phase.reset();
  phase.emplace("levels");
  ProgressCounter level_progress(options, "levels", static_cast<std::size_t>(level_count - 1));
  while (true) {
    const LevelInfo& finer = info.levels.back();
    if (std::all_of(finer.dims.begin(), finer.dims.end(),
                    [&](std::int64_t d) { return d <= options.brick_size; })) {
      break;
    }
    LevelInfo coarser{finer.level + 1, ceilDiv3(finer.dims, 2), finer.voxel_size.scaled(2.0), {}};
    const Index3 coarse_brick_dims = ceilDiv3(coarser.dims, options.brick_size);
    std::filesystem::create_directories(brickPath(dir, coarser.level, {0, 0, 0}).parent_path());
    tbb::parallel_for(std::size_t{0}, product(coarse_brick_dims), [&](std::size_t i) {
      const Index3 brick = unravel(i, coarse_brick_dims);
      std::vector<std::filesystem::path> children;
      for (const Index3& child : finer.bricks) {
        if (child[0] / 2 == brick[0] && child[1] / 2 == brick[1] && child[2] / 2 == brick[2]) {
          children.push_back(brickPath(dir, finer.level, child));
        }
      }
      if (children.empty()) {
        return;
      }
      auto grid = downsample(children);
      setGridProperties(*grid, coarser.level, info.voxel_size);
      writeBrick(brickPath(dir, coarser.level, brick), grid);
      const std::scoped_lock lock(mutex);
      coarser.bricks.push_back(brick);
    });
    std::sort(coarser.bricks.begin(), coarser.bricks.end());
    info.levels.push_back(std::move(coarser));
    level_progress.step();
  }

  // Overview: the single brick of the coarsest level, or an empty grid for an empty dataset.
  const LevelInfo& top = info.levels.back();
  if (top.bricks.empty()) {
    auto empty = openvdb::FloatGrid::create(0.0F);
    setGridProperties(*empty, top.level, info.voxel_size);
    writeBrick(dir / "overview.vdb", empty);
  } else {
    std::filesystem::copy_file(brickPath(dir, top.level, top.bricks.front()), dir / "overview.vdb");
  }

  std::ofstream index(dir / "index.json");
  index << toJson(info).dump(2) << '\n';
  if (!index) {
    throw std::runtime_error("Cannot write " + (dir / "index.json").string());
  }
  return info;
}

DatasetInfo readDatasetInfo(const std::filesystem::path& dir) {
  std::ifstream in(dir / "index.json");
  if (!in) {
    throw std::runtime_error("No index.json in " + dir.string());
  }
  const auto json = nlohmann::json::parse(in);
  if (json.at("format") != "voxelsieve-dataset" || json.at("version") != kFormatVersion) {
    throw std::runtime_error("Unsupported dataset format in " + dir.string());
  }
  DatasetInfo info;
  info.dims = json.at("dims").get<Index3>();
  info.voxel_size = readVoxelSize(json);
  info.brick_size = json.at("brick_size").get<std::int64_t>();
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
  }
  return info;
}

}  // namespace voxelsieve
