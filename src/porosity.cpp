#include "voxelsieve/porosity.hpp"

#include <openvdb/io/File.h>
#include <openvdb/tools/Morphology.h>
#include <openvdb/tree/LeafManager.h>
#include <tbb/blocked_range.h>
#include <tbb/combinable.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>
#include <tbb/task_arena.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <queue>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "detail/components.hpp"
#include "detail/png.hpp"
#include "detail/transform.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/telemetry.hpp"
#include "voxelsieve/vdb.hpp"

namespace voxelsieve {
namespace {

using Index3 = std::array<std::int64_t, 3>;
using openvdb::Coord;

constexpr int kBlock = 8;
constexpr int kBlockVoxels = kBlock * kBlock * kBlock;
constexpr std::int64_t kMinImagePixels = 512;
/// A block counts as material when at most this many voxels lie below the threshold (small pores
/// of loosened material, noise).
constexpr int kMaxBelowInMaterialBlock = kBlockVoxels / 16;
/// Width of the surface band: voxels this close to outside air are darkened by partial volume
/// and the unsharpness of the scan, so they describe neither the material nor a zone. On faces
/// that run obliquely through the blocks they would otherwise darken whole blocks.
constexpr int kSurfaceBandVoxels = 3;
/// Material ids a segmentation may use for the reference per material.
constexpr std::size_t kMaxMaterials = 8;
/// Material voxels per material id (index 0 unused) of one 8^3 block.
using MaterialCounts = std::array<std::uint16_t, kMaxMaterials + 1>;

/// Whether nearly all material voxels of `counts` are of material `id`, apart from a few voids
/// and noise.
bool holdsOnly(const MaterialCounts& counts, std::size_t id) {
  int total = 0;
  for (std::size_t m = 1; m <= kMaxMaterials; ++m) {
    total += counts[m];
  }
  return total > 0 && counts[id] >= total - total / 16;
}

/// Index of voxel `c` in a region of `box` read x fastest.
std::size_t regionIndex(const Box& box, const Coord& c) {
  return static_cast<std::size_t>((c.x() - box.min[0]) +
                                  box.size(0) *
                                      ((c.y() - box.min[1]) + box.size(1) * (c.z() - box.min[2])));
}

Coord blockOf(const Coord& voxel) { return {voxel.x() >> 3, voxel.y() >> 3, voxel.z() >> 3}; }

std::array<double, 3> blockCenter(const Coord& block) {
  return {block.x() * 8.0 + 3.5, block.y() * 8.0 + 3.5, block.z() * 8.0 + 3.5};
}

/// Offsets of the 26 neighbours.
const std::array<Coord, 26>& neighbours26() {
  static const std::array<Coord, 26> all = [] {
    std::array<Coord, 26> offsets{};
    std::size_t n = 0;
    for (int z = -1; z <= 1; ++z) {
      for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
          if (x != 0 || y != 0 || z != 0) {
            offsets[n++] = Coord(x, y, z);
          }
        }
      }
    }
    return offsets;
  }();
  return all;
}

const std::array<Coord, 6> face_neighbours{Coord(1, 0, 0),  Coord(-1, 0, 0), Coord(0, 1, 0),
                                           Coord(0, -1, 0), Coord(0, 0, 1),  Coord(0, 0, -1)};

double median(std::vector<float> values) {
  if (values.empty()) {
    return 0.0;
  }
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

/// Statistics of a fully active, mostly material 8^3 block.
struct BlockStats {
  float mean = 0.0F;
  float variance = 0.0F;
};

/// Sums over the voxels of a block that a pore shell leaves: grey values outside the surface band
/// and their count, and the grey values above air inside it.
struct BlockSums {
  int count = 0;
  double sum = 0.0;
  double sum_sq = 0.0;
  double band_above_air = 0.0;
  /// With a material segmentation: the materials of the voxels outside the band.
  MaterialCounts materials{};
};

/// Values per 8^3 block, sorted by block for lookups: for the blocks at the surface or at pores,
/// whose number grows with the surface rather than the volume. About 12 bytes per block plus the
/// value, a third of a tree map.
template <typename T>
class BlockTable {
 public:
  void add(const Coord& block, const T& value) { entries_.emplace_back(block, value); }
  void append(const BlockTable& other) {
    entries_.insert(entries_.end(), other.entries_.begin(), other.entries_.end());
  }
  void sort() {
    tbb::parallel_sort(entries_.begin(), entries_.end(),
                       [](const auto& a, const auto& b) { return a.first < b.first; });
  }
  /// The value of `block`, or nullptr; needs `sort` first.
  [[nodiscard]] const T* find(const Coord& block) const {
    const auto it =
        std::lower_bound(entries_.begin(), entries_.end(), block,
                         [](const auto& entry, const Coord& c) { return entry.first < c; });
    return it == entries_.end() || it->first != block ? nullptr : &it->second;
  }
  [[nodiscard]] auto begin() const { return entries_.begin(); }
  [[nodiscard]] auto end() const { return entries_.end(); }

 private:
  std::vector<std::pair<Coord, T>> entries_;
};

/// Everything the first streaming pass collects. Values per block live in grids at block
/// resolution (one voxel per 8^3 block, about 4 bytes per block each), not in tree maps.
struct Scan {
  openvdb::BoolGrid::Ptr candidates = openvdb::BoolGrid::create(false);  // below threshold
  openvdb::BoolGrid::Ptr seeds = openvdb::BoolGrid::create(false);  // candidates next to outside
  /// Mean and variance of the fully active, mostly material blocks; active for those only.
  openvdb::FloatGrid::Ptr mean = openvdb::FloatGrid::create(0.0F);
  openvdb::FloatGrid::Ptr variance = openvdb::FloatGrid::create(0.0F);
  /// Active voxels of every block that holds any.
  openvdb::Int32Grid::Ptr active = openvdb::Int32Grid::create(0);

  /// Adds the disjoint results of another part of the dataset.
  void merge(Scan& other) {
    candidates->tree().topologyUnion(other.candidates->tree());
    seeds->tree().topologyUnion(other.seeds->tree());
    mean->tree().merge(other.mean->tree());
    variance->tree().merge(other.variance->tree());
    active->tree().merge(other.active->tree());
  }
};

Scan scanBricks(const Dataset& dataset, const ProgressToken& progress) {
  const DatasetInfo& info = dataset.info();
  const float threshold = info.threshold;
  // One accumulator per thread, merged at the end. No lock is held around OpenVDB calls, which may
  // run nested TBB tasks (a lock there can deadlock when the waiting thread steals another brick).
  tbb::combinable<Scan> partial_scans;
  dataset.forEachGrid(
      0,
      [&](const Index3& brick, const auto& grid) {
        const Box box = dataset.brickBox(0, brick);
        Scan brick_scan;
        auto candidate_acc = brick_scan.candidates->getAccessor();
        auto seed_acc = brick_scan.seeds->getAccessor();
        auto mean_acc = brick_scan.mean->getAccessor();
        auto variance_acc = brick_scan.variance->getAccessor();
        auto active_acc = brick_scan.active->getAccessor();
        const auto acc = grid.getConstAccessor();
        Dataset::Sampler neighbours(dataset, 0);  // voxels of the bricks around this one

        const auto is_active = [&](const Coord& c) {
          const bool inside_brick = c.x() >= box.min[0] && c.x() < box.max[0] &&
                                    c.y() >= box.min[1] && c.y() < box.max[1] &&
                                    c.z() >= box.min[2] && c.z() < box.max[2];
          return inside_brick ? acc.isValueOn(c) : neighbours({c.x(), c.y(), c.z()}).has_value();
        };

        for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
          double sum = 0.0;
          double sum_sq = 0.0;
          int below = 0;
          for (auto it = leaf->cbeginValueOn(); it; ++it) {
            const float value = *it;
            sum += value;
            sum_sq += static_cast<double>(value) * value;
            if (value >= threshold) {
              continue;
            }
            ++below;
            const Coord c = it.getCoord();
            candidate_acc.setValueOn(c);
            if (std::any_of(face_neighbours.begin(), face_neighbours.end(),
                            [&](const Coord& d) { return !is_active(c + d); })) {
              seed_acc.setValueOn(c);
            }
          }
          const auto count = static_cast<std::int64_t>(leaf->onVoxelCount());
          const Coord block = blockOf(leaf->origin());
          active_acc.setValue(block, static_cast<int>(count));
          // Material blocks only: blocks at the surface mix in air and would bias the material
          // level.
          if (count == kBlockVoxels && below <= kMaxBelowInMaterialBlock) {
            const double mean = sum / kBlockVoxels;
            const double variance = std::max(0.0, sum_sq / kBlockVoxels - mean * mean);
            mean_acc.setValue(block, static_cast<float>(mean));
            variance_acc.setValue(block, static_cast<float>(variance));
          }
        }

        // topologyUnion runs TBB tasks. Isolated, the waiting thread cannot pick up another brick,
        // whose body would change this thread's accumulator while the union is still writing it.
        tbb::this_task_arena::isolate([&] { partial_scans.local().merge(brick_scan); });
      },
      BrickSweep{.progress = progress});
  Scan scan;
  partial_scans.combine_each([&](Scan& local) { scan.merge(local); });
  // Large voids are runs of fully active leaves; as tiles they cost nothing per voxel.
  scan.candidates->tree().prune();
  return scan;
}

