// Cylindrical cells: sections around the axis and the overhang.

#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "detail/battery.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve::detail::battery {
namespace {

void fitCentreLine(CylinderData& data, const std::vector<BatterySection>& sections) {
  const auto n = static_cast<double>(sections.size());
  double sp = 0.0;
  double spp = 0.0;
  std::array<double, 2> sc{};
  std::array<double, 2> spc{};
  for (const BatterySection& s : sections) {
    const double p = s.position_mm[0];
    sp += p;
    spp += p * p;
    for (std::size_t k = 0; k < 2; ++k) {
      sc[k] += s.center_mm[k];
      spc[k] += p * s.center_mm[k];
    }
  }
  const double det = n * spp - sp * sp;
  for (std::size_t k = 0; k < 2; ++k) {
    if (sections.size() < 2 || std::abs(det) < 1e-12) {
      data.c1[k] = 0.0;
      data.c0[k] = sc[k] / n;
    } else {
      data.c1[k] = (n * spc[k] - sp * sc[k]) / det;
      data.c0[k] = (sc[k] - data.c1[k] * sp) / n;
    }
  }
}

}  // namespace

void cylinderSections(const VolumeView& volume, const BatteryOptions& options, CylinderData& data,
                      BatteryResult& result) {
  const TelemetryPhase phase("cross-sections");
  options.progress.check();
  const VolumeInfo& info = volume.info();
  const VoxelSize& pitch = info.voxel_size;
  const int a = data.axis;
  const auto lo = data.box.lo;
  const auto hi = data.box.hi;
  data.step = std::min(pitch[at(data.u)], pitch[at(data.v)]);
  const double half_u =
      0.5 * static_cast<double>(hi[at(data.u)] - lo[at(data.u)]) * pitch[at(data.u)];
  const double half_v =
      0.5 * static_cast<double>(hi[at(data.v)] - lo[at(data.v)]) * pitch[at(data.v)];
  data.ray_samples =
      static_cast<std::int64_t>(std::ceil(1.25 * std::hypot(half_u, half_v) / data.step));
  const int n_angles = std::max(8, options.angles);
  const int n_sections = std::max(1, options.sections);
  const auto air = static_cast<double>(info.air_level);
  const auto threshold = static_cast<double>(info.threshold);
  const auto wall_window =
      static_cast<std::size_t>(std::max(4.0, std::ceil(0.5 / data.step)));  // 0.5 mm

  struct Ray {
    std::vector<double> profile;
    Band wall;
    double outer = kNaN;  // samples
    double core = 0.0;    // samples
    double inner = kNaN;  // samples
    double noise = 0.0;
    std::vector<Peak> peaks;
  };
  std::vector<std::vector<Ray>> rays(at(n_sections));
  std::vector<Plane> planes(at(n_sections));
  result.sections.resize(at(n_sections));
  data.core.assign(at(n_sections), std::vector<double>(at(n_angles), kNaN));
  data.inner.assign(at(n_sections), std::vector<double>(at(n_angles), kNaN));
  tbb::parallel_for(0, n_sections, [&](int k) {
    const double fraction = n_sections == 1 ? 0.5 : 0.2 + 0.6 * k / (n_sections - 1);
    auto index = static_cast<std::int64_t>(
        std::lround(static_cast<double>(lo[at(a)]) +
                    fraction * static_cast<double>(hi[at(a)] - lo[at(a)] - 1)));
    if (a == 2 && !info.planes.empty()) {
      index = nearestPlane(info.planes, index);
    }
    planes[at(k)] = readPlane(volume, a, index, lo, hi, info.air_level);
    const Plane& plane = planes[at(k)];
    BatterySection& section = result.sections[at(k)];
    section.position_mm[0] = static_cast<double>(index) * pitch[at(a)];
    // Centre of the material in the slice, then of the circle through the outer surface.
    double sx = 0.0;
    double sy = 0.0;
    double count = 0.0;
    for (std::int64_t j = 0; j < plane.nv; ++j) {
      for (std::int64_t i = 0; i < plane.nu; ++i) {
        if (plane.voxel(i, j) >= info.threshold) {
          sx += static_cast<double>(plane.u0 + i) * plane.pu;
          sy += static_cast<double>(plane.v0 + j) * plane.pv;
          count += 1.0;
        }
      }
    }
    std::array<double, 2> centre{sx / std::max(1.0, count), sy / std::max(1.0, count)};
    std::vector<Ray>& section_rays = rays[at(k)];
    section_rays.assign(at(n_angles), Ray{});
    double radius = kNaN;
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<std::array<double, 2>> points;
      for (int j = 0; j < n_angles; ++j) {
        const double angle = 2.0 * kPi * j / n_angles;
        Ray& ray = section_rays[at(j)];
        ray.profile = rayProfile(plane, centre, angle, data.step, data.ray_samples,
                                 options.average_voxels, ray.noise);
        std::vector<double> reversed(ray.profile.rbegin(), ray.profile.rend());
        ray.wall = firstBand(reversed, threshold, air, wall_window);
        const auto last = static_cast<double>(ray.profile.size() - 1);
        ray.outer = last - ray.wall.outer;
        ray.inner = std::isnan(ray.wall.inner) ? kNaN : last - ray.wall.inner;
        if (!std::isnan(ray.outer)) {
          points.push_back({centre[0] + ray.outer * data.step * std::cos(angle),
                            centre[1] + ray.outer * data.step * std::sin(angle)});
        }
      }
      if (points.size() < 3) {
        throw std::runtime_error("No outer surface of the cell found");
      }
      const auto [fitted, r] = fitCircle(points);
      if (std::isnan(r)) {
        throw std::runtime_error("No outer surface of the cell found");
      }
      if (pass == 0) {
        centre = fitted;
      }
      radius = r;
    }
    section.center_mm = centre;
    section.outer_mm = 2.0 * radius;
    double rmin = kNaN;
    double rmax = kNaN;
    double wall_min = kNaN;
    double wall_sum = 0.0;
    double wall_count = 0.0;
    double core_sum = 0.0;
    double core_count = 0.0;
    double cmin = kNaN;
    double cmax = kNaN;
    std::vector<std::array<double, 2>> core_points;
    section.outer_radius_mm.assign(at(n_angles), kNaN);
    section.core_radius_mm.assign(at(n_angles), 0.0);
    for (int j = 0; j < n_angles; ++j) {
      Ray& ray = section_rays[at(j)];
      const double angle = 2.0 * kPi * j / n_angles;
      const double r_out = ray.outer * data.step;
      section.outer_radius_mm[at(j)] = r_out;
      rmin = nanMin(rmin, r_out);
      rmax = nanMax(rmax, r_out);
      if (!std::isnan(ray.inner)) {
        const double wall = (ray.outer - ray.inner) * data.step;
        wall_min = nanMin(wall_min, wall);
        wall_sum += wall;
        wall_count += 1.0;
      }
      // Hollow core: from the centre to the first material.
      const std::vector<double>& p = ray.profile;
      std::size_t c = 0;
      if (p.size() > 3 && (p[0] + p[1] + p[2]) / 3.0 < threshold) {
        while (c + 1 < p.size() && p[c + 1] < threshold) {
          ++c;
        }
        ray.core = static_cast<double>(c) + (threshold - p[c]) / std::max(1e-9, p[c + 1] - p[c]);
        core_points.push_back({centre[0] + ray.core * data.step * std::cos(angle),
                               centre[1] + ray.core * data.step * std::sin(angle)});
      }
      section.core_radius_mm[at(j)] = ray.core * data.step;
      data.core[at(k)][at(j)] = ray.core * data.step;
      core_sum += ray.core * data.step;
      core_count += 1.0;
      cmin = nanMin(cmin, ray.core * data.step);
      cmax = nanMax(cmax, ray.core * data.step);
      const double inner =
          std::isnan(ray.inner) ? ray.outer - 2.0 * static_cast<double>(wall_window) : ray.inner;
      data.inner[at(k)][at(j)] = inner * data.step;
    }
    section.out_of_roundness_mm = rmax - rmin;
    section.dent_mm = std::max(0.0, radius - rmin);
    section.min_wall_mm = wall_min;
    section.mean_wall_mm = wall_count > 0.0 ? wall_sum / wall_count : kNaN;
    section.core_diameter_mm = 2.0 * core_sum / std::max(1.0, core_count);
    section.core_out_of_roundness_mm = cmax - cmin;
    if (core_points.size() >= 3) {
      const auto [core_centre, core_r] = fitCircle(core_points);
      if (!std::isnan(core_r)) {
        section.core_offset_mm = std::hypot(core_centre[0] - centre[0], core_centre[1] - centre[1]);
      }
    }
  });

  // Layers: peaks between the core and the can wall; classes over all sections.
  std::vector<double> heights;
  std::vector<double> valleys;
  for (int k = 0; k < n_sections; ++k) {
    for (int j = 0; j < n_angles; ++j) {
      auto& ray = rays[at(k)][at(j)];
      const auto from = static_cast<std::size_t>(std::ceil(ray.core)) + 1;
      const double inner = data.inner[at(k)][at(j)] / data.step - 1.0;
      const auto to = static_cast<std::size_t>(std::max(0.0, std::floor(inner)));
      if (to <= from + 3) {
        continue;
      }
      const std::span<const double> segment(ray.profile.data() + from, to - from);
      ray.peaks = findPeaks(ray.profile, from, to, minProminence(segment, ray.noise));
      for (const Peak& peak : ray.peaks) {
        heights.push_back(peak.value);
      }
      collectValleys(ray.profile, ray.peaks, valleys);
    }
  }
  const Classes classes = classify(heights);
  result.levels.electrodes_distinct = classes.distinct;
  result.levels.cathode = options.cathode_brighter ? classes.high : classes.low;
  result.levels.anode = options.cathode_brighter ? classes.low : classes.high;
  result.levels.gap = median(valleys);
  std::vector<double> wall_peaks;
  for (const auto& section_rays : rays) {
    for (const auto& ray : section_rays) {
      wall_peaks.push_back(ray.wall.peak);
    }
  }
  result.levels.housing = median(wall_peaks);
  if (!classes.distinct) {
    result.warnings.emplace_back(
        "Anode and cathode layers do not differ in brightness: the layers found are counted as "
        "cathode layers, and the anode is looked for between them beyond the cathode ends");
  }

  std::vector<double> all_spacing;
  for (int k = 0; k < n_sections; ++k) {
    for (auto& ray : rays[at(k)]) {
      label(ray.peaks, classes, options.cathode_brighter);
      const ProfileLayers layers = countLayers(ray.peaks);
      all_spacing.insert(all_spacing.end(), layers.spacing.begin(), layers.spacing.end());
    }
  }
  const double spacing_median = median(all_spacing);
  data.pitch = spacing_median * data.step;
  data.first_layer.assign(at(n_sections), std::vector<double>(at(n_angles), kNaN));
  data.last_layer.assign(at(n_sections), std::vector<double>(at(n_angles), kNaN));
  for (int k = 0; k < n_sections; ++k) {
    BatterySection& section = result.sections[at(k)];
    std::vector<int> cathodes;
    std::vector<int> anodes;
    std::vector<double> pitches;
    std::vector<double> outermost;
    for (int j = 0; j < n_angles; ++j) {
      const auto& ray = rays[at(k)][at(j)];
      const double angle = 2.0 * kPi * j / n_angles;
      const ProfileLayers layers = countLayers(ray.peaks);
      cathodes.push_back(layers.cathodes);
      anodes.push_back(layers.anodes);
      data.first_layer[at(k)][at(j)] = layers.first * data.step;
      data.last_layer[at(k)][at(j)] = layers.last * data.step;
      pitches.insert(pitches.end(), layers.cathode_spacing.begin(), layers.cathode_spacing.end());
      if (!std::isnan(layers.last)) {
        outermost.push_back(layers.last * data.step);
      }
      // Not next to the innermost and outermost layer, where the electrode strips start and
      // end and a layer is missing over part of the turn.
      for (std::size_t i = 2; i + 1 < ray.peaks.size(); ++i) {
        const double spacing = ray.peaks[i].position - ray.peaks[i - 1].position;
        if (isGap(ray.profile, ray.peaks[i - 1], ray.peaks[i], spacing_median, options.gap_factor,
                  result.levels)) {
          const double r = 0.5 * (ray.peaks[i].position + ray.peaks[i - 1].position) * data.step;
          LayerGap gap;
          gap.position_mm[at(a)] = section.position_mm[0];
          gap.position_mm[at(data.u)] = section.center_mm[0] + r * std::cos(angle);
          gap.position_mm[at(data.v)] = section.center_mm[1] + r * std::sin(angle);
          gap.size_mm = spacing * data.step;
          gap.expected_mm = spacing_median * data.step;
          gap.section = k;
          result.gaps.push_back(gap);
        }
      }
      if (k == n_sections / 2) {
        for (const Peak& peak : ray.peaks) {
          const double r = peak.position * data.step;
          data.points.push_back({{section.center_mm[0] + r * std::cos(angle),
                                  section.center_mm[1] + r * std::sin(angle)},
                                 peak.cathode});
        }
      }
    }
    section.cathode_layers = medianInt(cathodes);
    section.anode_layers = medianInt(anodes);
    section.pitch_mm = median(pitches) * data.step;
    section.stack_mm = 2.0 * median(outermost);
  }
  data.section_pos.clear();
  for (const BatterySection& s : result.sections) {
    data.section_pos.push_back(s.position_mm[0]);
  }
  fitCentreLine(data, result.sections);
}

