// Stacked and wound prismatic cells: sections and the overhang.

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <utility>

#include "detail/battery.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve::detail::battery {
namespace {

/// Mean profile along the normal over a (2w+1)² column at (iu, iv); `noise` receives its noise
/// from the spread within the column.
std::vector<double> stackProfile(const VolumeView& volume, const StackData& data, std::int64_t iu,
                                 std::int64_t iv, int w, double& noise) {
  const VolumeInfo& info = volume.info();
  Box box;
  box.min = data.box.lo;
  box.max = data.box.hi;
  box.min[at(data.u)] = std::max<std::int64_t>(0, iu - w);
  box.max[at(data.u)] = std::min(info.dims[at(data.u)], iu + w + 1);
  box.min[at(data.v)] = std::max<std::int64_t>(0, iv - w);
  box.max[at(data.v)] = std::min(info.dims[at(data.v)], iv + w + 1);
  std::vector<float> values(at(box.voxelCount()));
  volume.readRegion(box, values, info.air_level);
  const std::int64_t len = box.size(at(data.n));
  std::vector<double> profile(at(len), 0.0);
  std::vector<double> sum_sq(at(len), 0.0);
  const Index3 size{box.size(0), box.size(1), box.size(2)};
  const auto count = static_cast<double>(box.voxelCount()) / static_cast<double>(len);
  for (std::int64_t z = 0; z < size[2]; ++z) {
    for (std::int64_t y = 0; y < size[1]; ++y) {
      for (std::int64_t x = 0; x < size[0]; ++x) {
        const Index3 c{x, y, z};
        const double v = values[at(x + size[0] * (y + size[1] * z))];
        profile[at(c[at(data.n)])] += v / count;
        sum_sq[at(c[at(data.n)])] += v * v / count;
      }
    }
  }
  std::vector<double> spread(at(len));
  for (std::size_t i = 0; i < spread.size(); ++i) {
    spread[i] = std::sqrt(std::max(0.0, sum_sq[i] - profile[i] * profile[i]));
  }
  noise = count > 1.0 ? median(spread) * std::sqrt(count / (count - 1.0)) / std::sqrt(count) : 0.0;
  return profile;
}

}  // namespace