/// The internal components of the candidates: pores and their labels, and the outside air.
struct Components {
  /// Kept pores in id order, with id and voxel count.
  std::vector<DetectedPore> pores;
  /// Pore id per pore voxel; uniform leaves are tiles.
  openvdb::Int32Grid::Ptr labels = openvdb::Int32Grid::create(0);
  /// Outside-air voxels per block, at block resolution.
  openvdb::Int32Grid::Ptr outside = openvdb::Int32Grid::create(0);
};

/// Connected components (26-neighbourhood) of the candidates. Those connected to a seed are
/// outside air, marked in `outside_air`; the others of at least `min_pore_voxels` voxels are
/// pores, numbered in the order of their first voxel. Memory grows with the cells of the
/// candidates, not with their voxels: no coordinate lists, and a cell inside one pore is a tile.
Components internalComponents(const Scan& scan, std::int64_t min_pore_voxels,
                              openvdb::BoolGrid& outside_air) {
  using BoolLeaf = openvdb::BoolTree::LeafNodeType;
  using LabelLeaf = openvdb::Int32Tree::LeafNodeType;
  const detail::VoxelComponents components(scan.candidates->tree());

  // Components that hold a seed are outside air. Seeds are candidates.
  std::vector<std::atomic<bool>> is_outside(components.size());
  openvdb::tree::LeafManager<const openvdb::BoolTree> seed_leaves(scan.seeds->tree());
  seed_leaves.foreach ([&](const BoolLeaf& leaf, std::size_t) {
    const std::int64_t cell = components.findCell(leaf.origin());
    if (cell < 0) {
      return;
    }
    for (auto it = leaf.cbeginValueOn(); it; ++it) {
      if (components.isOn(static_cast<std::size_t>(cell), it.pos())) {
        is_outside[components.component(static_cast<std::size_t>(cell), it.pos())].store(
            true, std::memory_order_relaxed);
      }
    }
  });

  Components result;
  std::vector<std::int32_t> pore_of(components.size(), 0);
  for (std::size_t c = 0; c < components.size(); ++c) {
    if (!is_outside[c] &&
        static_cast<std::int64_t>(components.componentSize(c)) >= min_pore_voxels) {
      if (result.pores.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("More pores than 32-bit labels can hold");
      }
      DetectedPore pore;
      pore.id = static_cast<int>(result.pores.size()) + 1;
      pore.voxel_count = static_cast<std::int64_t>(components.componentSize(c));
      pore_of[c] = pore.id;
      result.pores.push_back(pore);
    }
  }

  // Outside air and pore labels cell by cell: built in parallel, a chunk at a time so the leaves
  // in flight stay bounded, and added to the grids in order.
  struct CellResult {
    std::unique_ptr<BoolLeaf> air;
    std::unique_ptr<LabelLeaf> labels;
    int air_count = 0;
    int tile_label = 0;  // a full cell of one pore
  };
  constexpr std::size_t kChunk = std::size_t{1} << 16U;
  std::vector<CellResult> cells;
  auto outside_acc = result.outside->getAccessor();
  for (std::size_t begin = 0; begin < components.cellCount(); begin += kChunk) {
    const std::size_t end = std::min(components.cellCount(), begin + kChunk);
    cells.clear();
    cells.resize(end - begin);
    tbb::parallel_for(begin, end, [&](std::size_t i) {
      const auto pieces = components.componentsInCell(i);
      const bool any_air =
          std::any_of(pieces.begin(), pieces.end(), [&](auto c) { return is_outside[c].load(); });
      const bool any_pore =
          std::any_of(pieces.begin(), pieces.end(), [&](auto c) { return pore_of[c] > 0; });
      CellResult& cell = cells[i - begin];
      const Coord origin = components.cellOrigin(i);
      if (components.cellFull(i)) {  // one piece
        cell.air_count = any_air ? kBlockVoxels : 0;
        cell.tile_label = any_pore ? pore_of[pieces.front()] : 0;
        return;
      }
      if (any_air) {
        cell.air = std::make_unique<BoolLeaf>(origin, false);
      }
      if (any_pore) {
        cell.labels = std::make_unique<LabelLeaf>(origin, 0, false);
      }
      components.forEachOn(i, [&](openvdb::Index pos) {
        const auto c = components.component(i, pos);
        if (is_outside[c]) {
          cell.air->setValueOn(pos);
          ++cell.air_count;
        } else if (pore_of[c] > 0) {
          cell.labels->setValueOn(pos, pore_of[c]);
        }
      });
    });
    for (std::size_t i = begin; i < end; ++i) {
      CellResult& cell = cells[i - begin];
      const Coord origin = components.cellOrigin(i);
      if (cell.air) {
        outside_air.tree().addLeaf(cell.air.release());
      } else if (cell.air_count > 0) {
        outside_air.tree().addTile(1, origin, true, true);
      }
      if (cell.air_count > 0) {
        outside_acc.setValue(blockOf(origin), cell.air_count);
      }
      if (cell.labels) {
        result.labels->tree().addLeaf(cell.labels.release());
      } else if (cell.tile_label > 0) {
        result.labels->tree().addTile(1, origin, cell.tile_label, true);
      }
    }
  }
  result.labels->tree().prune();
  return result;
}

/// The voxels of a cell (an 8^3 leaf) of a tree: the leaf's active voxels, or all of a tile.
/// Calls `f(cell origin, leaf or nullptr)` for each cell with active voxels, leaves first.
template <typename TreeT, typename F>
void forEachCell(const TreeT& tree, F&& f) {
  for (auto leaf = tree.cbeginLeaf(); leaf; ++leaf) {
    if (!leaf->isEmpty()) {
      f(leaf->origin(), leaf.getLeaf());
    }
  }
  auto tile = tree.cbeginValueOn();
  tile.setMaxDepth(TreeT::ValueOnCIter::LEAF_DEPTH - 1);
  for (; tile; ++tile) {
    openvdb::CoordBBox box;
    tile.getBoundingBox(box);
    for (int x = box.min().x(); x <= box.max().x(); x += kBlock) {
      for (int y = box.min().y(); y <= box.max().y(); y += kBlock) {
        for (int z = box.min().z(); z <= box.max().z(); z += kBlock) {
          f(Coord(x, y, z), static_cast<const typename TreeT::LeafNodeType*>(nullptr));
        }
      }
    }
  }
}

/// Mean and variance of the blocks that describe the material, at block resolution.
struct MaterialBlocks {
  openvdb::FloatGrid::Ptr mean;
  openvdb::FloatGrid::Ptr variance;

  /// Calls `f(block, mean)` for every block, in tree order.
  template <typename F>
  void forEach(F&& f) const {
    for (auto it = mean->cbeginValueOn(); it; ++it) {
      f(it.getCoord(), *it);
    }
  }
  void erase(const Coord& block) const {
    mean->tree().setValueOff(block);
    variance->tree().setValueOff(block);
  }
};

/// Depth of every block inside the part below the surface, in voxels.
///
/// Depth is measured at block resolution but with sub-block precision: each block's part fraction
/// (voxels that are neither removed nor outside air) places the surface between block centres,
/// and depth grows by one block per step inside.
class BlockDepth {
 public:
  BlockDepth(const Scan& scan, const openvdb::Int32Grid& outside) {
    // Part voxels (neither removed nor outside air) of every block, at block resolution.
    auto part = scan.active->deepCopy();
    {
      auto part_acc = part->getAccessor();
      for (auto it = outside.cbeginValueOn(); it; ++it) {
        part_acc.setValue(it.getCoord(), part_acc.getValue(it.getCoord()) - *it);
      }
    }
    const auto part_acc = part->getConstAccessor();
    const auto fraction = [&](const Coord& block) {
      return static_cast<double>(part_acc.getValue(block)) / kBlockVoxels;
    };
    const auto inside = [&](const Coord& block) { return fraction(block) >= 0.5; };

    // Seeds: blocks at least half inside with a neighbour that is not fully inside. For a planar
    // surface the depth of the block centre is 8 * (f_block + f_neighbour) - 4 voxels.
    using Item = std::pair<double, Coord>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
    auto depth_acc = depth_->getAccessor();
    for (auto it = part->cbeginValueOn(); it; ++it) {
      const Coord block = it.getCoord();
      if (!inside(block)) {
        continue;
      }
      const double own = fraction(block);
      double best = std::numeric_limits<double>::infinity();
      for (const Coord& d : face_neighbours) {
        const double neighbour = fraction(block + d);
        if (neighbour < 1.0) {
          best = std::min(best, kBlock * (own + neighbour) - kBlock / 2.0);
        }
      }
      if (best < std::numeric_limits<double>::infinity()) {
        depth_acc.setValue(block, best);
        queue.emplace(best, block);
      }
    }
    while (!queue.empty()) {
      const auto [depth, block] = queue.top();
      queue.pop();
      if (depth > depth_acc.getValue(block)) {
        continue;
      }
      for (const Coord& d : face_neighbours) {
        const Coord n = block + d;
        if (!inside(n)) {
          continue;
        }
        if (!depth_acc.isValueOn(n) || depth + kBlock < depth_acc.getValue(n)) {
          depth_acc.setValue(n, depth + kBlock);
          queue.emplace(depth + kBlock, n);
        }
      }
    }
  }

