#include "voxelsieve/sieve.hpp"

#include <openvdb/io/File.h>
#include <openvdb/tools/Morphology.h>
#include <openvdb/tree/LeafManager.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "detail/blocks.hpp"
#include "detail/transform.hpp"

namespace voxelsieve {
namespace {

using detail::BlockGrid;
using detail::BlockState;
using detail::ceilDiv;
using detail::floodFillOutsideAir;
using detail::Histogram;
using detail::kHistogramBins;
using detail::otsuThreshold;
using detail::ThresholdResult;

ThresholdResult otsu(const Volume16& volume, int stride) {
  if (stride < 1) {
    throw std::invalid_argument("histogram_stride must be >= 1");
  }
  Histogram histogram(kHistogramBins, 0);
  for (std::int64_t z = 0; z < volume.dims[2]; z += stride) {
    for (std::int64_t y = 0; y < volume.dims[1]; y += stride) {
      for (std::int64_t x = 0; x < volume.dims[0]; x += stride) {
        ++histogram[volume.at(x, y, z)];
      }
    }
  }
  return otsuThreshold(histogram);
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
  floodFillOutsideAir(blocks, options.outside_air_axes);

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

  grid->setTransform(detail::voxelTransform(volume.voxel_size));
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
  // OpenVDB does not report failed writes. A full disk is the usual cause, and it would only
  // show later as "not a VDB file", so check for it here.
  std::error_code error;
  const auto space =
      std::filesystem::space(path.parent_path().empty() ? "." : path.parent_path(), error);
  if (!error && space.available < (std::uintmax_t{1} << 20)) {
    throw std::runtime_error("No space left on the device while writing " + path.string());
  }
}

AirAxes parseAirAxes(std::string_view text) {
  AirAxes axes{false, false, false};
  for (const char c : text) {
    if (c < 'x' || c > 'z') {
      throw std::invalid_argument("Axes must be letters x, y and z, got '" + std::string(text) +
                                  "'");
    }
    axes[static_cast<std::size_t>(c - 'x')] = true;
  }
  if (axes == AirAxes{false, false, false}) {
    throw std::invalid_argument("Outside air needs at least one axis");
  }
  return axes;
}

std::string airAxesName(const AirAxes& axes) {
  std::string name;
  for (std::size_t a = 0; a < 3; ++a) {
    if (axes[a]) {
      name += static_cast<char>('x' + a);
    }
  }
  return name;
}

}  // namespace voxelsieve