void stackSections(const VolumeView& volume, const BatteryOptions& options, StackData& data,
                   BatteryResult& result) {
  const TelemetryPhase phase("profiles");
  options.progress.check();
  const VolumeInfo& info = volume.info();
  const VoxelSize& pitch = info.voxel_size;
  const auto air = static_cast<double>(info.air_level);
  const auto threshold = static_cast<double>(info.threshold);
  const int per_axis = std::max(1, options.sections);
  const double step = pitch[at(data.n)];
  const auto wall_window = static_cast<std::size_t>(std::max(4.0, std::ceil(0.5 / step)));
  const auto lo = data.box.lo;
  const auto hi = data.box.hi;
  struct Profile {
    std::vector<double> values;
    double noise = 0.0;
    Band low;
    Band high;
    std::vector<Peak> peaks;
  };
  std::vector<Profile> profiles(at(static_cast<std::int64_t>(per_axis) * per_axis));
  result.sections.resize(profiles.size());
  tbb::parallel_for(0, per_axis * per_axis, [&](int k) {
    const int i = k % per_axis;
    const int j = k / per_axis;
    const double fu = per_axis == 1 ? 0.5 : 0.2 + 0.6 * i / (per_axis - 1);
    const double fv = per_axis == 1 ? 0.5 : 0.2 + 0.6 * j / (per_axis - 1);
    const auto iu = static_cast<std::int64_t>(
        std::lround(static_cast<double>(lo[at(data.u)]) +
                    fu * static_cast<double>(hi[at(data.u)] - lo[at(data.u)] - 1)));
    const auto iv = static_cast<std::int64_t>(
        std::lround(static_cast<double>(lo[at(data.v)]) +
                    fv * static_cast<double>(hi[at(data.v)] - lo[at(data.v)] - 1)));
    Profile& profile = profiles[at(k)];
    profile.values = stackProfile(volume, data, iu, iv, options.average_voxels, profile.noise);
    profile.low = firstBand(profile.values, threshold, air, wall_window);
    std::vector<double> reversed(profile.values.rbegin(), profile.values.rend());
    profile.high = firstBand(reversed, threshold, air, wall_window);
    BatterySection& section = result.sections[at(k)];
    section.position_mm = {static_cast<double>(iu) * pitch[at(data.u)],
                           static_cast<double>(iv) * pitch[at(data.v)]};
    const auto last = static_cast<double>(profile.values.size() - 1);
    section.outer_mm = (last - profile.high.outer - profile.low.outer) * step;
    double wall_min = kNaN;
    double wall_sum = 0.0;
    double walls = 0.0;
    for (const Band* band : {&profile.low, &profile.high}) {
      if (!std::isnan(band->inner)) {
        const double wall = (band->inner - band->outer) * step;
        wall_min = nanMin(wall_min, wall);
        wall_sum += wall;
        walls += 1.0;
      }
    }
    section.min_wall_mm = wall_min;
    section.mean_wall_mm = walls > 0.0 ? wall_sum / walls : kNaN;
  });
  std::vector<double> heights;
  std::vector<double> valleys;
  for (Profile& profile : profiles) {
    const auto last = static_cast<double>(profile.values.size() - 1);
    const double from_f = std::isnan(profile.low.inner)
                              ? profile.low.outer + 2.0 * static_cast<double>(wall_window)
                              : profile.low.inner + 1.0;
    const double to_f = std::isnan(profile.high.inner)
                            ? last - profile.high.outer - 2.0 * static_cast<double>(wall_window)
                            : last - profile.high.inner - 1.0;
    if (to_f <= from_f + 3.0) {
      continue;
    }
    const auto from = static_cast<std::size_t>(std::ceil(from_f));
    const auto to = static_cast<std::size_t>(std::floor(to_f));
    const std::span<const double> segment(profile.values.data() + from, to - from);
    profile.peaks = findPeaks(profile.values, from, to, minProminence(segment, profile.noise));
    for (const Peak& peak : profile.peaks) {
      heights.push_back(peak.value);
    }
    collectValleys(profile.values, profile.peaks, valleys);
  }
  const Classes classes = classify(heights);
  result.levels.electrodes_distinct = classes.distinct;
  result.levels.cathode = options.cathode_brighter ? classes.high : classes.low;
  result.levels.anode = options.cathode_brighter ? classes.low : classes.high;
  result.levels.gap = median(valleys);
  std::vector<double> wall_peaks;
  for (const Profile& profile : profiles) {
    wall_peaks.push_back(profile.low.peak);
    wall_peaks.push_back(profile.high.peak);
  }
  result.levels.housing = median(wall_peaks);
  if (!classes.distinct) {
    result.warnings.emplace_back(
        "Anode and cathode layers do not differ in brightness: the layers found are counted as "
        "cathode layers, and the anode is looked for between them beyond the cathode ends");
  }
  std::vector<double> all_spacing;
  for (Profile& profile : profiles) {
    label(profile.peaks, classes, options.cathode_brighter);
    const ProfileLayers layers = countLayers(profile.peaks);
    all_spacing.insert(all_spacing.end(), layers.spacing.begin(), layers.spacing.end());
  }
  const double spacing_median = median(all_spacing);
  // Without two layers in any profile there is no stack: no pitch, and no layers to measure the
  // overhang of. The extent of the layers stays unknown, so the overhang is left out.
  const bool stack_found = !std::isnan(spacing_median);
  if (!stack_found) {
    result.warnings.emplace_back(
        "No electrode stack found: no section shows two or more layers, so the anode overhang "
        "is not measured. Is the volume a battery cell?");
  }
  data.half_pitch = stack_found ? 0.5 * spacing_median * step : 0.0;
  const auto n0 = static_cast<double>(lo[at(data.n)]) * step;
  for (std::size_t k = 0; k < profiles.size(); ++k) {
    const Profile& profile = profiles[k];
    BatterySection& section = result.sections[k];
    const ProfileLayers layers = countLayers(profile.peaks);
    section.cathode_layers = layers.cathodes;
    section.anode_layers = layers.anodes;
    section.pitch_mm = median(layers.cathode_spacing) * step;
    if (stack_found && !std::isnan(layers.first)) {
      section.stack_mm = (layers.last - layers.first) * step;
      data.first_layer = nanMin(data.first_layer, n0 + layers.first * step);
      data.last_layer = nanMax(data.last_layer, n0 + layers.last * step);
    }
    for (std::size_t i = 1; i < profile.peaks.size(); ++i) {
      const double spacing = profile.peaks[i].position - profile.peaks[i - 1].position;
      if (isGap(profile.values, profile.peaks[i - 1], profile.peaks[i], spacing_median,
                options.gap_factor, result.levels)) {
        LayerGap gap;
        gap.position_mm[at(data.u)] = section.position_mm[0];
        gap.position_mm[at(data.v)] = section.position_mm[1];
        gap.position_mm[at(data.n)] =
            n0 + 0.5 * (profile.peaks[i].position + profile.peaks[i - 1].position) * step;
        gap.size_mm = spacing * step;
        gap.expected_mm = spacing_median * step;
        gap.section = static_cast<int>(k);
        result.gaps.push_back(gap);
      }
    }
  }
}