  /// Depth of `block`; 0 for blocks outside the part. Safe to call from many threads.
  [[nodiscard]] double at(const Coord& block) const { return depth_->tree().getValue(block); }
  [[nodiscard]] bool contains(const Coord& block) const { return depth_->tree().isValueOn(block); }

 private:
  /// Depth per block, at block resolution; active for the blocks inside the part.
  openvdb::DoubleGrid::Ptr depth_ = openvdb::DoubleGrid::create(0.0);
};

/// Grey value of one material as a function of the depth below the surface, which follows
/// cupping from beam hardening: median of the block means at each depth. Loosened zones are local
/// and cover only a small part of each depth, so the median ignores them.
class DepthLevels {
 public:
  /// From the blocks of `material` for which `keep(block)` holds.
  template <typename Keep>
  DepthLevels(const BlockDepth& depth, const MaterialBlocks& material, Keep&& keep,
              double fallback) {
    // Median per depth bin; bins with too few blocks are skipped and interpolated over.
    std::vector<std::pair<double, float>> blocks;  // depth and mean of the blocks used
    material.forEach([&](const Coord& block, float mean) {
      if (depth.contains(block) && keep(block)) {
        blocks.emplace_back(depth.at(block), mean);
      }
    });
    std::map<int, std::vector<float>> per_bin;
    for (const auto& [block_depth, mean] : blocks) {
      per_bin[static_cast<int>(block_depth / kBinVoxels)].push_back(mean);
    }
    for (auto& [bin, means] : per_bin) {
      if (means.size() >= kMinBlocks) {
        levels_[(bin + 0.5) * kBinVoxels] = median(std::move(means));
      }
    }
    per_bin.clear();
    if (levels_.empty()) {
      levels_[0.0] = fallback;
    }
    // Spread of the block means around the level (median absolute deviation, scaled to a
    // standard deviation): noise, and the variation of the material itself.
    std::vector<float> deviations;
    deviations.reserve(blocks.size());
    for (const auto& [block_depth, mean] : blocks) {
      deviations.push_back(static_cast<float>(std::abs(mean - at(block_depth))));
    }
    blocks = {};
    spread_ = kMadToSigma * median(std::move(deviations));
  }

  /// Standard deviation of the means of the blocks of this material around its level.
  [[nodiscard]] double spread() const { return spread_; }

  /// Level at `depth` voxels below the surface, interpolated linearly.
  [[nodiscard]] double at(double depth) const {
    const auto upper = levels_.lower_bound(depth);
    if (upper == levels_.begin()) {
      // Above the first bin, toward the surface: cupping is steepest here, so extrapolate.
      const auto next = std::next(upper);
      if (next == levels_.end()) {
        return upper->second;
      }
      const double slope = (next->second - upper->second) / (next->first - upper->first);
      return upper->second + slope * (depth - upper->first);
    }
    if (upper == levels_.end()) {
      return std::prev(upper)->second;
    }
    const auto lower = std::prev(upper);
    const double t = (depth - lower->first) / (upper->first - lower->first);
    return lower->second + t * (upper->second - lower->second);
  }

 private:
  static constexpr double kBinVoxels = 2.0;
  static constexpr std::size_t kMinBlocks = 4;
  static constexpr double kMadToSigma = 1.4826;
  std::map<double, double> levels_;  // bin centre depth -> material level
  double spread_ = 0.0;
};

/// Expected grey value of material at each block: the level by depth of the one material, or with
/// a material segmentation the mix of the levels of the materials in the block.
class MaterialReference {
 public:
  MaterialReference(const BlockDepth& depth, const MaterialBlocks& material, double fallback)
      : depth_(depth) {
    levels_.emplace_back(depth, material, [](const Coord&) { return true; }, fallback);
  }

  /// One level by depth per material id, from the blocks of `material` that hold that material
  /// only (`counts`, or `restricted` for the voxels of a block that describe it); `fallbacks`
  /// (grey value per id) where a material has no such blocks at any depth.
  MaterialReference(const BlockDepth& depth, const MaterialBlocks& material,
                    const BlockTable<MaterialCounts>& counts,
                    const BlockTable<MaterialCounts>& restricted,
                    const std::array<double, kMaxMaterials + 1>& fallbacks, double fallback)
      : depth_(depth), counts_(&counts) {
    levels_.emplace_back(depth, material, [](const Coord&) { return true; }, fallback);
    for (std::size_t id = 1; id <= kMaxMaterials; ++id) {
      const auto pure = [&](const Coord& block) {
        if (const MaterialCounts* own = restricted.find(block)) {
          return holdsOnly(*own, id);
        }
        const MaterialCounts* all = counts.find(block);
        return all != nullptr && holdsOnly(*all, id);
      };
      levels_.emplace_back(depth, material, pure, fallbacks[id]);
    }
  }

  /// Material level for the block containing `voxel`.
  [[nodiscard]] double at(const Coord& voxel) const { return at(voxel, countsOf(blockOf(voxel))); }

  /// Material level for the block containing `voxel` when its voxels hold `counts` of each
  /// material (for example only those outside the surface band).
  [[nodiscard]] double at(const Coord& voxel, const MaterialCounts* counts) const {
    const double depth = depth_.at(blockOf(voxel));
    if (counts != nullptr && counts_ != nullptr) {
      double sum = 0.0;
      int count = 0;
      for (std::size_t id = 1; id <= kMaxMaterials; ++id) {
        if ((*counts)[id] > 0) {
          sum += (*counts)[id] * levels_[id].at(depth);
          count += (*counts)[id];
        }
      }
      if (count > 0) {
        return sum / count;
      }
    }
    return levels_.front().at(depth);
  }

  /// With a material segmentation: standard deviation of the block means of the materials
  /// `counts` around their levels, weighted like the level; 0 without.
  [[nodiscard]] double spread(const MaterialCounts* counts) const {
    if (counts_ == nullptr) {
      return 0.0;
    }
    double sum = 0.0;
    int count = 0;
    for (std::size_t id = 1; counts != nullptr && id <= kMaxMaterials; ++id) {
      sum += (*counts)[id] * levels_[id].spread();
      count += (*counts)[id];
    }
    return count > 0 ? sum / count : levels_.front().spread();
  }

  /// Materials of `block` with a material segmentation; nullptr without or outside the material.
  [[nodiscard]] const MaterialCounts* countsOf(const Coord& block) const {
    return counts_ == nullptr ? nullptr : counts_->find(block);
  }

 private:
  const BlockDepth& depth_;
  const BlockTable<MaterialCounts>* counts_ = nullptr;
  std::vector<DepthLevels> levels_;  // all material, then per material id
};

/// Material voxels per block and the grey value per material of `materials`, which must have the
/// dataset's dimensions.
struct MaterialScan {
  BlockTable<MaterialCounts> counts;
  std::array<double, kMaxMaterials + 1> mean{};  // grey value per material id
};

