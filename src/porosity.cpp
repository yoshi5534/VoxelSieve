#include "voxelsieve/porosity.hpp"

#include <openvdb/io/File.h>
#include <openvdb/tools/Morphology.h>
#include <tbb/combinable.h>
#include <tbb/task_arena.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>

#include "detail/png.hpp"
#include "detail/transform.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/telemetry.hpp"

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
};

/// Everything the first streaming pass collects.
struct Scan {
  openvdb::BoolGrid::Ptr candidates = openvdb::BoolGrid::create(false);  // below threshold
  openvdb::BoolGrid::Ptr seeds = openvdb::BoolGrid::create(false);  // candidates next to outside
  std::map<Coord, BlockStats> blocks;
  std::set<Coord> solid;         // fully active blocks
  std::map<Coord, int> partial;  // active voxel count of the other blocks
};

Scan scanBricks(const Dataset& dataset) {
  const DatasetInfo& info = dataset.info();
  const float threshold = info.threshold;
  // One accumulator per thread, merged at the end. No lock is held around OpenVDB calls, which may
  // run nested TBB tasks (a lock there can deadlock when the waiting thread steals another brick).
  tbb::combinable<Scan> partial_scans;
  dataset.forEachBrick(0, [&](const Index3& brick, const openvdb::FloatGrid& grid) {
    const Box box = dataset.brickBox(0, brick);
    auto candidates = openvdb::BoolGrid::create(false);
    auto seeds = openvdb::BoolGrid::create(false);
    auto candidate_acc = candidates->getAccessor();
    auto seed_acc = seeds->getAccessor();
    const auto acc = grid.getConstAccessor();
    std::map<Coord, BlockStats> blocks;
    std::set<Coord> solid;
    std::map<Coord, int> partial;

    const auto is_active = [&](const Coord& c) {
      const bool inside_brick = c.x() >= box.min[0] && c.x() < box.max[0] && c.y() >= box.min[1] &&
                                c.y() < box.max[1] && c.z() >= box.min[2] && c.z() < box.max[2];
      return inside_brick ? acc.isValueOn(c) : dataset.sample(0, {c.x(), c.y(), c.z()}).has_value();
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
      if (count == kBlockVoxels) {
        solid.insert(blockOf(leaf->origin()));
      } else {
        partial[blockOf(leaf->origin())] = static_cast<int>(count);
      }
      // Material blocks only: blocks at the surface mix in air and would bias the material level.
      if (count == kBlockVoxels && below <= kMaxBelowInMaterialBlock) {
        const double mean = sum / kBlockVoxels;
        const double variance = std::max(0.0, sum_sq / kBlockVoxels - mean * mean);
        blocks[blockOf(leaf->origin())] = {static_cast<float>(mean), static_cast<float>(variance)};
      }
    }

    // topologyUnion runs TBB tasks. Isolated, the waiting thread cannot pick up another brick,
    // whose body would change this thread's accumulator while the union is still writing it.
    tbb::this_task_arena::isolate([&] {
      Scan& local = partial_scans.local();
      local.candidates->tree().topologyUnion(candidates->tree());
      local.seeds->tree().topologyUnion(seeds->tree());
      local.blocks.merge(blocks);
      local.solid.merge(solid);
      local.partial.merge(partial);
    });
  });
  Scan scan;
  partial_scans.combine_each([&](Scan& local) {
    scan.candidates->tree().topologyUnion(local.candidates->tree());
    scan.seeds->tree().topologyUnion(local.seeds->tree());
    scan.blocks.merge(local.blocks);
    scan.solid.merge(local.solid);
    scan.partial.merge(local.partial);
  });
  return scan;
}

