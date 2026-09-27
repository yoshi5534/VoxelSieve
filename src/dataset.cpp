#include "voxelsieve/dataset.hpp"

#include <openvdb/io/File.h>
#include <openvdb/tools/Morphology.h>
#include <openvdb/tree/LeafManager.h>
#include <tbb/blocked_range.h>
#include <tbb/combinable.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include "detail/blocks.hpp"
#include "voxelsieve/sieve.hpp"

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

// ---------------------------------------------------------------------------------------------
// Pass 1: histogram and maximum grey value per 8^3 block.

struct BlockStatistics {
  Histogram histogram;
  std::vector<std::uint16_t> block_max;
  Index3 block_dims{};
};

BlockStatistics collectBlockStatistics(const VolumeSource& source) {
  constexpr std::int64_t kB = kBlockSize;
  const Index3 dims = source.dims();
  BlockStatistics stats;
  stats.block_dims = ceilDiv3(dims, kB);
  stats.block_max.assign(product(stats.block_dims), 0);

  tbb::combinable<Histogram> histograms([] { return Histogram(kHistogramBins, 0); });
  const std::int64_t rows = stats.block_dims[1] * stats.block_dims[2];
  // One task per row of blocks: an 8 x 8 x dims[0] region, so memory per task stays small.
  tbb::parallel_for(tbb::blocked_range<std::int64_t>(0, rows), [&](const auto& range) {
    Histogram& histogram = histograms.local();
    std::vector<std::uint16_t> buffer;
    for (std::int64_t row = range.begin(); row != range.end(); ++row) {
      const std::int64_t by = row % stats.block_dims[1];
      const std::int64_t bz = row / stats.block_dims[1];
      const Box box{{0, by * kB, bz * kB},
                    {dims[0], std::min((by + 1) * kB, dims[1]), std::min((bz + 1) * kB, dims[2])}};
      buffer.resize(static_cast<std::size_t>(box.voxelCount()));
      source.readRegion(box, buffer);
      std::uint16_t* block_max = &stats.block_max[static_cast<std::size_t>(
          stats.block_dims[0] * (by + stats.block_dims[1] * bz))];
      std::size_t offset = 0;
      for (std::int64_t z = 0; z < box.size(2); ++z) {
        for (std::int64_t y = 0; y < box.size(1); ++y) {
          for (std::int64_t x = 0; x < dims[0]; ++x) {
            const std::uint16_t value = buffer[offset++];
            ++histogram[value];
            std::uint16_t& current = block_max[x / kB];
            current = std::max(current, value);
          }
        }
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

BlockGrid classifyBlocks(const BlockStatistics& stats, float threshold) {
  BlockGrid blocks;
  blocks.dims = stats.block_dims;
  blocks.states.resize(stats.block_max.size());
  std::transform(stats.block_max.begin(), stats.block_max.end(), blocks.states.begin(),
                 [threshold](std::uint16_t max) {
                   return static_cast<float>(max) > threshold ? BlockState::kMaterial
                                                              : BlockState::kAir;
                 });
  detail::floodFillOutsideAir(blocks);
  return blocks;
}

// ---------------------------------------------------------------------------------------------
// Pass 2: level-0 bricks.

void setGridProperties(openvdb::FloatGrid& grid, int level, double voxel_size_mm) {
  const auto scale = static_cast<double>(std::int64_t{1} << level);
  auto transform = openvdb::math::Transform::createLinearTransform(voxel_size_mm * scale);
  // Level-L voxel i covers level-0 voxels [i * 2^L, (i + 1) * 2^L); centre it on them.
  transform->postTranslate(openvdb::Vec3d((scale - 1.0) / 2.0 * voxel_size_mm));
  grid.setTransform(transform);
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
  auto sum = openvdb::FloatGrid::create(0.0F);
  auto count = openvdb::FloatGrid::create(0.0F);
  auto sum_acc = sum->getAccessor();
  auto count_acc = count->getAccessor();
  for (const auto& path : children) {
    const auto child = readBrick(path, /*delay_load=*/false);
    for (auto it = child->cbeginValueOn(); it; ++it) {
      const openvdb::Coord c = it.getCoord();
      const openvdb::Coord parent(c.x() >> 1, c.y() >> 1, c.z() >> 1);
      sum_acc.setValue(parent, sum_acc.getValue(parent) + *it);
      count_acc.setValue(parent, count_acc.getValue(parent) + 1.0F);
    }
  }
  for (auto it = sum->beginValueOn(); it; ++it) {
    it.setValue(*it / count_acc.getValue(it.getCoord()));
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
                      {"voxel_size_mm", level.voxel_size_mm},
                      {"bricks", level.bricks}});
  }
  return {{"format", "voxelsieve-dataset"},
          {"version", kFormatVersion},
          {"value_type", "float"},
          {"dims", info.dims},
          {"voxel_size_mm", info.voxel_size_mm},
          {"brick_size", info.brick_size},
          {"threshold", info.threshold},
          {"air_level", info.air_level},
          {"margin_voxels", info.margin_voxels},
          {"active_voxel_count", info.active_voxel_count},
          {"overview", "overview.vdb"},
          {"levels", levels}};
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

DatasetInfo writeDataset(const VolumeSource& source, const std::filesystem::path& dir,
                         const DatasetOptions& options) {
  if (options.brick_size <= 0 || options.brick_size % kBlockSize != 0) {
    throw std::invalid_argument("brick_size must be a positive multiple of 8");
  }
  if (options.margin_voxels < 0) {
    throw std::invalid_argument("margin_voxels must be >= 0");
  }
  if (std::filesystem::exists(dir) && !std::filesystem::is_empty(dir)) {
    throw std::invalid_argument("Output directory is not empty: " + dir.string());
  }
  openvdb::initialize();

  DatasetInfo info;
  info.dims = source.dims();
  info.voxel_size_mm = source.voxelSizeMm();
  info.brick_size = options.brick_size;
  info.margin_voxels = options.margin_voxels;

  const BlockStatistics stats = collectBlockStatistics(source);
  const detail::ThresholdResult estimate = detail::otsuThreshold(stats.histogram);
  info.threshold = options.threshold.value_or(estimate.threshold);
  info.air_level = estimate.air_level;
  const BlockGrid blocks = classifyBlocks(stats, info.threshold);

  // Level 0.
  LevelInfo level0{0, info.dims, info.voxel_size_mm, {}};
  const Index3 brick_dims = ceilDiv3(info.dims, options.brick_size);
  std::filesystem::create_directories(brickPath(dir, 0, {0, 0, 0}).parent_path());
  std::mutex mutex;
  tbb::parallel_for(std::size_t{0}, product(brick_dims), [&](std::size_t i) {
    const Index3 brick = unravel(i, brick_dims);
    auto grid = buildBrick(source, blocks, brick, options);
    if (!grid) {
      return;
    }
    setGridProperties(*grid, 0, info.voxel_size_mm);
    grid->insertMeta("voxelsieve_threshold", openvdb::FloatMetadata(info.threshold));
    writeBrick(brickPath(dir, 0, brick), grid);
    const std::scoped_lock lock(mutex);
    level0.bricks.push_back(brick);
    info.active_voxel_count += static_cast<std::int64_t>(grid->activeVoxelCount());
  });
  std::sort(level0.bricks.begin(), level0.bricks.end());
  info.levels.push_back(std::move(level0));

  // Coarser levels until one brick holds the whole volume.
  while (true) {
    const LevelInfo& finer = info.levels.back();
    if (std::all_of(finer.dims.begin(), finer.dims.end(),
                    [&](std::int64_t d) { return d <= options.brick_size; })) {
      break;
    }
    LevelInfo coarser{finer.level + 1, ceilDiv3(finer.dims, 2), finer.voxel_size_mm * 2.0, {}};
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
      setGridProperties(*grid, coarser.level, info.voxel_size_mm);
      writeBrick(brickPath(dir, coarser.level, brick), grid);
      const std::scoped_lock lock(mutex);
      coarser.bricks.push_back(brick);
    });
    std::sort(coarser.bricks.begin(), coarser.bricks.end());
    info.levels.push_back(std::move(coarser));
  }

  // Overview: the single brick of the coarsest level, or an empty grid for an empty dataset.
  const LevelInfo& top = info.levels.back();
  if (top.bricks.empty()) {
    auto empty = openvdb::FloatGrid::create(0.0F);
    setGridProperties(*empty, top.level, info.voxel_size_mm);
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
  info.voxel_size_mm = json.at("voxel_size_mm").get<double>();
  info.brick_size = json.at("brick_size").get<std::int64_t>();
  info.threshold = json.at("threshold").get<float>();
  info.air_level = json.at("air_level").get<float>();
  info.margin_voxels = json.at("margin_voxels").get<int>();
  info.active_voxel_count = json.at("active_voxel_count").get<std::int64_t>();
  for (const auto& level : json.at("levels")) {
    info.levels.push_back({level.at("level").get<int>(), level.at("dims").get<Index3>(),
                           level.at("voxel_size_mm").get<double>(),
                           level.at("bricks").get<std::vector<Index3>>()});
  }
  return info;
}

}  // namespace voxelsieve