MaterialScan scanMaterials(const Dataset& dataset, const MaterialVolume& materials,
                           const ProgressToken& progress) {
  const MaterialVolumeInfo& info = materials.info();
  if (info.dims != dataset.info().dims) {
    throw std::invalid_argument("The material volume does not have the dataset's dimensions");
  }
  for (const Material& material : info.materials) {
    if (material.id < 1 || material.id > static_cast<int>(kMaxMaterials)) {
      throw std::invalid_argument("Porosity takes material ids 1 to " +
                                  std::to_string(kMaxMaterials));
    }
  }
  struct Partial {
    BlockTable<MaterialCounts> counts;
    std::array<double, kMaxMaterials + 1> sum{};
    std::array<double, kMaxMaterials + 1> n{};
  };
  tbb::combinable<Partial> partials;
  dataset.forEachGrid(
      0,
      [&](const Index3& brick, const auto& grid) {
        const Box box = dataset.brickBox(0, brick);
        std::vector<std::uint8_t> ids(static_cast<std::size_t>(box.voxelCount()));
        // readRegion runs TBB tasks; isolated, the waiting thread cannot start another brick.
        tbb::this_task_arena::isolate([&] { materials.readRegion(box, ids); });
        Partial brick_partial;
        for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
          MaterialCounts counts{};
          bool any = false;
          for (auto it = leaf->cbeginValueOn(); it; ++it) {
            const Coord c = it.getCoord();
            const std::size_t id = ids[regionIndex(box, c)];
            if (id == 0 || id > kMaxMaterials) {
              continue;
            }
            ++counts[id];
            brick_partial.sum[id] += *it;
            ++brick_partial.n[id];
            any = true;
          }
          if (any) {
            brick_partial.counts.add(blockOf(leaf->origin()), counts);
          }
        }
        Partial& local = partials.local();
        local.counts.append(brick_partial.counts);
        for (std::size_t id = 0; id <= kMaxMaterials; ++id) {
          local.sum[id] += brick_partial.sum[id];
          local.n[id] += brick_partial.n[id];
        }
      },
      BrickSweep{.progress = progress});
  MaterialScan scan;
  std::array<double, kMaxMaterials + 1> sum{};
  std::array<double, kMaxMaterials + 1> n{};
  partials.combine_each([&](Partial& local) {
    scan.counts.append(local.counts);
    local.counts = {};
    for (std::size_t id = 0; id <= kMaxMaterials; ++id) {
      sum[id] += local.sum[id];
      n[id] += local.n[id];
    }
  });
  scan.counts.sort();
  for (std::size_t id = 0; id <= kMaxMaterials; ++id) {
    scan.mean[id] = n[id] > 0.0 ? sum[id] / n[id] : 0.0;
  }
  return scan;
}

std::array<double, 3> toArray(const Coord& c) {
  return {static_cast<double>(c.x()), static_cast<double>(c.y()), static_cast<double>(c.z())};
}

Box boundsOf(const std::array<double, 3>& lo, const std::array<double, 3>& hi) {
  return {{static_cast<std::int64_t>(lo[0]), static_cast<std::int64_t>(lo[1]),
           static_cast<std::int64_t>(lo[2])},
          {static_cast<std::int64_t>(hi[0]) + 1, static_cast<std::int64_t>(hi[1]) + 1,
           static_cast<std::int64_t>(hi[2]) + 1}};
}

}  // namespace

double PorosityResult::poreVolumeMm3() const {
  double volume = 0.0;
  for (const DetectedPore& pore : pores) {
    volume += pore.volume_mm3;
  }
  return volume;
}

double PorosityResult::zoneVoidVolumeMm3() const {
  double volume = 0.0;
  for (const PorosityZone& zone : zones) {
    volume += zone.void_volume_mm3;
  }
  return volume;
}

double PorosityResult::porosity() const {
  return part_volume_mm3 > 0.0 ? (poreVolumeMm3() + zoneVoidVolumeMm3()) / part_volume_mm3 : 0.0;
}

double PorosityResult::partVolumeMm3(const std::array<std::array<double, 3>, 2>& box_mm) const {
  // Box in voxel index units; voxel i spans [i - 0.5, i + 0.5].
  std::array<double, 3> lo{};
  std::array<double, 3> hi{};
  for (std::size_t a = 0; a < 3; ++a) {
    lo[a] = box_mm[0][a] / voxel_size[a];
    hi[a] = box_mm[1][a] / voxel_size[a];
  }
  const auto inside = [&](const std::array<double, 3>& p) {
    for (std::size_t a = 0; a < 3; ++a) {
      if (p[a] < lo[a] || p[a] >= hi[a]) {
        return false;
      }
    }
    return true;
  };
  double volume = 0.0;
  if (material_blocks) {
    for (auto it = material_blocks->cbeginValueOn(); it; ++it) {
      const Coord block = it.getCoord();
      double overlap = 1.0;
      for (std::size_t axis = 0; axis < 3; ++axis) {
        const double begin = block[axis] * static_cast<double>(kBlock) - 0.5;
        const double length = std::min(hi[axis], begin + kBlock) - std::max(lo[axis], begin);
        overlap *= std::clamp(length / kBlock, 0.0, 1.0);
      }
      volume += overlap * *it;
    }
  }
  for (const DetectedPore& pore : pores) {
    if (inside(pore.center_voxels)) {
      volume += pore.volume_mm3;
    }
  }
  for (const PorosityZone& zone : zones) {
    if (inside(zone.center_voxels)) {
      volume += zone.void_volume_mm3;
    }
  }
  return volume;
}