/// Overhang samples of the sections across one edge of a cylindrical cell, one per angle (in
/// degrees); keeps the section of the smallest overhang in `worst`.
void edgeOverhang(std::vector<SectionImage>& images, const std::vector<double>& angles, int edge,
                  const BatteryOptions& options, BatteryResult& result, bool& unfinished,
                  std::vector<int>& hidden_edges, std::vector<SectionImage>& worst) {
  const auto n = static_cast<int>(images.size());
  std::vector<std::vector<OverhangSample>> per_angle(at(n));
  std::vector<char> per_unfinished(at(n), 0);
  std::vector<char> per_hidden(at(n), 0);
  tbb::parallel_for(0, n, [&](int j) {
    bool open = false;
    bool hide = false;
    sectionOverhang(images[at(j)], result.levels, options, edge, angles[at(j)], per_angle[at(j)],
                    open, hide);
    per_unfinished[at(j)] = open ? 1 : 0;
    per_hidden[at(j)] = hide ? 1 : 0;
  });
  warnStrayEnds(result, edge, dropStrayEnds(per_angle, options.max_end_deviation_mm),
                options.max_end_deviation_mm);
  // Fewer layer ends than sections: what is left are stray ends, not the edge.
  std::size_t kept = 0;
  for (const auto& samples : per_angle) {
    kept += samples.size();
  }
  if (kept > 0 && kept < per_angle.size()) {
    for (auto& samples : per_angle) {
      samples.clear();
    }
    result.warnings.push_back("At " + result.edges[at(edge)].name + " only " +
                              std::to_string(kept) +
                              " layer ends were found, fewer than one per section; the overhang "
                              "there is not measured");
  }
  int hidden_sections = 0;
  for (int j = 0; j < n; ++j) {
    result.overhang.insert(result.overhang.end(), per_angle[at(j)].begin(), per_angle[at(j)].end());
    unfinished = unfinished || per_unfinished[at(j)] != 0;
    hidden_sections += per_hidden[at(j)];
  }
  if (2 * hidden_sections > n) {
    hidden_edges.push_back(edge);
  }
  std::int64_t worst_index = -1;
  double worst_value = std::numeric_limits<double>::infinity();
  for (const OverhangSample& s : result.overhang) {
    if (s.edge == edge && s.overhang_mm < worst_value) {
      worst_value = s.overhang_mm;
      worst_index = std::find(angles.begin(), angles.end(), s.section) - angles.begin();
    }
  }
  worst.push_back(worst_index >= 0 && worst_index < n ? std::move(images[at(worst_index)])
                                                      : SectionImage{});
}