/// Sections across the four edges of a stack, from the middle of the cell outwards.
void stackOverhang(const VolumeView& volume, const BatteryOptions& options, StackData& data,
                   BatteryResult& result, std::vector<SectionImage>& worst) {
  if (std::isnan(data.first_layer)) {
    return;
  }
  const TelemetryPhase phase("overhang");
  options.progress.check();
  const VolumeInfo& info = volume.info();
  const VoxelSize& pitch = info.voxel_size;
  const int per_edge = std::max(3, options.overhang_sections / 4);
  const double step_n = pitch[at(data.n)];
  const double margin = std::max(2.0 * data.half_pitch, 2.0 * step_n);
  const auto n_lo = std::max<std::int64_t>(
      0, static_cast<std::int64_t>(std::floor((data.first_layer - margin) / step_n)));
  const auto n_hi = std::min<std::int64_t>(
      info.dims[at(data.n)],
      static_cast<std::int64_t>(std::ceil((data.last_layer + margin) / step_n)) + 1);
  if (n_hi <= n_lo) {
    return;
  }
  bool unfinished = false;
  std::vector<int> hidden_edges;
  for (const int axis : {data.u, data.v}) {
    const int other = axis == data.u ? data.v : data.u;
    for (int side = 0; side < 2; ++side) {
      const int edge = static_cast<int>(result.edges.size());
      result.edges.push_back({axis, side, edgeName(axis, side)});
      const std::int64_t lo = data.box.lo[at(axis)];
      const std::int64_t hi = data.box.hi[at(axis)];
      const std::int64_t mid = (lo + hi) / 2;
      const std::int64_t rows = side == 0 ? mid - lo + 1 : hi - mid;
      std::vector<SectionImage> images(at(per_edge));
      std::vector<double> positions(at(per_edge));
      tbb::parallel_for(0, per_edge, [&](int j) {
        const double f = 0.1 + 0.8 * j / (per_edge - 1);
        const std::int64_t olo = data.box.lo[at(other)];
        const std::int64_t ohi = data.box.hi[at(other)];
        const auto io = static_cast<std::int64_t>(
            std::lround(static_cast<double>(olo) + f * static_cast<double>(ohi - olo - 1)));
        positions[at(j)] = static_cast<double>(io) * pitch[at(other)];
        Box box;
        box.min[at(data.n)] = n_lo;
        box.max[at(data.n)] = n_hi;
        box.min[at(axis)] = side == 0 ? lo : mid;
        box.max[at(axis)] = side == 0 ? mid + 1 : hi;
        box.min[at(other)] = std::max<std::int64_t>(0, io - options.average_voxels);
        box.max[at(other)] = std::min(info.dims[at(other)], io + options.average_voxels + 1);
        std::vector<float> values(at(box.voxelCount()));
        volume.readRegion(box, values, info.air_level);
        SectionImage& image = images[at(j)];
        image.across = n_hi - n_lo;
        image.along = rows;
        image.valid_across = image.across;
        image.across_from_mm = static_cast<double>(n_lo) * step_n;
        image.across_step_mm = step_n;
        image.along_from_mm = static_cast<double>(mid) * pitch[at(axis)];
        image.along_step_mm = (side == 0 ? -1.0 : 1.0) * pitch[at(axis)];
        image.values.assign(at(image.across * rows), 0.0F);
        const Index3 size{box.size(0), box.size(1), box.size(2)};
        const auto count = static_cast<float>(box.size(at(other)));
        for (std::int64_t z = 0; z < size[2]; ++z) {
          for (std::int64_t y = 0; y < size[1]; ++y) {
            for (std::int64_t x = 0; x < size[0]; ++x) {
              const Index3 c{x, y, z};
              const std::int64_t a = c[at(data.n)];
              const std::int64_t s = side == 0 ? (mid - lo) - c[at(axis)] : c[at(axis)];
              image.values[at(a + image.across * s)] +=
                  values[at(x + size[0] * (y + size[1] * z))] / count;
            }
          }
        }
      });
      std::vector<std::vector<OverhangSample>> per_section(at(per_edge));
      std::vector<char> open(at(per_edge), 0);
      std::vector<char> hidden(at(per_edge), 0);
      tbb::parallel_for(0, per_edge, [&](int j) {
        bool o = false;
        bool h = false;
        sectionOverhang(images[at(j)], result.levels, options, edge, positions[at(j)],
                        per_section[at(j)], o, h);
        open[at(j)] = o ? 1 : 0;
        hidden[at(j)] = h ? 1 : 0;
      });
      if (2 * std::count(hidden.begin(), hidden.end(), 1) > per_edge) {
        hidden_edges.push_back(edge);
      }
      warnStrayEnds(result, edge, dropStrayEnds(per_section, options.max_end_deviation_mm),
                    options.max_end_deviation_mm);
      std::int64_t worst_index = -1;
      double worst_value = std::numeric_limits<double>::infinity();
      for (int j = 0; j < per_edge; ++j) {
        for (const OverhangSample& s : per_section[at(j)]) {
          if (s.overhang_mm < worst_value) {
            worst_value = s.overhang_mm;
            worst_index = j;
          }
        }
        result.overhang.insert(result.overhang.end(), per_section[at(j)].begin(),
                               per_section[at(j)].end());
        unfinished = unfinished || open[at(j)] != 0;
      }
      worst.push_back(worst_index >= 0 ? std::move(images[at(worst_index)]) : SectionImage{});
    }
  }
  warnOverhang(result, unfinished, hidden_edges);
}