PorosityResult analyzePorosity(const Dataset& dataset, const PorosityOptions& options) {
  const DatasetInfo& info = dataset.info();
  PorosityResult result;
  result.voxel_size = info.voxel_size;
  result.air_level = info.air_level;
  const double air = info.air_level;
  const double voxel_volume = info.voxel_size.volumeMm3();

  std::optional<TelemetryPhase> phase(std::in_place, "scan bricks");
  const Scan scan = scanBricks(dataset, options.progress.part(0.0, 0.3));
  phase.reset();
  std::optional<MaterialScan> material_scan;
  if (options.materials != nullptr) {
    phase.emplace("materials");
    material_scan = scanMaterials(dataset, *options.materials, options.progress.part(0.3, 0.4));
    phase.reset();
  }
  phase.emplace("pores and zones");
  auto band = openvdb::BoolGrid::create(false);
  options.progress.check();
  Components components = internalComponents(scan, options.min_pore_voxels, *band);
  options.progress.check();
  openvdb::tools::dilateActiveValues(band->tree(), kSurfaceBandVoxels,
                                     openvdb::tools::NN_FACE_EDGE_VERTEX);
  band->tree().voxelizeActiveTiles();
  // Blocks are 8^3 like the leaves of the grid, so a block touches the band when its leaf exists.
  const auto touches_band = [&](const Coord& block) {
    const auto* leaf = band->tree().probeConstLeaf(
        Coord(block.x() * kBlock, block.y() * kBlock, block.z() * kBlock));
    return leaf != nullptr && !leaf->isEmpty();
  };

  // Pore candidates and their one-voxel shells. Blocks they touch do not describe the material,
  // so they are left out of the material level; their zone deficit is computed without the shell.
  result.pore_labels = components.labels;
  result.pores = std::move(components.pores);
  // The shells of all pores in one grid: the pore voxels dilated by one voxel. The shell of a
  // pore is the voxels with one of its voxels in their 26-neighbourhood; shells of pores close
  // together overlap. The inside of large pores stays tiles, which the dilation keeps.
  auto all_shells = openvdb::BoolGrid::create(false);
  all_shells->tree().topologyUnion(result.pore_labels->tree());
  openvdb::tools::dilateActiveValues(all_shells->tree(), 1, openvdb::tools::NN_FACE_EDGE_VERTEX);
  // A block is one cell of the shells; the blocks they touch do not describe the material.
  const MaterialBlocks material_blocks{scan.mean->deepCopy(), scan.variance->deepCopy()};
  {
    auto mean_acc = material_blocks.mean->getAccessor();
    auto variance_acc = material_blocks.variance->getAccessor();
    forEachCell(all_shells->tree(), [&](const Coord& origin, const auto*) {
      mean_acc.setValueOff(blockOf(origin));
      variance_acc.setValueOff(blockOf(origin));
    });
  }

  // Blocks at the surface or at a pore: sums over their voxels outside the pore shells, apart
  // for the surface band and the rest, in one more pass over the bricks.
  auto restricted = openvdb::BoolGrid::create(false);
  {
    auto restricted_acc = restricted->getAccessor();
    const auto material_acc = material_blocks.mean->getConstAccessor();
    for (auto it = scan.mean->cbeginValueOn(); it; ++it) {
      if (!material_acc.isValueOn(it.getCoord()) || touches_band(it.getCoord())) {
        restricted_acc.setValueOn(it.getCoord());
      }
    }
  }
  tbb::combinable<BlockTable<BlockSums>> partial_sums;
  dataset.forEachGrid(
      0,
      [&](const Index3& brick, const auto& grid) {
        const Box box = dataset.brickBox(0, brick);
        std::vector<std::uint8_t> ids;
        if (options.materials != nullptr) {
          ids.resize(static_cast<std::size_t>(box.voxelCount()));
          // readRegion runs TBB tasks; isolated, the waiting thread cannot start another brick.
          tbb::this_task_arena::isolate([&] { options.materials->readRegion(box, ids); });
        }
        BlockTable<BlockSums> brick_sums;
        const auto band_in_brick = band->getConstAccessor();
        const auto shells_in_brick = all_shells->getConstAccessor();
        const auto restricted_in_brick = restricted->getConstAccessor();
        for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
          const Coord block = blockOf(leaf->origin());
          if (!restricted_in_brick.isValueOn(block)) {
            continue;
          }
          BlockSums sums;
          for (auto it = leaf->cbeginValueOn(); it; ++it) {
            const Coord c = it.getCoord();
            if (shells_in_brick.isValueOn(c)) {
              continue;
            }
            const double value = *it;
            if (band_in_brick.isValueOn(c)) {
              sums.band_above_air += value - air;
            } else {
              sums.sum += value;
              sums.sum_sq += value * value;
              ++sums.count;
              if (!ids.empty()) {
                const std::size_t id = ids[regionIndex(box, c)];
                if (id >= 1 && id <= kMaxMaterials) {
                  ++sums.materials[id];
                }
              }
            }
          }
          brick_sums.add(block, sums);
        }
        partial_sums.local().append(brick_sums);
      },
      BrickSweep{.progress = options.progress.part(0.4, 0.7)});
  BlockTable<BlockSums> block_sums;
  partial_sums.combine_each([&](BlockTable<BlockSums>& local) {
    // Bricks do not share blocks.
    block_sums.append(local);
    local = {};
  });
  block_sums.sort();
  restricted = {};

  // Material blocks at the surface: statistics of the voxels outside the band only.
  {
    auto mean_acc = material_blocks.mean->getAccessor();
    auto variance_acc = material_blocks.variance->getAccessor();
    for (const auto& [block, s] : block_sums) {
      if (!mean_acc.isValueOn(block)) {
        continue;
      }
      if (s.count < kBlockVoxels / 4) {
        mean_acc.setValueOff(block);
        variance_acc.setValueOff(block);
        continue;
      }
      const double mean = s.sum / s.count;
      mean_acc.setValue(block, static_cast<float>(mean));
      variance_acc.setValue(block,
                            static_cast<float>(std::max(0.0, s.sum_sq / s.count - mean * mean)));
    }
  }

  std::vector<float> means;
  std::vector<float> variances;
  for (auto it = material_blocks.mean->cbeginValueOn(); it; ++it) {
    means.push_back(*it);
  }
  for (auto it = material_blocks.variance->cbeginValueOn(); it; ++it) {
    variances.push_back(*it);
  }
  result.material_level = static_cast<float>(means.empty() ? info.threshold : median(means));
  result.noise_sigma = std::sqrt(median(variances));
  means = {};
  variances = {};
  const BlockDepth depth(scan, *components.outside);
  components.outside = {};
  // The materials of the voxels outside the band and the pore shells, for the blocks restricted
  // to those.
  BlockTable<MaterialCounts> restricted_materials;
  if (material_scan) {
    for (const auto& [block, sums] : block_sums) {
      restricted_materials.add(block, sums.materials);
    }
    restricted_materials.sort();
  }
  const auto materials_of = [&](const Coord& block) -> const MaterialCounts* {
    if (const MaterialCounts* own = restricted_materials.find(block)) {
      return own;
    }
    return material_scan ? material_scan->counts.find(block) : nullptr;
  };
  const MaterialReference reference =
      material_scan
          ? MaterialReference(depth, material_blocks, material_scan->counts, restricted_materials,
                              material_scan->mean, result.material_level)
          : MaterialReference(depth, material_blocks, result.material_level);
  if (material_scan) {
    for (const Material& material : options.materials->info().materials) {
      std::vector<float> pure;
      material_blocks.forEach([&](const Coord& block, float mean) {
        if (const MaterialCounts* counts = materials_of(block);
            counts != nullptr && holdsOnly(*counts, static_cast<std::size_t>(material.id))) {
          pure.push_back(mean);
        }
      });
      result.material_levels[material.id] = static_cast<float>(
          pure.empty() ? material_scan->mean[static_cast<std::size_t>(material.id)]
                       : median(std::move(pure)));
    }
  }

  // Pore volumes from the grey values of the pore and its shell: each cell of the shells sums its
  // voxels per pore, and the sums of the cells are added up in cell order, so the result does
  // not depend on the threads. Cells go a chunk at a time, so their sums stay bounded.
  struct PoreSums {
    double void_sum = 0.0;
    double weight_sum = 0.0;
    std::array<double, 3> weighted{};
    std::array<double, 3> lo{1e18, 1e18, 1e18};
    std::array<double, 3> hi{-1e18, -1e18, -1e18};
  };
  using ShellLeaf = openvdb::BoolTree::LeafNodeType;
  std::vector<std::pair<Coord, const ShellLeaf*>> shell_cells;
  forEachCell(all_shells->tree(), [&](const Coord& origin, const ShellLeaf* leaf) {
    shell_cells.emplace_back(origin, leaf);
  });
  std::vector<PoreSums> pore_sums(result.pores.size());
  std::vector<std::vector<std::pair<int, PoreSums>>> cell_sums;
  constexpr std::size_t kShellChunk = std::size_t{1} << 14U;
  for (std::size_t begin = 0; begin < shell_cells.size(); begin += kShellChunk) {
    const std::size_t end = std::min(shell_cells.size(), begin + kShellChunk);
    cell_sums.clear();
    cell_sums.resize(end - begin);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(begin, end), [&](const auto& range) {
      Dataset::Sampler grey(dataset, 0);
      auto labels = result.pore_labels->getConstAccessor();
      // The pore whose tile holds the whole cell, or 0.
      const auto tile_label = [&](const Coord& origin) {
        return labels.probeConstLeaf(origin) == nullptr && labels.isValueOn(origin)
                   ? labels.getValue(origin)
                   : 0;
      };
      for (std::size_t l = range.begin(); l != range.end(); ++l) {
        const auto& [origin, leaf] = shell_cells[l];
        std::vector<std::pair<int, PoreSums>>& sums = cell_sums[l - begin];
        // Inside a large pore: the cell and its 26 neighbours lie in one tile of labels, so every
        // voxel is in the shell of that pore only.
        int inner = leaf == nullptr ? tile_label(origin) : 0;
        for (int dz = -1; dz <= 1 && inner > 0; ++dz) {
          for (int dy = -1; dy <= 1 && inner > 0; ++dy) {
            for (int dx = -1; dx <= 1 && inner > 0; ++dx) {
              if (tile_label(origin.offsetBy(dx * kBlock, dy * kBlock, dz * kBlock)) != inner) {
                inner = 0;
              }
            }
          }
        }
        const auto add = [&](const Coord& c, std::span<const int> near, int own) {
          const auto p = toArray(c);
          const double material = reference.at(c);
          const auto value = grey({c.x(), c.y(), c.z()});
          // Voxels outside the kept region are air.
          const double fraction = value ? (material - *value) / (material - air) : 1.0;
          const double weight = std::clamp(fraction, 0.0, 1.0);
          for (const int label : near) {
            auto entry = std::find_if(sums.begin(), sums.end(),
                                      [&](const auto& s) { return s.first == label; });
            if (entry == sums.end()) {
              entry = sums.insert(sums.end(), {label, PoreSums{}});
            }
            PoreSums& s = entry->second;
            s.void_sum += fraction;
            s.weight_sum += weight;
            for (std::size_t a = 0; a < 3; ++a) {
              s.weighted[a] += weight * p[a];
              if (own == label) {
                s.lo[a] = std::min(s.lo[a], p[a]);
                s.hi[a] = std::max(s.hi[a], p[a]);
              }
            }
          }
        };
        for (openvdb::Index pos = 0; pos < ShellLeaf::SIZE; ++pos) {
          if (leaf != nullptr && !leaf->isValueOn(pos)) {
            continue;
          }
          const Coord c = origin + ShellLeaf::offsetToLocalCoord(pos);
          if (inner > 0) {
            add(c, std::span(&inner, 1), inner);
            continue;
          }
          // The pores this voxel is in the shell of.
          std::array<int, 27> near{};
          std::size_t n_near = 0;
          for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
              for (int dx = -1; dx <= 1; ++dx) {
                const int label = labels.getValue(c.offsetBy(dx, dy, dz));
                const std::span<const int> seen = std::span(near).first(n_near);
                if (label > 0 && std::ranges::find(seen, label) == seen.end()) {
                  near[n_near++] = label;
                }
              }
            }
          }
          add(c, std::span(near).first(n_near), labels.getValue(c));
        }
      }
    });
    for (const auto& sums : cell_sums) {
      for (const auto& [label, s] : sums) {
        PoreSums& total = pore_sums[static_cast<std::size_t>(label - 1)];
        total.void_sum += s.void_sum;
        total.weight_sum += s.weight_sum;
        for (std::size_t a = 0; a < 3; ++a) {
          total.weighted[a] += s.weighted[a];
          total.lo[a] = std::min(total.lo[a], s.lo[a]);
          total.hi[a] = std::max(total.hi[a], s.hi[a]);
        }
      }
    }
  }
  cell_sums = {};
  shell_cells = {};
  all_shells = {};
  for (std::size_t i = 0; i < result.pores.size(); ++i) {
    DetectedPore& pore = result.pores[i];
    const PoreSums& s = pore_sums[i];
    pore.volume_mm3 = std::max(0.0, s.void_sum) * voxel_volume;
    for (std::size_t a = 0; a < 3; ++a) {
      pore.center_voxels[a] = s.weight_sum > 0.0 ? s.weighted[a] / s.weight_sum : 0.0;
    }
    pore.bounds = boundsOf(s.lo, s.hi);
    pore.equivalent_diameter_mm = std::cbrt(6.0 * pore.volume_mm3 / std::numbers::pi);
  }

  // Zones: blocks whose mean lies clearly below the local material level.
  // Deficit per block at block resolution, active for the blocks it was computed for.
  auto deficits = openvdb::DoubleGrid::create(0.0);
  auto deficit_acc = deficits->getAccessor();
  auto flagged = openvdb::BoolGrid::create(false);
  auto flagged_acc = flagged->getAccessor();
  for (auto block_it = scan.mean->cbeginValueOn(); block_it; ++block_it) {
    const Coord block = block_it.getCoord();
    const Coord block_origin(block.x() * kBlock, block.y() * kBlock, block.z() * kBlock);
    const MaterialCounts* materials = material_scan ? materials_of(block) : nullptr;
    const double material = reference.at(block_origin, materials);
    const double contrast = material - air;
    if (contrast <= 0.0) {
      continue;
    }
    double deficit = (material - *block_it) / contrast;
    int counted = kBlockVoxels;
    if (const BlockSums* sums = block_sums.find(block)) {
      // The pore's own voids are in its volume already: sum the other voxels only. The surface
      // band is darkened by the air outside, so its voids cannot be told from its grey values;
      // they are taken at the void fraction of the rest of the block, over the band's share of
      // the part (its grey values above air, which the voids lower by that same fraction).
      const BlockSums& s = *sums;
      counted = s.count;
      if (counted < kBlockVoxels / 4) {
        continue;
      }
      const double sum = (counted * material - s.sum) / contrast;
      const double void_fraction = std::clamp(sum / counted, 0.0, 0.5);
      const double band_part = s.band_above_air / contrast / (1.0 - void_fraction);
      // Void fraction of the whole block.
      deficit = (sum + void_fraction * band_part) / kBlockVoxels;
    }
    deficit_acc.setValue(block, deficit);
    const double n = counted;
    // Noise of the block mean, and with several materials the variation of the materials
    // themselves beyond that noise: the fibre content of a fibre bundle varies from block to
    // block, and so does its grey value, without any void.
    const double noise = result.noise_sigma * std::sqrt(n) / kBlockVoxels;
    const double spread = reference.spread(materials);
    const double variation =
        std::max(0.0, spread * spread - result.noise_sigma * result.noise_sigma / kBlockVoxels);
    const double sigma = std::sqrt(noise * noise + variation) / contrast;
    if (deficit > std::max(options.min_zone_void_fraction, options.zone_sigma * sigma)) {
      flagged_acc.setValueOn(block);
    }
  }
  result.zone_blocks = openvdb::FloatGrid::create(0.0F);
  auto zone_acc = result.zone_blocks->getAccessor();
  auto assigned = openvdb::BoolGrid::create(false);
  auto assigned_acc = assigned->getAccessor();
  const double block_volume = kBlockVoxels * voxel_volume;
  for (auto it = flagged->cbeginValueOn(); it; ++it) {
    if (assigned_acc.isValueOn(it.getCoord())) {
      continue;
    }
    std::vector<Coord> members;
    std::deque<Coord> queue{it.getCoord()};
    assigned_acc.setValueOn(it.getCoord());
    while (!queue.empty()) {
      const Coord c = queue.front();
      queue.pop_front();
      members.push_back(c);
      for (const Coord& d : neighbours26()) {
        const Coord n = c + d;
        if (flagged_acc.isValueOn(n) && !assigned_acc.isValueOn(n)) {
          assigned_acc.setValueOn(n);
          queue.push_back(n);
        }
      }
    }
    if (static_cast<std::int64_t>(members.size()) < options.min_zone_blocks) {
      continue;
    }
    PorosityZone zone;
    zone.block_count = static_cast<std::int64_t>(members.size());
    zone.volume_mm3 = static_cast<double>(members.size()) * block_volume;
    // The deficit of a one-block rim counts too: the zone fades out below the detection limit.
    std::map<Coord, double> counted;
    for (const Coord& c : members) {
      counted[c] = deficit_acc.getValue(c);
      for (const Coord& d : neighbours26()) {
        if (deficit_acc.isValueOn(c + d) && !flagged_acc.isValueOn(c + d)) {
          counted[c + d] = deficit_acc.getValue(c + d);
        }
      }
    }
    // Residual errors of the material level (for example from cupping) add up over the many rim
    // blocks, so measure the deficit against the blocks just outside the rim.
    std::vector<float> outer;
    for (const auto& [block, unused] : counted) {
      for (const Coord& d : neighbours26()) {
        const Coord n = block + d;
        if (deficit_acc.isValueOn(n) && !counted.contains(n) && !flagged_acc.isValueOn(n)) {
          outer.push_back(static_cast<float>(deficit_acc.getValue(n)));
        }
      }
    }
    const double baseline = outer.size() >= 8 ? median(std::move(outer)) : 0.0;
    double void_sum = 0.0;
    double weight_sum = 0.0;
    std::array<double, 3> weighted{};
    for (auto& [block, deficit] : counted) {
      deficit -= baseline;
      void_sum += deficit;
      const double weight = std::max(deficit, 0.0);
      weight_sum += weight;
      const auto center = blockCenter(block);
      for (std::size_t a = 0; a < 3; ++a) {
        weighted[a] += weight * center[a];
      }
    }
    zone.void_volume_mm3 = std::max(0.0, void_sum) * block_volume;
    // A smooth darkening such as cupping flags blocks, but its rim cancels the deficit out.
    if (zone.porosity() < options.min_zone_void_fraction) {
      continue;
    }
    std::array<double, 3> lo{1e18, 1e18, 1e18};
    std::array<double, 3> hi{-1e18, -1e18, -1e18};
    for (const Coord& c : members) {
      zone_acc.setValue(c, static_cast<float>(deficit_acc.getValue(c)));
      for (std::size_t a = 0; a < 3; ++a) {
        lo[a] = std::min(lo[a], toArray(c)[a] * kBlock);
        hi[a] = std::max(hi[a], toArray(c)[a] * kBlock + kBlock - 1);
      }
    }
    for (std::size_t a = 0; a < 3; ++a) {
      zone.center_voxels[a] = weight_sum > 0.0 ? weighted[a] / weight_sum : 0.0;
    }
    zone.bounds = boundsOf(lo, hi);
    zone.id = static_cast<int>(result.zones.size()) + 1;
    result.zones.push_back(zone);
  }

  // Part volume: material from the grey values of all kept voxels against the local material
  // level (so cupping does not bias it), plus the voids.
  tbb::combinable<double> material_fractions(0.0);
  tbb::combinable<openvdb::FloatGrid::Ptr> material_grids(
      [] { return openvdb::FloatGrid::create(0.0F); });
  dataset.forEachGrid(
      0,
      [&](const Index3&, const auto& grid) {
        double sum = 0.0;
        auto material_acc = material_grids.local()->getAccessor();
        for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
          const double contrast = reference.at(leaf->origin()) - air;
          double leaf_sum = 0.0;
          for (auto it = leaf->cbeginValueOn(); it; ++it) {
            leaf_sum += *it - air;
          }
          sum += leaf_sum / contrast;
          material_acc.setValue(blockOf(leaf->origin()),
                                static_cast<float>(leaf_sum / contrast * voxel_volume));
        }
        material_fractions.local() += sum;
      },
      BrickSweep{.progress = options.progress.part(0.7, 0.95)});
  result.material_blocks = openvdb::FloatGrid::create(0.0F);
  material_grids.combine_each([&](const openvdb::FloatGrid::Ptr& grid) {
    // Bricks do not share blocks, so the per-thread grids are disjoint.
    result.material_blocks->tree().merge(grid->tree());
  });
  const double material_fraction_sum = material_fractions.combine(std::plus<>());
  result.part_volume_mm3 =
      material_fraction_sum * voxel_volume + result.poreVolumeMm3() + result.zoneVoidVolumeMm3();

  std::sort(result.pores.begin(), result.pores.end(),
            [](const auto& a, const auto& b) { return a.volume_mm3 > b.volume_mm3; });
  std::sort(result.zones.begin(), result.zones.end(),
            [](const auto& a, const auto& b) { return a.void_volume_mm3 > b.void_volume_mm3; });
  return result;
}