/// Sections across both ends of a cylindrical cell, one per angle, from a quarter of the length
/// inside the cell outwards to the end of the can.
void cylinderOverhang(const VolumeView& volume, const BatteryOptions& options, CylinderData& data,
                      BatteryResult& result, std::vector<SectionImage>& worst) {
  const TelemetryPhase phase("overhang");
  options.progress.check();
  const VolumeInfo& info = volume.info();
  const VoxelSize& pitch = info.voxel_size;
  const int a = data.axis;
  const int n_angles = std::max(1, options.overhang_sections);
  const auto lo = data.box.lo;
  const auto hi = data.box.hi;
  // Across: from inside the core to the wall, the same samples for every angle.
  double core_min = kNaN;
  double inner_max = kNaN;
  for (std::size_t k = 0; k < data.core.size(); ++k) {
    for (std::size_t j = 0; j < data.core[k].size(); ++j) {
      core_min = nanMin(core_min, data.core[k][j]);
      inner_max = nanMax(inner_max, data.inner[k][j]);
    }
  }
  const double across_from = std::max(0.0, core_min - 2.0 * data.step);
  const auto across = static_cast<std::int64_t>(std::ceil((inner_max - across_from) / data.step));
  const std::int64_t length = hi[at(a)] - lo[at(a)];
  const std::int64_t depth = std::max<std::int64_t>(8, length / 4);
  bool unfinished = false;
  std::vector<int> hidden_edges;
  for (int side = 0; side < 2; ++side) {
    const int edge = static_cast<int>(result.edges.size());
    result.edges.push_back({a, side, edgeName(a, side)});
    const std::int64_t start = side == 0 ? lo[at(a)] + depth : hi[at(a)] - 1 - depth;
    const std::int64_t rows = depth + 1;
    std::vector<SectionImage> images(at(n_angles));
    for (int j = 0; j < n_angles; ++j) {
      SectionImage& image = images[at(j)];
      image.across = across;
      image.along = rows;
      image.across_from_mm = across_from;
      image.across_step_mm = data.step;
      image.along_from_mm = static_cast<double>(start) * pitch[at(a)];
      image.along_step_mm = (side == 0 ? -1.0 : 1.0) * pitch[at(a)];
      image.values.assign(at(across * rows), info.air_level);
      // Valid up to the can wall at this angle (smallest over the sections).
      const double angle_deg = 360.0 * j / n_angles;
      const auto bin = static_cast<std::size_t>(std::lround(
                           angle_deg / 360.0 * static_cast<double>(data.inner.front().size()))) %
                       data.inner.front().size();
      double wall = kNaN;
      for (const auto& inner : data.inner) {
        wall = nanMin(wall, inner[bin]);
      }
      image.valid_across = std::clamp<std::int64_t>(
          static_cast<std::int64_t>(std::floor((wall - across_from) / data.step)) - 1, 0, across);
    }
    // One plane buffer per thread for all its rows, instead of a new one per row. The rows go in
    // chunks of 128, each in parallel: all threads read the bricks of one or two brick layers,
    // which the cache holds, instead of rows from all over the cell at once (X3).
    tbb::enumerable_thread_specific<Plane> planes;
    constexpr std::int64_t kRowChunk = 128;
    for (std::int64_t chunk = 0; chunk < rows; chunk += kRowChunk) {
      tbb::parallel_for(
          tbb::blocked_range<std::int64_t>(chunk, std::min(rows, chunk + kRowChunk)),
          [&](const tbb::blocked_range<std::int64_t>& range) {
            Plane& plane = planes.local();
            for (std::int64_t s = range.begin(); s != range.end(); ++s) {
              const std::int64_t index = start + (side == 0 ? -s : s);
              readPlaneInto(plane, volume, a, index, lo, hi, info.air_level);
              const auto centre = data.centre(static_cast<double>(index) * pitch[at(a)]);
              for (int j = 0; j < n_angles; ++j) {
                const double angle = 2.0 * kPi * j / n_angles;
                const double c = std::cos(angle);
                const double sn = std::sin(angle);
                SectionImage& image = images[at(j)];
                for (std::int64_t i = 0; i < across; ++i) {
                  const double r = across_from + static_cast<double>(i) * data.step;
                  double sum = 0.0;
                  for (int t = -options.average_voxels; t <= options.average_voxels; ++t) {
                    const double off = t * data.step;
                    sum += plane.sample(centre[0] + r * c - off * sn, centre[1] + r * sn + off * c);
                  }
                  image.values[at(i + across * s)] =
                      static_cast<float>(sum / (2 * options.average_voxels + 1));
                }
              }
            }
          });
    }
    std::vector<double> angles(at(n_angles));
    for (int j = 0; j < n_angles; ++j) {
      angles[at(j)] = 360.0 * j / n_angles;
    }
    edgeOverhang(images, angles, edge, options, result, unfinished, hidden_edges, worst);
  }
  warnOverhang(result, unfinished, hidden_edges);
}

