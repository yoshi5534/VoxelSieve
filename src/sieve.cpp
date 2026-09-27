#include "voxelsieve/sieve.hpp"

#include <openvdb/io/File.h>
#include <openvdb/tools/Morphology.h>
#include <openvdb/tree/LeafManager.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <stdexcept>
#include <vector>

namespace voxelsieve {
namespace {

enum class BlockState : std::uint8_t { kAir, kMaterial, kOutsideAir };

struct BlockGrid {
  std::array<std::int64_t, 3> dims{};
  std::vector<BlockState> states;

  [[nodiscard]] std::size_t index(std::int64_t bx, std::int64_t by, std::int64_t bz) const {
    return static_cast<std::size_t>(bx + dims[0] * (by + dims[1] * bz));
  }
};

std::int64_t ceilDiv(std::int64_t a, std::int64_t b) { return (a + b - 1) / b; }

struct ThresholdResult {
  float threshold = 0.0F;
  float air_level = 0.0F;
};

ThresholdResult otsu(const Volume16& volume, int stride) {
  if (stride < 1) {
    throw std::invalid_argument("histogram_stride must be >= 1");
  }
  std::vector<std::uint64_t> histogram(65536, 0);
  std::uint64_t total = 0;
  for (std::int64_t z = 0; z < volume.dims[2]; z += stride) {
    for (std::int64_t y = 0; y < volume.dims[1]; y += stride) {
      for (std::int64_t x = 0; x < volume.dims[0]; x += stride) {
        ++histogram[volume.at(x, y, z)];
        ++total;
      }
    }
  }
  if (total == 0) {
    throw std::invalid_argument("Cannot estimate a threshold for an empty volume");
  }

  double sum_all = 0.0;
  for (std::size_t value = 0; value < histogram.size(); ++value) {
    sum_all += static_cast<double>(value) * static_cast<double>(histogram[value]);
  }
  // Maximise the between-class variance; class "below" holds values <= t.
  double best_variance = -1.0;
  std::size_t best_value = 0;
  double weight_below = 0.0;
  double sum_below = 0.0;
  for (std::size_t value = 0; value + 1 < histogram.size(); ++value) {
    weight_below += static_cast<double>(histogram[value]);
    sum_below += static_cast<double>(value) * static_cast<double>(histogram[value]);
    const double weight_above = static_cast<double>(total) - weight_below;
    if (weight_below == 0.0 || weight_above == 0.0) {
      continue;
    }
    const double mean_below = sum_below / weight_below;
    const double mean_above = (sum_all - sum_below) / weight_above;
    const double variance =
        weight_below * weight_above * (mean_below - mean_above) * (mean_below - mean_above);
    if (variance > best_variance) {
      best_variance = variance;
      best_value = value;
    }
  }

  double air_weight = 0.0;
  double air_sum = 0.0;
  for (std::size_t value = 0; value <= best_value; ++value) {
    air_weight += static_cast<double>(histogram[value]);
    air_sum += static_cast<double>(value) * static_cast<double>(histogram[value]);
  }
  // Values <= best_value are air, so the separating grey value lies half a step above.
  return {static_cast<float>(static_cast<double>(best_value) + 0.5),
          static_cast<float>(air_weight > 0.0 ? air_sum / air_weight : 0.0)};
}

BlockGrid classifyBlocks(const Volume16& volume, float threshold, int min_material_voxels) {
  BlockGrid blocks;
  for (std::size_t i = 0; i < 3; ++i) {
    blocks.dims[i] = ceilDiv(volume.dims[i], kBlockSize);
  }
  blocks.states.assign(static_cast<std::size_t>(blocks.dims[0] * blocks.dims[1] * blocks.dims[2]),
                       BlockState::kAir);

  // One task per slab of 8 slices, so each task reads a contiguous part of the volume.
  tbb::parallel_for(tbb::blocked_range<std::int64_t>(0, blocks.dims[2]), [&](const auto& range) {
    for (std::int64_t bz = range.begin(); bz != range.end(); ++bz) {
      const std::int64_t z_end = std::min((bz + 1) * kBlockSize, volume.dims[2]);
      std::vector<int> counts(static_cast<std::size_t>(blocks.dims[0] * blocks.dims[1]), 0);
      for (std::int64_t z = bz * kBlockSize; z < z_end; ++z) {
        for (std::int64_t y = 0; y < volume.dims[1]; ++y) {
          const std::int64_t row = (y / kBlockSize) * blocks.dims[0];
          for (std::int64_t x = 0; x < volume.dims[0]; ++x) {
            if (static_cast<float>(volume.at(x, y, z)) > threshold) {
              ++counts[static_cast<std::size_t>(row + x / kBlockSize)];
            }
          }
        }
      }
      for (std::int64_t by = 0; by < blocks.dims[1]; ++by) {
        for (std::int64_t bx = 0; bx < blocks.dims[0]; ++bx) {
          if (counts[static_cast<std::size_t>(bx + by * blocks.dims[0])] >= min_material_voxels) {
            blocks.states[blocks.index(bx, by, bz)] = BlockState::kMaterial;
          }
        }
      }
    }
  });
  return blocks;
}

/// Marks air blocks that are face-connected to the volume boundary as outside air.
void floodFillOutsideAir(BlockGrid& blocks) {
  std::deque<std::array<std::int64_t, 3>> queue;
  const auto visit = [&](std::int64_t bx, std::int64_t by, std::int64_t bz) {
    if (bx < 0 || by < 0 || bz < 0 || bx >= blocks.dims[0] || by >= blocks.dims[1] ||
        bz >= blocks.dims[2]) {
      return;
    }
    BlockState& state = blocks.states[blocks.index(bx, by, bz)];
    if (state == BlockState::kAir) {
      state = BlockState::kOutsideAir;
      queue.push_back({bx, by, bz});
    }
  };

  const auto& d = blocks.dims;
  for (std::int64_t a = 0; a < d[0]; ++a) {
    for (std::int64_t b = 0; b < d[1]; ++b) {
      visit(a, b, 0);
      visit(a, b, d[2] - 1);
    }
  }
  for (std::int64_t a = 0; a < d[0]; ++a) {
    for (std::int64_t c = 0; c < d[2]; ++c) {
      visit(a, 0, c);
      visit(a, d[1] - 1, c);
    }
  }
  for (std::int64_t b = 0; b < d[1]; ++b) {
    for (std::int64_t c = 0; c < d[2]; ++c) {
      visit(0, b, c);
      visit(d[0] - 1, b, c);
    }
  }

  while (!queue.empty()) {
    const auto [bx, by, bz] = queue.front();
    queue.pop_front();
    visit(bx - 1, by, bz);
    visit(bx + 1, by, bz);
    visit(bx, by - 1, bz);
    visit(bx, by + 1, bz);
    visit(bx, by, bz - 1);
    visit(bx, by, bz + 1);
  }
}

openvdb::Coord toCoord(std::int64_t x, std::int64_t y, std::int64_t z) {
  return {static_cast<openvdb::Int32>(x), static_cast<openvdb::Int32>(y),
          static_cast<openvdb::Int32>(z)};
}

}  // namespace

float estimateThreshold(const Volume16& volume, int stride) {
  return otsu(volume, stride).threshold;
}

SieveResult sieve(const Volume16& volume, const SieveOptions& options) {
  if (options.margin_voxels < 0) {
    throw std::invalid_argument("margin_voxels must be >= 0");
  }
  openvdb::initialize();
  SieveResult result;
  SieveStats& stats = result.stats;

  const ThresholdResult estimate = otsu(volume, options.histogram_stride);
  stats.threshold = options.threshold.value_or(estimate.threshold);
  stats.air_level = estimate.air_level;

  BlockGrid blocks = classifyBlocks(volume, stats.threshold, options.min_material_voxels);
  floodFillOutsideAir(blocks);

  // Topology: every block that is not outside air, as active tiles, then the air margin.
  auto grid = openvdb::FloatGrid::create(0.0F);
  auto& tree = grid->tree();
  const openvdb::Coord volume_max =
      toCoord(volume.dims[0] - 1, volume.dims[1] - 1, volume.dims[2] - 1);
  for (std::int64_t bz = 0; bz < blocks.dims[2]; ++bz) {
    for (std::int64_t by = 0; by < blocks.dims[1]; ++by) {
      for (std::int64_t bx = 0; bx < blocks.dims[0]; ++bx) {
        const BlockState state = blocks.states[blocks.index(bx, by, bz)];
        stats.material_block_count += state == BlockState::kMaterial ? 1 : 0;
        stats.outside_air_block_count += state == BlockState::kOutsideAir ? 1 : 0;
        if (state == BlockState::kOutsideAir) {
          continue;
        }
        const openvdb::Coord min = toCoord(bx * kBlockSize, by * kBlockSize, bz * kBlockSize);
        const openvdb::Coord max = openvdb::Coord::minComponent(
            min.offsetBy(static_cast<openvdb::Int32>(kBlockSize - 1)), volume_max);
        tree.fill(openvdb::CoordBBox(min, max), 0.0F, /*active=*/true);
      }
    }
  }
  if (options.margin_voxels > 0) {
    openvdb::tools::dilateActiveValues(tree, options.margin_voxels, openvdb::tools::NN_FACE_EDGE,
                                       openvdb::tools::EXPAND_TILES);
  }
  tree.voxelizeActiveTiles();

  // Values: copy grey values into active voxels and drop voxels the margin pushed past the volume.
  openvdb::tree::LeafManager<openvdb::FloatTree> leaves(tree);
  leaves.foreach ([&](openvdb::FloatTree::LeafNodeType& leaf, std::size_t) {
    for (auto it = leaf.beginValueOn(); it; ++it) {
      const openvdb::Coord c = it.getCoord();
      if (c.x() < 0 || c.y() < 0 || c.z() < 0 || c.x() > volume_max.x() || c.y() > volume_max.y() ||
          c.z() > volume_max.z()) {
        it.setValueOff();
        continue;
      }
      it.setValue(static_cast<float>(volume.at(c.x(), c.y(), c.z())));
    }
  });
  tree.prune();

  grid->setTransform(openvdb::math::Transform::createLinearTransform(volume.voxel_size_mm));
  grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  grid->setName("density");
  grid->insertMeta("voxelsieve_threshold", openvdb::FloatMetadata(stats.threshold));
  grid->insertMeta("voxelsieve_air_level", openvdb::FloatMetadata(stats.air_level));
  grid->insertMeta("voxelsieve_margin_voxels", openvdb::Int32Metadata(options.margin_voxels));
  grid->insertMeta("voxelsieve_source_dims",
                   openvdb::Vec3IMetadata(
                       openvdb::Vec3i(volume_max.x() + 1, volume_max.y() + 1, volume_max.z() + 1)));

  stats.block_count = static_cast<std::int64_t>(blocks.states.size());
  stats.voxel_count = static_cast<std::int64_t>(volume.voxelCount());
  stats.active_voxel_count = static_cast<std::int64_t>(grid->activeVoxelCount());
  result.grid = grid;
  return result;
}

void writeVdb(const std::filesystem::path& path, const openvdb::GridPtrVec& grids) {
  openvdb::initialize();
  openvdb::io::File file(path.string());
  file.setCompression(openvdb::io::Archive::hasBloscCompression() ? openvdb::io::COMPRESS_BLOSC
                                                                  : openvdb::io::COMPRESS_ZIP);
  file.write(grids);
  file.close();
}

}  // namespace voxelsieve