namespace {

nlohmann::json poreJson(const DetectedPore& pore, const VoxelSize& v) {
  return {{"id", pore.id},
          {"voxel_count", pore.voxel_count},
          {"volume_mm3", pore.volume_mm3},
          {"equivalent_diameter_mm", pore.equivalent_diameter_mm},
          {"center_voxels", pore.center_voxels},
          {"center_mm", v.toMm(pore.center_voxels)},
          {"bounds_min", pore.bounds.min},
          {"bounds_max", pore.bounds.max}};
}

nlohmann::json zoneJson(const PorosityZone& zone, const VoxelSize& v) {
  return {{"id", zone.id},
          {"block_count", zone.block_count},
          {"volume_mm3", zone.volume_mm3},
          {"void_volume_mm3", zone.void_volume_mm3},
          {"porosity", zone.porosity()},
          {"center_voxels", zone.center_voxels},
          {"center_mm", v.toMm(zone.center_voxels)},
          {"bounds_min", zone.bounds.min},
          {"bounds_max", zone.bounds.max}};
}

/// Everything of `toJson` but the pores and zones.
nlohmann::json summaryJson(const PorosityResult& result);

}  // namespace

nlohmann::json toJson(const PorosityResult& result) {
  nlohmann::json json = summaryJson(result);
  nlohmann::json& pores = json["pores"] = nlohmann::json::array();
  for (const DetectedPore& pore : result.pores) {
    pores.push_back(poreJson(pore, result.voxel_size));
  }
  nlohmann::json& zones = json["zones"] = nlohmann::json::array();
  for (const PorosityZone& zone : result.zones) {
    zones.push_back(zoneJson(zone, result.voxel_size));
  }
  return json;
}