/// Takes the measured ends of the electrodes along the axis, or well inside the can, clear of
/// the tabs and the cap, where they were not measured.
void cylinderEnds(CylinderData& data, const BatteryResult& result) {
  for (const BatteryEdge& edge : result.edges) {
    if (edge.samples > 0) {
      (edge.side == 0 ? data.electrode_low : data.electrode_high) = edge.anode_edge_mm;
      // Voids inside the shortest cathode, clear of the electrolyte beyond the cathode ends.
      (edge.side == 0 ? data.cathode_low : data.cathode_high) =
          edge.cathode_edge_mm + (edge.side == 0 ? 1.0 : -1.0) * edge.cathode_edge_spread_mm;
    }
  }
  const double low = result.bounds_min_mm[at(data.axis)] + 0.1 * result.length_mm;
  const double high = result.bounds_max_mm[at(data.axis)] - 0.1 * result.length_mm;
  data.cathode_low = std::isnan(data.cathode_low) ? low : data.cathode_low;
  data.cathode_high = std::isnan(data.cathode_high) ? high : data.cathode_high;
  data.electrode_low = std::isnan(data.electrode_low) ? data.cathode_low : data.electrode_low;
  data.electrode_high = std::isnan(data.electrode_high) ? data.cathode_high : data.electrode_high;
}

