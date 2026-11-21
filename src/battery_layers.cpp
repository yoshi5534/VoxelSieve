// Sections across an edge of the electrodes and the anode overhang.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <sstream>
#include <string>
#include <utility>

#include "detail/battery.hpp"

namespace voxelsieve::detail::battery {
namespace {

struct Track {
  double start = 0.0;  // across sample at row 0
  bool cathode = false;
  bool ended = false;
  double end = 0.0;  // row, sub-sample
  double across_end = 0.0;
  std::vector<std::int64_t> path;  // across sample per row
  std::vector<double> along;       // grey value on the track per row, smoothed
};

std::vector<Track> trackLayers(const SectionImage& image, const Classes& classes,
                               bool cathode_brighter, double gap_level) {
  std::vector<Track> tracks;
  if (image.along < 4 || image.valid_across < 4) {
    return tracks;
  }
  std::vector<double> start(at(image.valid_across), 0.0);
  const std::int64_t rows = std::min<std::int64_t>(4, image.along);
  for (std::int64_t s = 0; s < rows; ++s) {
    for (std::int64_t a = 0; a < image.valid_across; ++a) {
      start[at(a)] += image.value(a, s) / static_cast<double>(rows);
    }
  }
  // The rows see the same layers: their differences are noise.
  std::vector<double> diff;
  for (std::int64_t s = 0; s + 1 < rows; ++s) {
    for (std::int64_t a = 0; a < image.valid_across; ++a) {
      diff.push_back(image.value(a, s) - image.value(a, s + 1));
    }
  }
  const double m = median(diff);
  for (double& d : diff) {
    d = std::abs(d - m);
  }
  const double noise = diff.empty() ? 0.0
                                    : 1.4826 * median(diff) / std::numbers::sqrt2 /
                                          std::sqrt(static_cast<double>(rows));
  std::vector<Peak> peaks = findPeaks(start, 1, start.size() - 1, minProminence(start, noise));
  label(peaks, classes, cathode_brighter);
  std::vector<double> along(at(image.along));
  for (const Peak& peak : peaks) {
    Track track;
    track.start = peak.position;
    track.cathode = peak.cathode;
    const double end_level = 0.5 * (peak.value + gap_level);
    auto pos = static_cast<std::int64_t>(std::lround(peak.position));
    for (std::int64_t s = 0; s < image.along; ++s) {
      std::int64_t best = pos;
      double best_value = -std::numeric_limits<double>::infinity();
      for (std::int64_t a = std::max<std::int64_t>(0, pos - 1);
           a <= std::min(image.valid_across - 1, pos + 1); ++a) {
        if (image.value(a, s) > best_value) {
          best_value = image.value(a, s);
          best = a;
        }
      }
      along[at(s)] = best_value;
      if (best_value >= end_level) {
        pos = best;  // follows the layer by at most one sample per row
      }
      track.path.push_back(pos);
    }
    std::vector<double> smoothed = smooth3(along);
    for (std::int64_t s = 1; s + 1 < image.along; ++s) {
      if (smoothed[at(s)] < end_level && smoothed[at(s + 1)] < end_level) {
        const double before = smoothed[at(s - 1)];
        const double frac = (before - end_level) / std::max(1e-9, before - smoothed[at(s)]);
        track.ended = true;
        track.end = static_cast<double>(s - 1) + std::clamp(frac, 0.0, 1.0);
        break;
      }
    }
    track.across_end = static_cast<double>(pos);
    track.along = std::move(smoothed);
    tracks.push_back(std::move(track));
  }
  return tracks;
}

/// Overhang samples of the cathode tracks of one section.
void overhangOfSection(const SectionImage& image, const std::vector<Track>& tracks, int edge,
                       double section, std::vector<OverhangSample>& samples, bool& unfinished) {
  // Neighbours are anode layers at the usual distance; a layer much farther away belongs to
  // another part of the winding, as at the start and end of the strip.
  std::vector<double> spacing;
  for (std::size_t t = 1; t < tracks.size(); ++t) {
    spacing.push_back(tracks[t].start - tracks[t - 1].start);
  }
  const double usual = median(spacing);
  int layer = 0;
  for (std::size_t t = 0; t < tracks.size(); ++t) {
    if (!tracks[t].cathode) {
      continue;
    }
    const int this_layer = layer++;
    if (!tracks[t].ended) {
      unfinished = true;
      continue;
    }
    double overhang = std::numeric_limits<double>::infinity();
    double anode_end = kNaN;
    double anode_across = kNaN;
    for (const std::size_t n : {t - 1, t + 1}) {
      if (n >= tracks.size() || tracks[n].cathode ||
          std::abs(std::abs(tracks[n].start - tracks[t].start) - usual) > 0.4 * usual) {
        continue;  // t - 1 wraps for t == 0
      }
      if (!tracks[n].ended) {
        unfinished = true;
        continue;
      }
      const double o = (tracks[n].end - tracks[t].end) * std::abs(image.along_step_mm);
      if (o < overhang) {
        overhang = o;
        anode_end = tracks[n].end;
        anode_across = tracks[n].across_end;
      }
    }
    if (!std::isfinite(overhang)) {
      continue;
    }
    OverhangSample sample;
    sample.edge = edge;
    sample.section = section;
    sample.layer = this_layer;
    sample.across_mm = image.across_from_mm + tracks[t].start * image.across_step_mm;
    sample.cathode_end_mm = image.along_from_mm + tracks[t].end * image.along_step_mm;
    sample.anode_end_mm = image.along_from_mm + anode_end * image.along_step_mm;
    sample.anode_across_mm = image.across_from_mm + anode_across * image.across_step_mm;
    sample.overhang_mm = overhang;
    samples.push_back(sample);
  }
}

/// Overhang samples where only the cathode layers stand out in the winding, as with hard carbon
/// or graphite anodes between metal oxide cathodes in a reconstruction with little contrast:
/// every track is a cathode, and the anode is looked for in the middle between two neighbouring
/// cathodes. Beyond the cathode ends the anode stands between darker electrolyte, and it ends
/// where its grey value falls half way to that of the space beyond the cathode ends. Where the
/// anode does not stand out there by `min_contrast`, as when the space beyond the cathodes is
/// filled with other material, the pair is left out and `hidden` is set.
void overhangBetweenCathodes(const SectionImage& image, const std::vector<Track>& tracks, int edge,
                             double section, double min_contrast,
                             std::vector<OverhangSample>& samples, bool& unfinished, bool& hidden) {
  std::vector<double> spacing;
  for (std::size_t t = 1; t < tracks.size(); ++t) {
    spacing.push_back(tracks[t].start - tracks[t - 1].start);
  }
  const double usual = median(spacing);
  if (!(usual > 2.0)) {
    return;
  }
  // Windows in rows, relative to the layer pitch so that they mean the same at any resolution.
  const auto half_width = std::max<std::int64_t>(1, std::lround(0.12 * usual));
  const auto smoothing = std::max<std::int64_t>(1, std::lround(0.17 * usual));
  const auto run = std::max<std::int64_t>(2, std::lround(0.2 * usual));
  const auto probe = std::max<std::int64_t>(3, std::lround(0.3 * usual));
  // Anode end per pair of neighbouring cathodes, in rows; NaN where not found.
  std::vector<double> anode_end(tracks.size(), kNaN);
  std::vector<double> anode_across(tracks.size(), kNaN);
  int hidden_pairs = 0;
  int seen_pairs = 0;
  for (std::size_t t = 0; t + 1 < tracks.size(); ++t) {
    const Track& a = tracks[t];
    const Track& b = tracks[t + 1];
    if (std::abs((b.start - a.start) - usual) > 0.4 * usual) {
      continue;  // not neighbours in the winding
    }
    if (!a.ended || !b.ended) {
      unfinished = true;
      continue;
    }
    std::vector<double> mid(at(image.along), 0.0);
    for (std::int64_t s = 0; s < image.along; ++s) {
      const std::int64_t c = (a.path[at(s)] + b.path[at(s)]) / 2;
      double sum = 0.0;
      double count = 0.0;
      for (std::int64_t k = std::max<std::int64_t>(0, c - half_width);
           k <= std::min(image.valid_across - 1, c + half_width); ++k) {
        sum += image.value(k, s);
        count += 1.0;
      }
      mid[at(s)] = count > 0.0 ? sum / count : kNaN;
    }
    std::vector<double> smoothed(mid.size(), kNaN);
    for (std::int64_t s = 0; s < image.along; ++s) {
      double sum = 0.0;
      double count = 0.0;
      for (std::int64_t k = std::max<std::int64_t>(0, s - smoothing);
           k <= std::min(image.along - 1, s + smoothing); ++k) {
        sum += mid[at(k)];
        count += 1.0;
      }
      smoothed[at(s)] = sum / count;
    }
    const auto first = static_cast<std::int64_t>(std::ceil(std::min(a.end, b.end)));
    const auto last = static_cast<std::int64_t>(std::ceil(std::max(a.end, b.end)));
    std::vector<double> anode_level;
    std::vector<double> dark;
    for (std::int64_t s = last + 1; s <= last + probe && s < image.along; ++s) {
      anode_level.push_back(smoothed[at(s)]);
    }
    for (const Track* track : {&a, &b}) {
      const auto end = static_cast<std::int64_t>(std::ceil(track->end));
      for (std::int64_t s = end + 2; s <= end + 1 + probe && s < image.along; ++s) {
        dark.push_back(track->along[at(s)]);
      }
    }
    const double level = median(anode_level);
    const double beyond = median(dark);
    if (!(level - beyond >= min_contrast)) {
      ++hidden_pairs;
      continue;
    }
    ++seen_pairs;
    const double end_level = 0.5 * (level + beyond);
    for (std::int64_t s = std::max<std::int64_t>(first, 1); s + run < image.along; ++s) {
      bool below = true;
      for (std::int64_t k = s; k < s + run && below; ++k) {
        below = smoothed[at(k)] < end_level;
      }
      if (below && s <= first + 1) {
        break;  // dark right at the cathode end: the anode end is not resolved
      }
      if (below) {
        const double before = smoothed[at(s - 1)];
        const double frac = (before - end_level) / std::max(1e-9, before - smoothed[at(s)]);
        anode_end[t] = static_cast<double>(s - 1) + std::clamp(frac, 0.0, 1.0);
        anode_across[t] = 0.5 * static_cast<double>(a.path[at(s)] + b.path[at(s)]);
        break;
      }
    }
    if (std::isnan(anode_end[t])) {
      unfinished = true;
    }
  }
  // Hidden where the anode does not stand out between most pairs.
  hidden = hidden_pairs > seen_pairs;
  int layer = 0;
  for (std::size_t t = 0; t < tracks.size(); ++t) {
    const int this_layer = layer++;
    if (!tracks[t].ended) {
      continue;
    }
    double overhang = std::numeric_limits<double>::infinity();
    std::size_t pair = tracks.size();
    for (const std::size_t n : {t - 1, t}) {
      if (n < tracks.size() && !std::isnan(anode_end[n])) {  // t - 1 wraps for t == 0
        const double o = (anode_end[n] - tracks[t].end) * std::abs(image.along_step_mm);
        if (o < overhang) {
          overhang = o;
          pair = n;
        }
      }
    }
    if (pair == tracks.size()) {
      continue;
    }
    OverhangSample sample;
    sample.edge = edge;
    sample.section = section;
    sample.layer = this_layer;
    sample.across_mm = image.across_from_mm + tracks[t].start * image.across_step_mm;
    sample.cathode_end_mm = image.along_from_mm + tracks[t].end * image.along_step_mm;
    sample.anode_end_mm = image.along_from_mm + anode_end[pair] * image.along_step_mm;
    sample.anode_across_mm = image.across_from_mm + anode_across[pair] * image.across_step_mm;
    sample.overhang_mm = overhang;
    samples.push_back(sample);
  }
}

}  // namespace

/// Overhang samples of one section across an edge, with the anode as its own layers where the
/// electrodes differ in brightness, else between the cathodes.
void sectionOverhang(const SectionImage& image, const BatteryLevels& levels,
                     const BatteryOptions& options, int edge, double section,
                     std::vector<OverhangSample>& samples, bool& unfinished, bool& hidden) {
  Classes classes;
  classes.distinct = levels.electrodes_distinct;
  classes.split = 0.5 * (levels.anode + levels.cathode);
  classes.low = std::min(levels.anode, levels.cathode);
  classes.high = std::max(levels.anode, levels.cathode);
  const std::vector<Track> tracks =
      trackLayers(image, classes, options.cathode_brighter, levels.gap);
  if (levels.electrodes_distinct) {
    overhangOfSection(image, tracks, edge, section, samples, unfinished);
  } else {
    overhangBetweenCathodes(image, tracks, edge, section, 0.12 * (levels.cathode - levels.gap),
                            samples, unfinished, hidden);
  }
}

/// Leaves out the layer ends of one edge that lie far from the others: a layer lost by the
/// tracking, a tab or the start of a strip, not the edge of the electrode stack. Returns how many.
std::int64_t dropStrayEnds(std::vector<std::vector<OverhangSample>>& per_section, double limit) {
  std::vector<double> cathode;
  std::vector<double> anode;
  for (const auto& samples : per_section) {
    for (const OverhangSample& s : samples) {
      cathode.push_back(s.cathode_end_mm);
      anode.push_back(s.anode_end_mm);
    }
  }
  if (cathode.empty()) {
    return 0;
  }
  const double cathode_median = median(cathode);
  const double anode_median = median(anode);
  std::int64_t dropped = 0;
  for (auto& samples : per_section) {
    // A tab bent over the winding ends above its layer's neighbours in the same section; the
    // band next to it does not show the anode either.
    std::vector<int> tabs;
    for (const OverhangSample& s : samples) {
      std::vector<double> near;
      for (const OverhangSample& n : samples) {
        if (n.layer != s.layer && std::abs(n.layer - s.layer) <= 2) {
          near.push_back(n.cathode_end_mm);
        }
      }
      if (near.size() >= 2 && std::abs(s.cathode_end_mm - median(near)) > 0.5 * limit) {
        tabs.push_back(s.layer);
      }
    }
    const auto stray = [&](const OverhangSample& s) {
      return std::abs(s.cathode_end_mm - cathode_median) > limit ||
             std::abs(s.anode_end_mm - anode_median) > limit ||
             std::any_of(tabs.begin(), tabs.end(),
                         [&](int layer) { return std::abs(s.layer - layer) <= 1; });
    };
    const auto before = samples.size();
    samples.erase(std::remove_if(samples.begin(), samples.end(), stray), samples.end());
    dropped += static_cast<std::int64_t>(before - samples.size());
  }
  return dropped;
}

void warnStrayEnds(BatteryResult& result, int edge, std::int64_t dropped, double limit) {
  if (dropped > 0) {
    std::ostringstream text;
    text << "At " << result.edges[at(edge)].name << ", " << dropped << " layer ends more than "
         << limit << " mm from the edge of the electrode stack or more than " << 0.5 * limit
         << " mm from their neighbours (lost layers, tabs) are left out";
    result.warnings.push_back(text.str());
  }
}

/// Warnings about layer ends that were not measured.
void warnOverhang(BatteryResult& result, bool unfinished, const std::vector<int>& hidden_edges) {
  if (unfinished) {
    result.warnings.emplace_back(
        "Some electrode layers did not end inside the analysed sections, or their neighbours "
        "did not; their overhang is left out");
  }
  for (const int edge : hidden_edges) {
    result.warnings.push_back("At " + result.edges[at(edge)].name +
                              " the anode does not stand out from the material beyond the "
                              "cathode ends in most sections; the overhang there is not measured");
  }
}

void summarizeEdges(BatteryResult& result) {
  for (std::size_t e = 0; e < result.edges.size(); ++e) {
    BatteryEdge& edge = result.edges[e];
    double sum = 0.0;
    double sum_sq = 0.0;
    double cmin = kNaN;
    double cmax = kNaN;
    double amin = kNaN;
    double amax = kNaN;
    double anode_sum = 0.0;
    double cathode_sum = 0.0;
    edge.min_mm = kNaN;
    edge.max_mm = kNaN;
    for (std::size_t i = 0; i < result.overhang.size(); ++i) {
      const OverhangSample& s = result.overhang[i];
      if (s.edge != static_cast<int>(e)) {
        continue;
      }
      ++edge.samples;
      sum += s.overhang_mm;
      sum_sq += s.overhang_mm * s.overhang_mm;
      if (std::isnan(edge.min_mm) || s.overhang_mm < edge.min_mm) {
        edge.min_mm = s.overhang_mm;
        edge.worst = static_cast<std::int64_t>(i);
      }
      edge.max_mm = nanMax(edge.max_mm, s.overhang_mm);
      cmin = nanMin(cmin, s.cathode_end_mm);
      cmax = nanMax(cmax, s.cathode_end_mm);
      amin = nanMin(amin, s.anode_end_mm);
      amax = nanMax(amax, s.anode_end_mm);
      anode_sum += s.anode_end_mm;
      cathode_sum += s.cathode_end_mm;
    }
    if (edge.samples > 0) {
      const auto n = static_cast<double>(edge.samples);
      edge.mean_mm = sum / n;
      edge.std_mm = std::sqrt(std::max(0.0, sum_sq / n - edge.mean_mm * edge.mean_mm));
      edge.cathode_edge_spread_mm = cmax - cmin;
      edge.anode_edge_spread_mm = amax - amin;
      edge.anode_edge_mm = anode_sum / n;
      edge.cathode_edge_mm = cathode_sum / n;
    } else {
      edge.mean_mm = edge.std_mm = kNaN;
      edge.anode_edge_mm = edge.cathode_edge_mm = kNaN;
    }
    result.min_overhang_mm = nanMin(result.min_overhang_mm, edge.min_mm);
  }
}

std::string edgeName(int axis, int side) {
  return std::string(1, "xyz"[axis]) + (side == 0 ? " min" : " max");
}

}  // namespace voxelsieve::detail::battery
