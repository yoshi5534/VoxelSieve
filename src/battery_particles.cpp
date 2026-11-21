// Particles and voids in a cell, and the layers in a profile.

#include <tbb/combinable.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "detail/battery.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve::detail::battery {

std::vector<CellFinding> components(std::vector<std::pair<std::uint64_t, float>> voxels,
                                    const Index3& dims, const VoxelSize& pitch,
                                    std::int64_t min_voxels, bool bright) {
  std::sort(voxels.begin(), voxels.end());
  const auto key = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
    return static_cast<std::uint64_t>(x + dims[0] * (y + dims[1] * z));
  };
  std::vector<std::size_t> parent(voxels.size());
  std::iota(parent.begin(), parent.end(), 0);
  const auto find = [&](std::size_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  const auto lookup = [&](std::uint64_t k) -> std::size_t {
    const auto it =
        std::lower_bound(voxels.begin(), voxels.end(), std::pair{k, -1e30F},
                         [](const auto& a, const auto& b) { return a.first < b.first; });
    return it != voxels.end() && it->first == k ? static_cast<std::size_t>(it - voxels.begin())
                                                : voxels.size();
  };
  for (std::size_t i = 0; i < voxels.size(); ++i) {
    const auto k = static_cast<std::int64_t>(voxels[i].first);
    const std::int64_t x = k % dims[0];
    const std::int64_t y = (k / dims[0]) % dims[1];
    const std::int64_t z = k / (dims[0] * dims[1]);
    // Neighbours with a larger key: +x, +y, +z.
    const std::array<std::array<std::int64_t, 3>, 3> next{
        {{x + 1, y, z}, {x, y + 1, z}, {x, y, z + 1}}};
    for (const auto& n : next) {
      if (n[0] >= dims[0] || n[1] >= dims[1] || n[2] >= dims[2]) {
        continue;
      }
      const std::size_t j = lookup(key(n[0], n[1], n[2]));
      if (j < voxels.size()) {
        parent[find(i)] = find(j);
      }
    }
  }
  struct Acc {
    std::int64_t count = 0;
    Vec3 sum{};
    Index3 lo{std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::max(),
              std::numeric_limits<std::int64_t>::max()};
    Index3 hi{std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::min(),
              std::numeric_limits<std::int64_t>::min()};
    double extreme = 0.0;
  };
  std::vector<Acc> acc(voxels.size());
  for (std::size_t i = 0; i < voxels.size(); ++i) {
    Acc& a = acc[find(i)];
    const auto k = static_cast<std::int64_t>(voxels[i].first);
    const Index3 c{k % dims[0], (k / dims[0]) % dims[1], k / (dims[0] * dims[1])};
    if (a.count == 0) {
      a.extreme = voxels[i].second;
    }
    ++a.count;
    for (std::size_t d = 0; d < 3; ++d) {
      a.sum[d] += static_cast<double>(c[d]);
      a.lo[d] = std::min(a.lo[d], c[d]);
      a.hi[d] = std::max(a.hi[d], c[d]);
    }
    a.extreme = bright ? std::max(a.extreme, static_cast<double>(voxels[i].second))
                       : std::min(a.extreme, static_cast<double>(voxels[i].second));
  }
  std::vector<CellFinding> findings;
  for (std::size_t i = 0; i < voxels.size(); ++i) {
    const Acc& a = acc[i];
    if (parent[i] != i || a.count < min_voxels) {
      continue;
    }
    CellFinding f;
    f.voxels = a.count;
    f.volume_mm3 = static_cast<double>(a.count) * pitch.volumeMm3();
    f.equivalent_diameter_mm = std::cbrt(6.0 * f.volume_mm3 / kPi);
    for (std::size_t d = 0; d < 3; ++d) {
      f.center_mm[d] = a.sum[d] / static_cast<double>(a.count) * pitch[d];
      f.size_mm = std::max(f.size_mm, static_cast<double>(a.hi[d] - a.lo[d] + 1) * pitch[d]);
    }
    f.extreme_value = a.extreme;
    findings.push_back(f);
  }
  std::sort(findings.begin(), findings.end(),
            [](const CellFinding& a, const CellFinding& b) { return a.voxels > b.voxels; });
  for (std::size_t i = 0; i < findings.size(); ++i) {
    findings[i].id = static_cast<int>(i + 1);
  }
  return findings;
}