void writePorosityJson(const PorosityResult& result, const std::filesystem::path& path) {
  // One pore at a time: a document of 10^7 pores would take some 18 GB as a JSON tree.
  writeFile(path, [&](std::ostream& out) {
    out << "{\n";
    const nlohmann::json summary = summaryJson(result);
    for (const auto& [key, value] : summary.items()) {
      out << "  " << nlohmann::json(key).dump() << ": " << value.dump() << ",\n";
    }
    const auto list = [&](const char* name, const auto& items, const auto& to_json, bool last) {
      out << "  \"" << name << "\": [";
      for (std::size_t i = 0; i < items.size(); ++i) {
        out << (i == 0 ? "\n    " : ",\n    ") << to_json(items[i], result.voxel_size).dump();
      }
      out << (items.empty() ? "]" : "\n  ]") << (last ? "\n" : ",\n");
    };
    list("pores", result.pores, poreJson, false);
    list("zones", result.zones, zoneJson, true);
    out << "}\n";
  });
}

namespace {

nlohmann::json summaryJson(const PorosityResult& result) {
  const VoxelSize& v = result.voxel_size;
  nlohmann::json material_levels = nlohmann::json::array();
  for (const auto& [id, level] : result.material_levels) {
    material_levels.push_back({{"id", id}, {"level", level}});
  }
  return {{"voxel_size_mm", v},
          {"air_level", result.air_level},
          {"material_level", result.material_level},
          {"material_levels", material_levels},
          {"noise_sigma", result.noise_sigma},
          {"part_volume_mm3", result.part_volume_mm3},
          {"pore_volume_mm3", result.poreVolumeMm3()},
          {"zone_void_volume_mm3", result.zoneVoidVolumeMm3()},
          {"porosity", result.porosity()}};
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Display.

namespace {

/// Sum of values projected along one axis.
/// Sum of values projected along one axis, binned `factor` voxels per pixel so memory stays
/// bounded by the image size.
struct Projection {
  int axis = 2;
  std::int64_t factor = 1;
  std::int64_t width = 0;   // first remaining axis
  std::int64_t height = 0;  // second remaining axis
  std::vector<double> values;

  Projection(int projected_axis, const Index3& dims, std::int64_t max_pixels)
      : axis(projected_axis) {
    const auto [u, w] = axes();
    const std::int64_t longest =
        std::max(dims[static_cast<std::size_t>(u)], dims[static_cast<std::size_t>(w)]);
    factor = std::max<std::int64_t>(1, (longest + max_pixels - 1) / max_pixels);
    width = (dims[static_cast<std::size_t>(u)] + factor - 1) / factor;
    height = (dims[static_cast<std::size_t>(w)] + factor - 1) / factor;
    values.assign(static_cast<std::size_t>(width * height), 0.0);
  }
  [[nodiscard]] std::pair<int, int> axes() const {
    return axis == 0 ? std::pair{1, 2} : axis == 1 ? std::pair{0, 2} : std::pair{0, 1};
  }
  void add(const Coord& c, double value) {
    const auto [u, w] = axes();
    const std::int64_t i =
        c[static_cast<std::size_t>(u)] / factor + width * (c[static_cast<std::size_t>(w)] / factor);
    values[static_cast<std::size_t>(i)] += value;
  }
};

void writeProjectionPng(const std::filesystem::path& path, const Projection& material,
                        const Projection& pores, const Projection& zones,
                        const VoxelSize& voxel_size) {
  const std::int64_t width = material.width;
  const std::int64_t height = material.height;
  const std::int64_t factor = material.factor;
  const auto& thickness = material.values;
  const auto& pore = pores.values;
  const auto& zone = zones.values;
  const double max_thickness =
      std::max(1e-9, *std::max_element(thickness.begin(), thickness.end()));

  // Small volumes are enlarged so the preview stays readable.
  const std::int64_t zoom = std::max<std::int64_t>(1, kMinImagePixels / std::max(width, height));
  const std::int64_t out_width = width * zoom;
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(out_width * height * zoom * 3), 0);
  for (std::int64_t y = 0; y < height; ++y) {
    for (std::int64_t x = 0; x < width; ++x) {
      const auto i = static_cast<std::size_t>(x + width * y);
      const double grey = thickness[i] > 0.0 ? 40.0 + 180.0 * thickness[i] / max_thickness : 0.0;
      std::array<double, 3> color{grey, grey, grey};
      const auto blend = [&](const std::array<double, 3>& tint, double alpha) {
        for (std::size_t c = 0; c < 3; ++c) {
          color[c] = (1.0 - alpha) * color[c] + alpha * tint[c];
        }
      };
      if (zone[i] > 0.0) {
        blend({250.0, 200.0, 0.0}, 0.55);
      }
      if (pore[i] > 0.0) {
        blend({230.0, 30.0, 30.0}, std::min(1.0, 0.6 + 0.1 * pore[i] / double(factor * factor)));
      }
      // Rows top to bottom with the second axis pointing up.
      for (std::int64_t zy = 0; zy < zoom; ++zy) {
        for (std::int64_t zx = 0; zx < zoom; ++zx) {
          const auto out = static_cast<std::size_t>(
              (x * zoom + zx + out_width * ((height - 1 - y) * zoom + zy)) * 3);
          for (std::size_t c = 0; c < 3; ++c) {
            rgb[out + c] = static_cast<std::uint8_t>(std::clamp(color[c], 0.0, 255.0));
          }
        }
      }
    }
  }
  const auto [u, w] = material.axes();
  detail::writeRgbPng(
      path, static_cast<std::uint32_t>(out_width), static_cast<std::uint32_t>(height * zoom), rgb,
      voxel_size[static_cast<std::size_t>(u)], voxel_size[static_cast<std::size_t>(w)]);
}

openvdb::math::Transform::Ptr levelZeroTransform(const VoxelSize& voxel_size) {
  return detail::voxelTransform(voxel_size);
}

}  // namespace

void writePorosityImages(const Dataset& dataset, const PorosityResult& result,
                         const std::filesystem::path& dir, std::int64_t max_pixels) {
  const TelemetryPhase phase("images");
  std::filesystem::create_directories(dir);
  const Index3 dims = dataset.info().dims;
  const double air = result.air_level;
  const double contrast = std::max(1.0, static_cast<double>(result.material_level) - air);
  const auto projections = [&] {
    return std::array<Projection, 3>{Projection(0, dims, max_pixels),
                                     Projection(1, dims, max_pixels),
                                     Projection(2, dims, max_pixels)};
  };
  tbb::combinable<std::array<Projection, 3>> partial_projections(projections);
  dataset.forEachGrid(0, [&](const Index3&, const auto& grid) {
    auto& local = partial_projections.local();
    for (auto it = grid.cbeginValueOn(); it; ++it) {
      const double fraction = std::clamp((*it - air) / contrast, 0.0, 1.0);
      if (fraction > 0.0) {
        for (auto& p : local) {
          p.add(it.getCoord(), fraction);
        }
      }
    }
  });
  auto material = projections();
  partial_projections.combine_each([&](const std::array<Projection, 3>& local) {
    for (std::size_t a = 0; a < 3; ++a) {
      for (std::size_t i = 0; i < material[a].values.size(); ++i) {
        material[a].values[i] += local[a].values[i];
      }
    }
  });

  auto pores = projections();
  if (result.pore_labels) {
    using LabelLeaf = openvdb::Int32Tree::LeafNodeType;
    std::vector<std::pair<Coord, const LabelLeaf*>> cells;
    forEachCell(result.pore_labels->tree(), [&](const Coord& origin, const LabelLeaf* leaf) {
      cells.emplace_back(origin, leaf);
    });
    tbb::combinable<std::array<Projection, 3>> partial_pores(projections);
    tbb::parallel_for(std::size_t{0}, cells.size(), [&](std::size_t i) {
      auto& local = partial_pores.local();
      const auto& [origin, leaf] = cells[i];
      for (openvdb::Index pos = 0; pos < LabelLeaf::SIZE; ++pos) {
        if (leaf == nullptr || leaf->isValueOn(pos)) {
          const Coord c = origin + LabelLeaf::offsetToLocalCoord(pos);
          for (auto& p : local) {
            p.add(c, 1.0);
          }
        }
      }
    });
    partial_pores.combine_each([&](const std::array<Projection, 3>& local) {
      for (std::size_t a = 0; a < 3; ++a) {
        for (std::size_t i = 0; i < pores[a].values.size(); ++i) {
          pores[a].values[i] += local[a].values[i];
        }
      }
    });
  }
  std::array<Projection, 3> zones{Projection(0, dims, max_pixels), Projection(1, dims, max_pixels),
                                  Projection(2, dims, max_pixels)};
  if (result.zone_blocks) {
    for (auto it = result.zone_blocks->cbeginValueOn(); it; ++it) {
      const Coord block = it.getCoord();
      for (int z = 0; z < kBlock; ++z) {
        for (int y = 0; y < kBlock; ++y) {
          for (int x = 0; x < kBlock; ++x) {
            const Coord c(block.x() * kBlock + x, block.y() * kBlock + y, block.z() * kBlock + z);
            if (c.x() < dims[0] && c.y() < dims[1] && c.z() < dims[2]) {
              for (auto& p : zones) {
                p.add(c, 1.0);
              }
            }
          }
        }
      }
    }
  }
  constexpr std::array<const char*, 3> kNames{"projection_x.png", "projection_y.png",
                                              "projection_z.png"};
  for (std::size_t a = 0; a < 3; ++a) {
    writeProjectionPng(dir / kNames[a], material[a], pores[a], zones[a], result.voxel_size);
  }
}

void writePorosityVdb(const Dataset& dataset, const PorosityResult& result,
                      const std::filesystem::path& file) {
  const TelemetryPhase phase("vdb");
  const double air = result.air_level;
  const double contrast = std::max(1.0, static_cast<double>(result.material_level) - air);
  auto pores = openvdb::FloatGrid::create(0.0F);
  if (result.pore_labels) {
    // Leaves of void fractions built in parallel, a chunk at a time, and added in order.
    using LabelLeaf = openvdb::Int32Tree::LeafNodeType;
    using FloatLeaf = openvdb::FloatTree::LeafNodeType;
    std::vector<std::pair<Coord, const LabelLeaf*>> cells;
    forEachCell(result.pore_labels->tree(), [&](const Coord& origin, const LabelLeaf* leaf) {
      cells.emplace_back(origin, leaf);
    });
    constexpr std::size_t kChunk = std::size_t{1} << 14U;
    std::vector<std::unique_ptr<FloatLeaf>> leaves;
    for (std::size_t begin = 0; begin < cells.size(); begin += kChunk) {
      const std::size_t end = std::min(cells.size(), begin + kChunk);
      leaves.clear();
      leaves.resize(end - begin);
      tbb::parallel_for(tbb::blocked_range<std::size_t>(begin, end), [&](const auto& range) {
        Dataset::Sampler grey(dataset, 0);
        for (std::size_t i = range.begin(); i != range.end(); ++i) {
          const auto& [origin, leaf] = cells[i];
          auto out = std::make_unique<FloatLeaf>(origin, 0.0F, false);
          for (openvdb::Index pos = 0; pos < FloatLeaf::SIZE; ++pos) {
            if (leaf != nullptr && !leaf->isValueOn(pos)) {
              continue;
            }
            const Coord c = origin + FloatLeaf::offsetToLocalCoord(pos);
            const auto value = grey({c.x(), c.y(), c.z()});
            const double fraction = value ? (result.material_level - *value) / contrast : 1.0;
            out->setValueOn(pos, static_cast<float>(std::clamp(fraction, 0.0, 1.0)));
          }
          leaves[i - begin] = std::move(out);
        }
      });
      for (auto& leaf : leaves) {
        pores->tree().addLeaf(leaf.release());
      }
    }
  }
  auto zones = openvdb::FloatGrid::create(0.0F);
  if (result.zone_blocks) {
    for (auto it = result.zone_blocks->cbeginValueOn(); it; ++it) {
      const Coord min(it.getCoord().x() * kBlock, it.getCoord().y() * kBlock,
                      it.getCoord().z() * kBlock);
      zones->tree().fill(openvdb::CoordBBox(min, min.offsetBy(kBlock - 1)), *it, true);
    }
  }
  pores->setName("pores");
  zones->setName("zones");
  for (const auto& grid : {pores, zones}) {
    grid->setTransform(levelZeroTransform(result.voxel_size));
    grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  }
  writeVdb(file, {pores, zones});
}

void savePorosityResult(const PorosityResult& result, const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
  writePorosityJson(result, dir / "porosity.json");
  openvdb::GridPtrVec grids;
  const auto add = [&grids](const openvdb::GridBase::Ptr& grid, const char* name) {
    if (grid) {
      auto copy = grid->copyGrid();  // shares the tree; only the name differs
      copy->setName(name);
      grids.push_back(copy);
    }
  };
  add(result.pore_labels, "pore_labels");
  add(result.zone_blocks, "zone_blocks");
  add(result.material_blocks, "material_blocks");
  openvdb::io::File file((dir / "analysis.vdb").string());
  file.write(grids);
  file.close();
}

PorosityResult loadPorosityResult(const std::filesystem::path& dir) {
  std::ifstream in(dir / "porosity.json");
  if (!in) {
    throw std::runtime_error("No porosity result in " + dir.string());
  }
  // Pores and zones are taken over as they are parsed and dropped from the document: as a JSON
  // tree, 10^7 pores would take some 18 GB.
  PorosityResult result;
  const auto read_pore = [](const nlohmann::json& item) {
    DetectedPore pore;
    pore.id = item.at("id").get<int>();
    pore.voxel_count = item.at("voxel_count").get<std::int64_t>();
    pore.volume_mm3 = item.at("volume_mm3").get<double>();
    pore.equivalent_diameter_mm = item.at("equivalent_diameter_mm").get<double>();
    pore.center_voxels = item.at("center_voxels").get<std::array<double, 3>>();
    pore.bounds.min = item.at("bounds_min").get<std::array<std::int64_t, 3>>();
    pore.bounds.max = item.at("bounds_max").get<std::array<std::int64_t, 3>>();
    return pore;
  };
  const auto read_zone = [](const nlohmann::json& item) {
    PorosityZone zone;
    zone.id = item.at("id").get<int>();
    zone.block_count = item.at("block_count").get<std::int64_t>();
    zone.volume_mm3 = item.at("volume_mm3").get<double>();
    zone.void_volume_mm3 = item.at("void_volume_mm3").get<double>();
    zone.center_voxels = item.at("center_voxels").get<std::array<double, 3>>();
    zone.bounds.min = item.at("bounds_min").get<std::array<std::int64_t, 3>>();
    zone.bounds.max = item.at("bounds_max").get<std::array<std::int64_t, 3>>();
    return zone;
  };
  std::string list;  // the top-level array being parsed
  const nlohmann::json json = nlohmann::json::parse(
      in, [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
        using Event = nlohmann::json::parse_event_t;
        if (depth == 1 && event == Event::key) {
          list = parsed.get<std::string>();
        } else if (depth == 2 && event == Event::object_end &&
                   (list == "pores" || list == "zones")) {
          if (list == "pores") {
            result.pores.push_back(read_pore(parsed));
          } else {
            result.zones.push_back(read_zone(parsed));
          }
          return false;  // taken over: not kept in the document
        }
        return true;
      });
  result.voxel_size = json.at("voxel_size_mm").get<VoxelSize>();
  result.air_level = json.at("air_level").get<float>();
  result.material_level = json.at("material_level").get<float>();
  for (const auto& level : json.value("material_levels", nlohmann::json::array())) {
    result.material_levels[level.at("id").get<int>()] = level.at("level").get<float>();
  }
  result.noise_sigma = json.at("noise_sigma").get<double>();
  result.part_volume_mm3 = json.at("part_volume_mm3").get<double>();
  if (std::filesystem::exists(dir / "analysis.vdb")) {
    initializeVdb();
    openvdb::io::File file((dir / "analysis.vdb").string());
    file.open();
    const auto grids = file.getGrids();
    file.close();
    for (const auto& grid : *grids) {
      if (grid->getName() == "pore_labels") {
        result.pore_labels = openvdb::gridPtrCast<openvdb::Int32Grid>(grid);
      } else if (grid->getName() == "zone_blocks") {
        result.zone_blocks = openvdb::gridPtrCast<openvdb::FloatGrid>(grid);
      } else if (grid->getName() == "material_blocks") {
        result.material_blocks = openvdb::gridPtrCast<openvdb::FloatGrid>(grid);
      }
    }
  }
  return result;
}

}  // namespace voxelsieve