/// Connected components (26-neighbourhood) of the candidates that are not connected to a seed.
/// Counts the outside-air voxels per block in `outside` and marks them in `outside_air`.
std::vector<std::vector<Coord>> internalComponents(const Scan& scan, std::map<Coord, int>& outside,
                                                   openvdb::BoolGrid& outside_air) {
  auto outside_acc = outside_air.getAccessor();
  auto visited = openvdb::BoolGrid::create(false);
  auto visited_acc = visited->getAccessor();
  const auto candidate_acc = scan.candidates->getConstAccessor();
  std::deque<Coord> queue;

  const auto flood = [&](const Coord& start, std::vector<Coord>* component) {
    visited_acc.setValueOn(start);
    queue.push_back(start);
    while (!queue.empty()) {
      const Coord c = queue.front();
      queue.pop_front();
      if (component != nullptr) {
        component->push_back(c);
      } else {
        ++outside[blockOf(c)];
        outside_acc.setValueOn(c);
      }
      for (const Coord& d : neighbours26()) {
        const Coord n = c + d;
        if (candidate_acc.isValueOn(n) && !visited_acc.isValueOn(n)) {
          visited_acc.setValueOn(n);
          queue.push_back(n);
        }
      }
    }
  };

  // Outside air first, without keeping its voxels.
  for (auto it = scan.seeds->cbeginValueOn(); it; ++it) {
    if (!visited_acc.isValueOn(it.getCoord())) {
      flood(it.getCoord(), nullptr);
    }
  }
  std::vector<std::vector<Coord>> components;
  for (auto it = scan.candidates->cbeginValueOn(); it; ++it) {
    if (!visited_acc.isValueOn(it.getCoord())) {
      components.emplace_back();
      flood(it.getCoord(), &components.back());
    }
  }
  return components;
}

/// Material grey value as a function of the depth below the surface, which follows cupping from
/// beam hardening: median of the block means at each depth. Loosened zones are local and cover
/// only a small part of each depth, so the median ignores them.
///
/// Depth is measured at block resolution but with sub-block precision: each block's part fraction
/// (voxels that are neither removed nor outside air) places the surface between block centres,
/// and depth grows by one block per step inside.
class MaterialReference {
 public:
  MaterialReference(const Scan& scan, const std::map<Coord, int>& outside,
                    const std::map<Coord, BlockStats>& material, double fallback) {
    const auto fraction = [&](const Coord& block) {
      int part = 0;
      if (scan.solid.contains(block)) {
        part = kBlockVoxels;
      } else if (const auto it = scan.partial.find(block); it != scan.partial.end()) {
        part = it->second;
      }
      if (const auto it = outside.find(block); it != outside.end()) {
        part -= it->second;
      }
      return static_cast<double>(part) / kBlockVoxels;
    };

    // Seeds: blocks at least half inside with a neighbour that is not fully inside. For a planar
    // surface the depth of the block centre is 8 * (f_block + f_neighbour) - 4 voxels.
    using Item = std::pair<double, Coord>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
    std::set<Coord> inside;
    for (const Coord& block : scan.solid) {
      if (fraction(block) >= 0.5) {
        inside.insert(block);
      }
    }
    for (const auto& [block, unused] : scan.partial) {
      if (fraction(block) >= 0.5) {
        inside.insert(block);
      }
    }
    for (const Coord& block : inside) {
      const double own = fraction(block);
      double best = std::numeric_limits<double>::infinity();
      for (const Coord& d : face_neighbours) {
        const double neighbour = fraction(block + d);
        if (neighbour < 1.0) {
          best = std::min(best, kBlock * (own + neighbour) - kBlock / 2.0);
        }
      }
      if (best < std::numeric_limits<double>::infinity()) {
        depth_[block] = best;
        queue.emplace(best, block);
      }
    }
    while (!queue.empty()) {
      const auto [depth, block] = queue.top();
      queue.pop();
      if (depth > depth_[block]) {
        continue;
      }
      for (const Coord& d : face_neighbours) {
        const Coord n = block + d;
        if (!inside.contains(n)) {
          continue;
        }
        const auto it = depth_.find(n);
        if (it == depth_.end() || depth + kBlock < it->second) {
          depth_[n] = depth + kBlock;
          queue.emplace(depth + kBlock, n);
        }
      }
    }

    // Median per depth bin; bins with too few blocks are skipped and interpolated over.
    std::map<int, std::vector<float>> per_bin;
    for (const auto& [block, stats] : material) {
      if (const auto it = depth_.find(block); it != depth_.end()) {
        per_bin[static_cast<int>(it->second / kBinVoxels)].push_back(stats.mean);
      }
    }
    for (auto& [bin, means] : per_bin) {
      if (means.size() >= kMinBlocks) {
        levels_[(bin + 0.5) * kBinVoxels] = median(std::move(means));
      }
    }
    if (levels_.empty()) {
      levels_[0.0] = fallback;
    }
  }