/// Moves bright components too large for particles out of the particles: tabs, foil ends and
/// welds reach into the electrode stack. Numbers the rest from 1.
void separateStructures(BatteryResult& result, const BatteryOptions& options) {
  const auto large = [&](const CellFinding& f) { return f.size_mm > options.max_particle_size_mm; };
  std::vector<CellFinding> structures;
  std::copy_if(result.particles.begin(), result.particles.end(), std::back_inserter(structures),
               large);
  const auto n = static_cast<std::int64_t>(structures.size());
  result.bright_structures += n;
  // Small bright pieces within the extent of a structure are parts of it, where its brightness
  // dips below the particle threshold.
  const auto part_of_structure = [&](const CellFinding& f) {
    return large(f) || std::any_of(structures.begin(), structures.end(), [&](const auto& t) {
             return std::hypot(f.center_mm[0] - t.center_mm[0], f.center_mm[1] - t.center_mm[1],
                               f.center_mm[2] - t.center_mm[2]) < 0.5 * t.size_mm;
           });
  };
  result.particles.erase(
      std::remove_if(result.particles.begin(), result.particles.end(), part_of_structure),
      result.particles.end());
  for (std::size_t i = 0; i < result.particles.size(); ++i) {
    result.particles[i].id = static_cast<int>(i + 1);
  }
  if (n > 0) {
    result.warnings.push_back(std::to_string(n) +
                              " bright structures longer than the largest particle (tabs, foil "
                              "ends, welds) are not counted as particles");
  }
}

void findParticlesAndVoids(const Dataset& dataset, const InRegion& in_region, BatteryResult& result,
                           const BatteryOptions& options) {
  const TelemetryPhase phase("particles and voids");
  options.progress.check();
  const DatasetInfo& info = dataset.info();
  using Found = std::vector<std::pair<std::uint64_t, float>>;
  tbb::combinable<Found> bright;
  tbb::combinable<Found> dark;
  const auto particle = static_cast<float>(result.particle_threshold);
  const auto gas = static_cast<float>(result.void_threshold);
  const Index3 dims = info.dims;
  // The limit holds for all threads together, so that no more than about kMaxFindingVoxels pile
  // up before the error: counted per thread, each of them could collect that many.
  std::atomic<std::size_t> found{0};
  std::atomic<bool> too_many{false};
  dataset.forEachGrid(
      0,
      [&](const Index3& /*brick*/, const auto& grid) {
        Found& b = bright.local();
        Found& d = dark.local();
        for (auto leaf = grid.tree().cbeginLeaf(); leaf && !too_many; ++leaf) {
          const std::size_t before = b.size() + d.size();
          for (auto it = leaf->cbeginValueOn(); it; ++it) {
            const auto value = static_cast<float>(*it);
            if (value < particle && value > gas) {
              continue;
            }
            const auto c = it.getCoord();
            const Index3 v{c.x(), c.y(), c.z()};
            if (!in_region(v, value >= particle)) {
              continue;
            }
            const auto key = static_cast<std::uint64_t>(v[0] + dims[0] * (v[1] + dims[1] * v[2]));
            (value >= particle ? b : d).emplace_back(key, value);
          }
          if (found.fetch_add(b.size() + d.size() - before) + (b.size() + d.size() - before) >
              kMaxFindingVoxels) {
            too_many = true;
          }
        }
      },
      BrickSweep{.progress = options.progress});
  if (too_many) {
    throw std::runtime_error(
        "Too many voxels above the particle threshold or below the void threshold in the "
        "electrode stack; set the thresholds by hand");
  }
  Found all_bright;
  Found all_dark;
  bright.combine_each(
      [&](const Found& f) { all_bright.insert(all_bright.end(), f.begin(), f.end()); });
  dark.combine_each([&](const Found& f) { all_dark.insert(all_dark.end(), f.begin(), f.end()); });
  if (all_bright.size() + all_dark.size() > kMaxFindingVoxels) {
    throw std::runtime_error(
        "Too many voxels above the particle threshold or below the void threshold in the "
        "electrode stack; set the thresholds by hand");
  }
  result.particles =
      components(std::move(all_bright), dims, info.voxel_size, options.min_particle_voxels, true);
  result.voids =
      components(std::move(all_dark), dims, info.voxel_size, options.min_void_voxels, false);
  separateStructures(result, options);
}