/// Where particles and voids are looked for in a cylindrical cell: in the winding, between the
/// electrode ends along the axis.
InRegion cylinderRegion(const CylinderData& data, const VoxelSize& pitch) {
  const double margin = 2.0 * pitch[at(data.axis)];
  const auto n_angles = static_cast<double>(data.core.front().size());
  // The innermost turn of each section: the strips start somewhere on it, and the turns inside
  // and next to it hold folded separator and electrolyte that look like gas.
  std::vector<double> first_turn;
  for (const std::vector<double>& radii : data.first_layer) {
    double r = kNaN;
    for (const double first : radii) {
      r = nanMax(r, first);
    }
    first_turn.push_back(r);
  }
  // The outermost turn of each section, half a pitch outside the median of its radius: beyond it,
  // at the ends, the can bends inwards and its wall can look like a particle.
  std::vector<double> last_turn;
  for (const std::vector<double>& radii : data.last_layer) {
    std::vector<double> found;
    std::copy_if(radii.begin(), radii.end(), std::back_inserter(found),
                 [](double r) { return !std::isnan(r); });
    last_turn.push_back(found.empty() ? kNaN : median(found) + 0.5 * data.pitch);
  }
  return [data, margin, pitch, n_angles, first_turn, last_turn](const Index3& v, bool bright) {
    const auto pos = static_cast<double>(v[at(data.axis)]) * pitch[at(data.axis)];
    const double from = bright ? data.electrode_low : data.cathode_low;
    const double to = bright ? data.electrode_high : data.cathode_high;
    if (pos < from + margin || pos > to - margin) {
      return false;
    }
    const auto c = data.centre(pos);
    const auto du = static_cast<double>(v[at(data.u)]) * pitch[at(data.u)] - c[0];
    const auto dv = static_cast<double>(v[at(data.v)]) * pitch[at(data.v)] - c[1];
    const double r = std::hypot(du, dv);
    double angle = std::atan2(dv, du);
    if (angle < 0.0) {
      angle += 2.0 * kPi;
    }
    const auto bins = static_cast<std::size_t>(n_angles);
    const auto bin = static_cast<std::size_t>(std::lround(angle / (2.0 * kPi) * n_angles)) % bins;
    std::size_t k = 0;
    for (std::size_t i = 1; i < data.section_pos.size(); ++i) {
      if (std::abs(data.section_pos[i] - pos) < std::abs(data.section_pos[k] - pos)) {
        k = i;
      }
    }
    // Between the innermost and the outermost layer at this angle, where there are layers; a
    // particle may sit half a layer pitch outside them. Voids not in the first and last turn,
    // where the electrolyte at the start and end of the strips looks like gas.
    const double first =
        bright || std::isnan(first_turn[k]) ? data.first_layer[k][bin] : first_turn[k];
    const double last = data.last_layer[k][bin];
    const double slack = bright ? 0.5 * data.pitch : -data.pitch;
    const double lo = std::isnan(first) ? data.core[k][bin] + 2.0 * data.step : first - slack;
    const double hi = std::isnan(last) ? data.inner[k][bin] - 2.0 * data.step : last + slack;
    const double outermost = std::isnan(last_turn[k]) ? hi : last_turn[k] + slack;
    return r > lo && r < hi && r < outermost && r < data.inner[k][bin] - 2.0 * data.step;
  };
}

/// Radius and angle (cylindrical) or position along the normal (stacked) of the findings.
void placeFindings(BatteryResult& result) {
  for (auto* list : {&result.particles, &result.voids}) {
    for (CellFinding& f : *list) {
      if (result.type == CellType::kCylindrical) {
        const auto [u, v] = planeAxes(result.axis);
        double best = std::numeric_limits<double>::infinity();
        std::array<double, 2> c{};
        for (const BatterySection& s : result.sections) {
          const double d = std::abs(s.position_mm[0] - f.center_mm[at(result.axis)]);
          if (d < best) {
            best = d;
            c = s.center_mm;
          }
        }
        const double du = f.center_mm[at(u)] - c[0];
        const double dv = f.center_mm[at(v)] - c[1];
        f.radius_mm = std::hypot(du, dv);
        f.angle_deg = std::atan2(dv, du) * 180.0 / kPi;
        if (f.angle_deg < 0.0) {
          f.angle_deg += 360.0;
        }
      } else {
        f.radius_mm = f.center_mm[at(result.axis)];
      }
    }
  }
}

}  // namespace voxelsieve::detail::battery