  /// Material level for the block containing `voxel`, interpolated linearly in depth.
  [[nodiscard]] double at(const Coord& voxel) const {
    const auto it = depth_.find(blockOf(voxel));
    const double depth = it == depth_.end() ? 0.0 : it->second;
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
  std::map<Coord, double> depth_;
  std::map<double, double> levels_;  // bin centre depth -> material level
};

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
  const Scan scan = scanBricks(dataset);
  phase.reset();
  phase.emplace("pores and zones");
  std::map<Coord, int> outside;
  auto band = openvdb::BoolGrid::create(false);
  const std::vector<std::vector<Coord>> components = internalComponents(scan, outside, *band);
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
  result.pore_labels = openvdb::Int32Grid::create(0);
  auto label_acc = result.pore_labels->getAccessor();
  std::vector<openvdb::BoolGrid::Ptr> shells;
  auto all_shells = openvdb::BoolGrid::create(false);
  std::map<Coord, BlockStats> material_blocks = scan.blocks;
  for (const auto& component : components) {
    if (static_cast<std::int64_t>(component.size()) < options.min_pore_voxels) {
      continue;
    }
    auto shell = openvdb::BoolGrid::create(false);
    auto shell_acc = shell->getAccessor();
    for (const Coord& c : component) {
      shell_acc.setValueOn(c);
      label_acc.setValue(c, static_cast<int>(shells.size()) + 1);
    }
    openvdb::tools::dilateActiveValues(shell->tree(), 1, openvdb::tools::NN_FACE_EDGE_VERTEX);
    // Dilation prunes fully active leaves of large pores into tiles; iterate voxels, not tiles.
    shell->tree().voxelizeActiveTiles();
    for (auto it = shell->cbeginValueOn(); it; ++it) {
      material_blocks.erase(blockOf(it.getCoord()));
    }
    all_shells->tree().topologyUnion(shell->tree());
    DetectedPore pore;
    pore.id = static_cast<int>(shells.size()) + 1;
    pore.voxel_count = static_cast<std::int64_t>(component.size());
    result.pores.push_back(pore);
    shells.push_back(shell);
  }