void setThresholds(BatteryResult& result, const BatteryOptions& options) {
  const BatteryLevels& l = result.levels;
  const double brightest = std::max(l.cathode, l.anode);
  result.particle_threshold = options.particle_threshold
                                  ? static_cast<double>(*options.particle_threshold)
                                  : brightest + (brightest - l.gap);
  result.void_threshold = options.void_threshold ? static_cast<double>(*options.void_threshold)
                                                 : l.air + 0.5 * (l.gap - l.air);
}

void summarizeSections(BatteryResult& result) {
  std::vector<int> cathodes;
  std::vector<int> anodes;
  std::vector<double> pitches;
  double outer_sum = 0.0;
  result.outer_min_size_mm = kNaN;
  result.outer_max_size_mm = kNaN;
  result.min_wall_mm = kNaN;
  for (const BatterySection& s : result.sections) {
    cathodes.push_back(s.cathode_layers);
    anodes.push_back(s.anode_layers);
    pitches.push_back(s.pitch_mm);
    outer_sum += s.outer_mm;
    result.outer_min_size_mm = nanMin(result.outer_min_size_mm, s.outer_mm);
    result.outer_max_size_mm = nanMax(result.outer_max_size_mm, s.outer_mm);
    result.max_out_of_roundness_mm =
        std::max(result.max_out_of_roundness_mm, s.out_of_roundness_mm);
    result.max_dent_mm = std::max(result.max_dent_mm, s.dent_mm);
    result.min_wall_mm = nanMin(result.min_wall_mm, s.min_wall_mm);
    result.max_core_out_of_roundness_mm =
        std::max(result.max_core_out_of_roundness_mm, s.core_out_of_roundness_mm);
  }
  result.cathode_layers = medianInt(cathodes);
  result.anode_layers = medianInt(anodes);
  result.pitch_mm = median(pitches);
  result.outer_mean_mm =
      result.sections.empty() ? kNaN : outer_sum / static_cast<double>(result.sections.size());
}

ProfileLayers countLayers(const std::vector<Peak>& peaks) {
  ProfileLayers p;
  double last_cathode = kNaN;
  for (std::size_t i = 0; i < peaks.size(); ++i) {
    if (peaks[i].cathode) {
      ++p.cathodes;
      if (!std::isnan(last_cathode)) {
        p.cathode_spacing.push_back(peaks[i].position - last_cathode);
      }
      last_cathode = peaks[i].position;
    } else {
      ++p.anodes;
    }
    if (i > 0) {
      p.spacing.push_back(peaks[i].position - peaks[i - 1].position);
    }
  }
  if (!peaks.empty()) {
    p.first = peaks.front().position;
    p.last = peaks.back().position;
  }
  return p;
}

/// Whether the space between two neighbouring layers is a gap: much wider than usual and dark
/// in its middle, not a layer the peak search missed.
bool isGap(std::span<const double> profile, const Peak& a, const Peak& b, double usual,
           double factor, const BatteryLevels& levels) {
  const double spacing = b.position - a.position;
  if (!(spacing > factor * usual)) {
    return false;
  }
  const auto from = static_cast<std::size_t>(std::ceil(a.position + 0.25 * spacing));
  const auto to = static_cast<std::size_t>(std::floor(b.position - 0.25 * spacing));
  double sum = 0.0;
  double count = 0.0;
  for (std::size_t i = from; i <= to && i < profile.size(); ++i) {
    sum += profile[i];
    count += 1.0;
  }
  const double dark = levels.gap + 0.5 * (std::min(levels.anode, levels.cathode) - levels.gap);
  return count > 0.0 && sum / count < dark;
}

}  // namespace voxelsieve::detail::battery