/// Outer size of the cell along `axis` on a line through `through` (level-0 voxel), averaged
/// over a small column: between the outer half-height edges of the housing at both ends.
double outerExtentMm(const VolumeView& volume, const Overview& box, int axis, const Index3& through,
                     int w) {
  const VolumeInfo& info = volume.info();
  Box line;
  for (std::size_t a = 0; a < 3; ++a) {
    if (static_cast<int>(a) == axis) {
      line.min[a] = box.lo[a];
      line.max[a] = box.hi[a];
    } else {
      line.min[a] = std::clamp<std::int64_t>(through[a] - w, 0, info.dims[a] - 1);
      line.max[a] = std::clamp<std::int64_t>(through[a] + w + 1, line.min[a] + 1, info.dims[a]);
    }
  }
  std::vector<float> values(at(line.voxelCount()));
  volume.readRegion(line, values, info.air_level);
  const std::int64_t len = line.size(at(axis));
  std::vector<double> profile(at(len), 0.0);
  const Index3 size{line.size(0), line.size(1), line.size(2)};
  const auto count = static_cast<double>(line.voxelCount()) / static_cast<double>(len);
  for (std::int64_t z = 0; z < size[2]; ++z) {
    for (std::int64_t y = 0; y < size[1]; ++y) {
      for (std::int64_t x = 0; x < size[0]; ++x) {
        const Index3 c{x, y, z};
        profile[at(c[at(axis)])] += values[at(x + size[0] * (y + size[1] * z))] / count;
      }
    }
  }
  const double pitch = info.voxel_size[at(axis)];
  const auto window = static_cast<std::size_t>(std::max(4.0, std::ceil(0.5 / pitch)));
  const auto threshold = static_cast<double>(info.threshold);
  const auto air = static_cast<double>(info.air_level);
  const Band low = firstBand(profile, threshold, air, window);
  std::vector<double> reversed(profile.rbegin(), profile.rend());
  const Band high = firstBand(reversed, threshold, air, window);
  const double extent = (static_cast<double>(len - 1) - high.outer - low.outer) * pitch;
  return std::isnan(extent) ? static_cast<double>(len) * pitch : extent;
}

/// Outer size of a cylindrical cell across (from `cross`) and along the axis through the
/// winding, between the core and the wall (from `along`).
void cylinderOuterSize(const VolumeView& cross, const VolumeView& along, const CylinderData& data,
                       BatteryResult& result) {
  const VoxelSize& pitch = cross.info().voxel_size;
  const int a = data.axis;
  const BatterySection& mid = result.sections[result.sections.size() / 2];
  Index3 through{};
  through[at(a)] = static_cast<std::int64_t>(std::lround(mid.position_mm[0] / pitch[at(a)]));
  through[at(data.u)] =
      static_cast<std::int64_t>(std::lround(mid.center_mm[0] / pitch[at(data.u)]));
  through[at(data.v)] =
      static_cast<std::int64_t>(std::lround(mid.center_mm[1] / pitch[at(data.v)]));
  result.outer_size_mm[at(data.u)] = outerExtentMm(cross, data.box, data.u, through, 1);
  result.outer_size_mm[at(data.v)] = outerExtentMm(cross, data.box, data.v, through, 1);
  const double r = 0.5 * (data.core[result.sections.size() / 2].front() +
                          data.inner[result.sections.size() / 2].front());
  through[at(data.u)] =
      static_cast<std::int64_t>(std::lround((mid.center_mm[0] + r) / pitch[at(data.u)]));
  result.outer_size_mm[at(a)] = outerExtentMm(along, data.box, a, through, 1);
  result.length_mm = result.outer_size_mm[at(a)];
}

}  // namespace voxelsieve::detail::battery