  // Blocks at the surface or at a pore: sums over their voxels outside the pore shells, apart
  // for the surface band and the rest, in one more pass over the bricks.
  std::set<Coord> restricted;
  for (const auto& [block, unused] : scan.blocks) {
    if (!material_blocks.contains(block) || touches_band(block)) {
      restricted.insert(block);
    }
  }
  tbb::combinable<std::map<Coord, BlockSums>> partial_sums;
  dataset.forEachBrick(0, [&](const Index3&, const openvdb::FloatGrid& grid) {
    auto& local = partial_sums.local();
    const auto band_in_brick = band->getConstAccessor();
    const auto shells_in_brick = all_shells->getConstAccessor();
    for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
      const Coord block = blockOf(leaf->origin());
      if (!restricted.contains(block)) {
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
        }
      }
      local[block] = sums;
    }
  });
  std::map<Coord, BlockSums> block_sums;
  partial_sums.combine_each([&](const std::map<Coord, BlockSums>& local) {
    // Bricks do not share blocks.
    block_sums.insert(local.begin(), local.end());
  });

  // Material blocks at the surface: statistics of the voxels outside the band only.
  for (auto it = material_blocks.begin(); it != material_blocks.end();) {
    const auto sums = block_sums.find(it->first);
    if (sums == block_sums.end()) {
      ++it;
      continue;
    }
    const BlockSums& s = sums->second;
    if (s.count < kBlockVoxels / 4) {
      it = material_blocks.erase(it);
      continue;
    }
    const double mean = s.sum / s.count;
    it->second = {static_cast<float>(mean),
                  static_cast<float>(std::max(0.0, s.sum_sq / s.count - mean * mean))};
    ++it;
  }

  std::vector<float> means;
  std::vector<float> variances;
  for (const auto& [block, stats] : material_blocks) {
    means.push_back(stats.mean);
    variances.push_back(stats.variance);
  }
  result.material_level = static_cast<float>(means.empty() ? info.threshold : median(means));
  result.noise_sigma = std::sqrt(median(variances));
  const MaterialReference reference(scan, outside, material_blocks, result.material_level);

  // Pore volumes from the grey values of the pore and its shell.
  for (std::size_t i = 0; i < result.pores.size(); ++i) {
    DetectedPore& pore = result.pores[i];
    double void_sum = 0.0;
    double weight_sum = 0.0;
    std::array<double, 3> weighted{};
    std::array<double, 3> lo{1e18, 1e18, 1e18};
    std::array<double, 3> hi{-1e18, -1e18, -1e18};
    for (auto it = shells[i]->cbeginValueOn(); it; ++it) {
      const Coord c = it.getCoord();
      const auto p = toArray(c);
      const double material = reference.at(c);
      const auto value = dataset.sample(0, {c.x(), c.y(), c.z()});
      // Voxels outside the kept region are air.
      const double fraction = value ? (material - *value) / (material - air) : 1.0;
      void_sum += fraction;
      const double weight = std::clamp(fraction, 0.0, 1.0);
      weight_sum += weight;
      for (std::size_t a = 0; a < 3; ++a) {
        weighted[a] += weight * p[a];
        if (label_acc.getValue(c) == pore.id) {
          lo[a] = std::min(lo[a], p[a]);
          hi[a] = std::max(hi[a], p[a]);
        }
      }
    }
    pore.volume_mm3 = std::max(0.0, void_sum) * voxel_volume;
    for (std::size_t a = 0; a < 3; ++a) {
      pore.center_voxels[a] = weight_sum > 0.0 ? weighted[a] / weight_sum : 0.0;
    }
    pore.bounds = boundsOf(lo, hi);
    pore.equivalent_diameter_mm = std::cbrt(6.0 * pore.volume_mm3 / std::numbers::pi);
  }

  // Zones: blocks whose mean lies clearly below the local material level.
  std::map<Coord, double> deficits;
  auto flagged = openvdb::BoolGrid::create(false);
  auto flagged_acc = flagged->getAccessor();
  for (const auto& [block, stats] : scan.blocks) {
    const double material =
        reference.at(Coord(block.x() * kBlock, block.y() * kBlock, block.z() * kBlock));
    const double contrast = material - air;
    if (contrast <= 0.0) {
      continue;
    }
    double deficit = (material - stats.mean) / contrast;
    int counted = kBlockVoxels;
    if (const auto sums = block_sums.find(block); sums != block_sums.end()) {
      // The pore's own voids are in its volume already: sum the other voxels only. The surface
      // band is darkened by the air outside, so its voids cannot be told from its grey values;
      // they are taken at the void fraction of the rest of the block, over the band's share of
      // the part (its grey values above air, which the voids lower by that same fraction).
      const BlockSums& s = sums->second;
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
    deficits[block] = deficit;
    const double n = counted;
    const double sigma = result.noise_sigma * std::sqrt(n) / kBlockVoxels / contrast;
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
      counted[c] = deficits[c];
      for (const Coord& d : neighbours26()) {
        if (const auto rim = deficits.find(c + d);
            rim != deficits.end() && !flagged_acc.isValueOn(c + d)) {
          counted[c + d] = rim->second;
        }
      }
    }
    // Residual errors of the material level (for example from cupping) add up over the many rim
    // blocks, so measure the deficit against the blocks just outside the rim.
    std::vector<float> outer;
    for (const auto& [block, unused] : counted) {
      for (const Coord& d : neighbours26()) {
        const Coord n = block + d;
        if (const auto found = deficits.find(n);
            found != deficits.end() && !counted.contains(n) && !flagged_acc.isValueOn(n)) {
          outer.push_back(static_cast<float>(found->second));
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
      zone_acc.setValue(c, static_cast<float>(deficits[c]));
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
  dataset.forEachBrick(0, [&](const Index3&, const openvdb::FloatGrid& grid) {
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
  });
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

nlohmann::json toJson(const PorosityResult& result) {
  const VoxelSize& v = result.voxel_size;
  const auto mm = [&v](const std::array<double, 3>& voxels) { return v.toMm(voxels); };
  nlohmann::json pores = nlohmann::json::array();
  for (const DetectedPore& pore : result.pores) {
    pores.push_back({{"id", pore.id},
                     {"voxel_count", pore.voxel_count},
                     {"volume_mm3", pore.volume_mm3},
                     {"equivalent_diameter_mm", pore.equivalent_diameter_mm},
                     {"center_voxels", pore.center_voxels},
                     {"center_mm", mm(pore.center_voxels)},
                     {"bounds_min", pore.bounds.min},
                     {"bounds_max", pore.bounds.max}});
  }
  nlohmann::json zones = nlohmann::json::array();
  for (const PorosityZone& zone : result.zones) {
    zones.push_back({{"id", zone.id},
                     {"block_count", zone.block_count},
                     {"volume_mm3", zone.volume_mm3},
                     {"void_volume_mm3", zone.void_volume_mm3},
                     {"porosity", zone.porosity()},
                     {"center_voxels", zone.center_voxels},
                     {"center_mm", mm(zone.center_voxels)},
                     {"bounds_min", zone.bounds.min},
                     {"bounds_max", zone.bounds.max}});
  }
  return {{"voxel_size_mm", v},
          {"air_level", result.air_level},
          {"material_level", result.material_level},
          {"noise_sigma", result.noise_sigma},
          {"part_volume_mm3", result.part_volume_mm3},
          {"pore_volume_mm3", result.poreVolumeMm3()},
          {"zone_void_volume_mm3", result.zoneVoidVolumeMm3()},
          {"porosity", result.porosity()},
          {"pores", pores},
          {"zones", zones}};
}

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
  dataset.forEachBrick(0, [&](const Index3&, const openvdb::FloatGrid& grid) {
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

  std::array<Projection, 3> pores{Projection(0, dims, max_pixels), Projection(1, dims, max_pixels),
                                  Projection(2, dims, max_pixels)};
  if (result.pore_labels) {
    for (auto it = result.pore_labels->cbeginValueOn(); it; ++it) {
      for (auto& p : pores) {
        p.add(it.getCoord(), 1.0);
      }
    }
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
  const double air = result.air_level;
  const double contrast = std::max(1.0, static_cast<double>(result.material_level) - air);
  auto pores = openvdb::FloatGrid::create(0.0F);
  auto pore_acc = pores->getAccessor();
  if (result.pore_labels) {
    for (auto it = result.pore_labels->cbeginValueOn(); it; ++it) {
      const Coord c = it.getCoord();
      const auto value = dataset.sample(0, {c.x(), c.y(), c.z()});
      const double fraction = value ? (result.material_level - *value) / contrast : 1.0;
      pore_acc.setValue(c, static_cast<float>(std::clamp(fraction, 0.0, 1.0)));
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
  writeJson(dir / "porosity.json", toJson(result));
  openvdb::GridPtrVec grids;
  const auto add = [&grids](const openvdb::GridBase::Ptr& grid, const char* name) {
    if (grid) {
      auto copy = grid->deepCopyGrid();
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
  const nlohmann::json json = nlohmann::json::parse(in);
  PorosityResult result;
  result.voxel_size = json.at("voxel_size_mm").get<VoxelSize>();
  result.air_level = json.at("air_level").get<float>();
  result.material_level = json.at("material_level").get<float>();
  result.noise_sigma = json.at("noise_sigma").get<double>();
  result.part_volume_mm3 = json.at("part_volume_mm3").get<double>();
  for (const nlohmann::json& item : json.at("pores")) {
    DetectedPore pore;
    pore.id = item.at("id").get<int>();
    pore.voxel_count = item.at("voxel_count").get<std::int64_t>();
    pore.volume_mm3 = item.at("volume_mm3").get<double>();
    pore.equivalent_diameter_mm = item.at("equivalent_diameter_mm").get<double>();
    pore.center_voxels = item.at("center_voxels").get<std::array<double, 3>>();
    pore.bounds.min = item.at("bounds_min").get<std::array<std::int64_t, 3>>();
    pore.bounds.max = item.at("bounds_max").get<std::array<std::int64_t, 3>>();
    result.pores.push_back(pore);
  }
  for (const nlohmann::json& item : json.at("zones")) {
    PorosityZone zone;
    zone.id = item.at("id").get<int>();
    zone.block_count = item.at("block_count").get<std::int64_t>();
    zone.volume_mm3 = item.at("volume_mm3").get<double>();
    zone.void_volume_mm3 = item.at("void_volume_mm3").get<double>();
    zone.center_voxels = item.at("center_voxels").get<std::array<double, 3>>();
    zone.bounds.min = item.at("bounds_min").get<std::array<std::int64_t, 3>>();
    zone.bounds.max = item.at("bounds_max").get<std::array<std::int64_t, 3>>();
    result.zones.push_back(zone);
  }
  if (std::filesystem::exists(dir / "analysis.vdb")) {
    openvdb::initialize();
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
