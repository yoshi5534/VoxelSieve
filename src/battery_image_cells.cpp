// Cylindrical cells given as section images instead of a volume.

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "detail/battery.hpp"
#include "detail/blocks.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve::detail::battery {
namespace {

/// The section images of a cell placed in a volume: x and y as in the cross-sections, z along
/// the rows of the longitudinal sections from the bottom, all with the pixel size.
struct ImageCell {
  std::vector<const CellImage*> along;    // longitudinal sections, ascending angle
  std::vector<const CellImage*> cross;    // cross-sections, ascending position
  std::vector<std::int64_t> cross_index;  // z index of each cross-section
  std::vector<std::size_t> nearest;       // per z index: the nearest cross-section
  std::vector<double> axis_column;        // per longitudinal section: column of the axis
  double pixel = 1.0;
  Index3 dims{};
  float air = 0.0F;
  float threshold = 0.0F;

  [[nodiscard]] std::int64_t height() const { return dims[2]; }
};

/// Column of the axis in a longitudinal section: the middle between the outer surfaces over the
/// middle third of its rows.
double axisColumn(const CellImage& image, float threshold) {
  std::vector<double> centres;
  const auto above = [&](std::int64_t row, std::int64_t col) {
    return static_cast<float>(image.values[at(col + image.width * row)]) >= threshold;
  };
  for (std::int64_t row = image.height / 3; row < 2 * image.height / 3; row += 8) {
    std::int64_t first = -1;
    std::int64_t last = -1;
    for (std::int64_t col = 0; col + 1 < image.width; ++col) {
      if (above(row, col) && above(row, col + 1)) {
        first = first < 0 ? col : first;
        last = col + 1;
      }
    }
    if (first >= 0) {
      centres.push_back(0.5 * static_cast<double>(first + last));
    }
  }
  if (centres.empty()) {
    throw std::runtime_error("No cell found in the longitudinal section at " +
                             std::to_string(image.position) + " degrees");
  }
  return median(centres);
}

ImageCell placeImages(const CellImages& images, const BatteryOptions& options,
                      std::vector<std::string>& warnings) {
  if (images.longitudinal.empty() || images.cross.size() < 2) {
    throw std::invalid_argument(
        "A cell given as images needs longitudinal sections and at least two cross-sections");
  }
  ImageCell cell;
  for (const CellImage& image : images.longitudinal) {
    cell.along.push_back(&image);
  }
  for (const CellImage& image : images.cross) {
    cell.cross.push_back(&image);
  }
  std::sort(cell.along.begin(), cell.along.end(),
            [](const CellImage* a, const CellImage* b) { return a->position < b->position; });
  std::sort(cell.cross.begin(), cell.cross.end(),
            [](const CellImage* a, const CellImage* b) { return a->position < b->position; });
  const CellImage& first_along = *cell.along.front();
  const CellImage& first_cross = *cell.cross.front();
  for (const CellImage* image : cell.along) {
    if (image->width != first_along.width || image->height != first_along.height) {
      throw std::invalid_argument("The longitudinal sections differ in size");
    }
  }
  for (const CellImage* image : cell.cross) {
    if (image->width != first_cross.width || image->height != first_cross.height) {
      throw std::invalid_argument("The cross-sections differ in size");
    }
  }
  if (first_along.height < 16 || first_cross.width < 16 || first_cross.height < 16) {
    throw std::invalid_argument("The section images are too small");
  }
  cell.dims = {first_cross.width, first_cross.height, first_along.height};
  const double span = cell.cross.back()->position - cell.cross.front()->position;
  if (options.image_pixel_mm) {
    cell.pixel = *options.image_pixel_mm;
  } else {
    cell.pixel = span / static_cast<double>(cell.height() - 1);
    std::ostringstream text;
    text << "The pixel size, " << std::setprecision(4) << cell.pixel * 1000.0
         << " µm, is taken from the positions of the cross-sections, which are assumed to span "
            "the height of the longitudinal sections";
    warnings.push_back(text.str());
  }
  if (!(cell.pixel > 0.0) || !std::isfinite(cell.pixel)) {
    throw std::invalid_argument("The pixel size of the section images must be positive");
  }
  for (const CellImage* image : cell.cross) {
    cell.cross_index.push_back(std::clamp<std::int64_t>(
        std::lround((image->position - cell.cross.front()->position) / cell.pixel), 0,
        cell.height() - 1));
  }
  cell.nearest.resize(at(cell.height()));
  std::size_t c = 0;
  for (std::int64_t k = 0; k < cell.height(); ++k) {
    while (c + 1 < cell.cross_index.size() &&
           std::abs(cell.cross_index[c + 1] - k) <= std::abs(cell.cross_index[c] - k)) {
      ++c;
    }
    cell.nearest[at(k)] = c;
  }
  // Air and threshold from the middle cross-section.
  const CellImage& middle = *cell.cross[cell.cross.size() / 2];
  detail::Histogram histogram(detail::kHistogramBins, 0);
  for (const std::uint16_t value : middle.values) {
    ++histogram[value];
  }
  const detail::ThresholdResult split = detail::airThreshold(histogram);
  cell.air = split.air_level;
  cell.threshold = split.threshold;
  for (const CellImage* image : cell.along) {
    cell.axis_column.push_back(axisColumn(*image, cell.threshold));
  }
  return cell;
}

VolumeView crossView(const ImageCell& cell) {
  std::vector<std::int64_t> planes = cell.cross_index;
  planes.erase(std::unique(planes.begin(), planes.end()), planes.end());
  return VolumeView({cell.dims, VoxelSize(cell.pixel), cell.air, cell.threshold, planes},
                    [&cell](const Box& box, std::span<float> out, float fill) {
                      std::size_t o = 0;
                      for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
                        const CellImage* image =
                            z < 0 || z >= cell.height() ? nullptr : cell.cross[cell.nearest[at(z)]];
                        for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
                          for (std::int64_t x = box.min[0]; x < box.max[0]; ++x) {
                            const bool inside = image != nullptr && x >= 0 && y >= 0 &&
                                                x < image->width && y < image->height;
                            out[o++] =
                                inside ? static_cast<float>(image->values[at(x + image->width * y)])
                                       : fill;
                          }
                        }
                      }
                    });
}

/// The longitudinal section at the first angle as a volume: the same in every plane normal to y,
/// its axis at the centre `centre_x` (mm) of the cross-sections.
VolumeView alongView(const ImageCell& cell, double centre_x) {
  const CellImage& image = *cell.along.front();
  const auto shift =
      static_cast<std::int64_t>(std::lround(cell.axis_column.front() - centre_x / cell.pixel));
  return VolumeView(
      {cell.dims, VoxelSize(cell.pixel), cell.air, cell.threshold, {}},
      [&image, shift](const Box& box, std::span<float> out, float fill) {
        std::size_t o = 0;
        for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
          const std::int64_t row = image.height - 1 - z;
          for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
            for (std::int64_t x = box.min[0]; x < box.max[0]; ++x) {
              const std::int64_t col = x + shift;
              const bool inside = row >= 0 && row < image.height && col >= 0 && col < image.width;
              out[o++] =
                  inside ? static_cast<float>(image.values[at(col + image.width * row)]) : fill;
            }
          }
        }
      });
}

/// Bounding box: across from the middle cross-section, along the axis from the longitudinal
/// sections.
Overview imageOverview(const ImageCell& cell) {
  const auto bounds = [](const std::vector<std::int64_t>& counts) {
    const std::int64_t most = *std::max_element(counts.begin(), counts.end());
    const std::int64_t enough = std::max<std::int64_t>(2, most / 50);
    std::int64_t lo = -1;
    std::int64_t hi = -1;
    for (std::size_t i = 0; i < counts.size(); ++i) {
      if (counts[i] >= enough) {
        lo = lo < 0 ? static_cast<std::int64_t>(i) : lo;
        hi = static_cast<std::int64_t>(i) + 1;
      }
    }
    if (lo < 0) {
      throw std::invalid_argument("The section images show no material above the air threshold");
    }
    return std::pair{lo, hi};
  };
  const CellImage& middle = *cell.cross[cell.cross.size() / 2];
  std::vector<std::int64_t> cols(at(middle.width), 0);
  std::vector<std::int64_t> rows(at(middle.height), 0);
  for (std::int64_t y = 0; y < middle.height; ++y) {
    for (std::int64_t x = 0; x < middle.width; ++x) {
      if (static_cast<float>(middle.values[at(x + middle.width * y)]) >= cell.threshold) {
        ++cols[at(x)];
        ++rows[at(y)];
      }
    }
  }
  std::vector<std::int64_t> levels(at(cell.height()), 0);
  for (const CellImage* image : cell.along) {
    for (std::int64_t row = 0; row < image->height; ++row) {
      for (std::int64_t col = 0; col < image->width; ++col) {
        if (static_cast<float>(image->values[at(col + image->width * row)]) >= cell.threshold) {
          ++levels[at(image->height - 1 - row)];
        }
      }
    }
  }
  Overview o;
  const std::array<std::pair<std::int64_t, std::int64_t>, 3> b{bounds(cols), bounds(rows),
                                                               bounds(levels)};
  for (std::size_t a = 0; a < 3; ++a) {
    o.lo[a] = std::max<std::int64_t>(0, b[a].first - 2);
    o.hi[a] = std::min(cell.dims[a], b[a].second + 2);
  }
  o.type = CellType::kCylindrical;
  o.axis = 2;
  return o;
}

/// Overhang at both ends from the longitudinal sections, both halves of each.
void imageOverhang(const ImageCell& cell, const BatteryOptions& options, const CylinderData& data,
                   BatteryResult& result, std::vector<SectionImage>& worst) {
  const TelemetryPhase phase("overhang");
  options.progress.check();
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
  const std::int64_t length = data.box.hi[2] - data.box.lo[2];
  const std::int64_t depth = std::max<std::int64_t>(8, length / 4);
  // Directions: the right half of each section at its angle, the left half opposite.
  struct Direction {
    std::size_t image = 0;
    double sign = 1.0;
    double angle = 0.0;
  };
  std::vector<Direction> directions;
  for (std::size_t i = 0; i < cell.along.size(); ++i) {
    for (int half = 0; half < 2; ++half) {
      double angle = std::fmod(cell.along[i]->position + 180.0 * half, 360.0);
      angle = angle < 0.0 ? angle + 360.0 : angle;
      directions.push_back({i, half == 0 ? 1.0 : -1.0, angle});
    }
  }
  std::vector<double> angles;
  angles.reserve(directions.size());
  for (const Direction& d : directions) {
    angles.push_back(d.angle);
  }
  const std::size_t bins = data.inner.front().size();
  bool unfinished = false;
  std::vector<int> hidden_edges;
  for (int side = 0; side < 2; ++side) {
    const int edge = static_cast<int>(result.edges.size());
    result.edges.push_back({2, side, edgeName(2, side)});
    const std::int64_t start = side == 0 ? data.box.lo[2] + depth : data.box.hi[2] - 1 - depth;
    const std::int64_t rows = depth + 1;
    std::vector<SectionImage> images(directions.size());
    tbb::parallel_for(std::size_t{0}, directions.size(), [&](std::size_t j) {
      const Direction& d = directions[j];
      const CellImage& source = *cell.along[d.image];
      const double axis = cell.axis_column[d.image];
      SectionImage& image = images[j];
      image.across = across;
      image.along = rows;
      image.across_from_mm = across_from;
      image.across_step_mm = data.step;
      image.along_from_mm = static_cast<double>(start) * cell.pixel;
      image.along_step_mm = (side == 0 ? -1.0 : 1.0) * cell.pixel;
      image.values.assign(at(across * rows), cell.air);
      const auto bin =
          static_cast<std::size_t>(std::lround(d.angle / 360.0 * static_cast<double>(bins))) % bins;
      double wall = kNaN;
      for (const auto& inner : data.inner) {
        wall = nanMin(wall, inner[bin]);
      }
      image.valid_across = std::clamp<std::int64_t>(
          static_cast<std::int64_t>(std::floor((wall - across_from) / data.step)) - 1, 0, across);
      for (std::int64_t s = 0; s < rows; ++s) {
        const std::int64_t row = source.height - 1 - (start + (side == 0 ? -s : s));
        if (row < 0 || row >= source.height) {
          continue;
        }
        const std::uint16_t* line = source.values.data() + at(source.width * row);
        for (std::int64_t i = 0; i < across; ++i) {
          const double r = (across_from + static_cast<double>(i) * data.step) / cell.pixel;
          const double col = axis + d.sign * r;
          const double base = std::floor(col);
          const auto c0 = static_cast<std::int64_t>(base);
          if (c0 < 0 || c0 + 1 >= source.width) {
            continue;
          }
          const double t = col - base;
          image.values[at(i + across * s)] = static_cast<float>(
              (1.0 - t) * static_cast<double>(line[c0]) + t * static_cast<double>(line[c0 + 1]));
        }
      }
    });
    edgeOverhang(images, angles, edge, options, result, unfinished, hidden_edges, worst);
  }
  warnOverhang(result, unfinished, hidden_edges);
}

/// The pixels at or below `gas` in columns [from, to) connected to the seed pixels (4-connected):
/// the hollow core, not a void.
std::vector<char> darkFrom(const CellImage& image, std::int64_t from, std::int64_t to, float gas,
                           const std::vector<std::int64_t>& seeds) {
  std::vector<char> mask(at(image.width * image.height), 0);
  const auto dark = [&](std::int64_t i) { return static_cast<float>(image.values[at(i)]) <= gas; };
  std::vector<std::int64_t> stack;
  for (const std::int64_t seed : seeds) {
    if (dark(seed) && mask[at(seed)] == 0) {
      mask[at(seed)] = 1;
      stack.push_back(seed);
    }
  }
  while (!stack.empty()) {
    const std::int64_t i = stack.back();
    stack.pop_back();
    const std::int64_t x = i % image.width;
    const std::int64_t y = i / image.width;
    const std::array<std::array<std::int64_t, 2>, 4> next{
        {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}}};
    for (const auto& n : next) {
      if (n[0] < from || n[0] >= to || n[1] < 0 || n[1] >= image.height) {
        continue;
      }
      const std::int64_t j = n[0] + image.width * n[1];
      if (mask[at(j)] == 0 && dark(j)) {
        mask[at(j)] = 1;
        stack.push_back(j);
      }
    }
  }
  return mask;
}

/// Particle thresholds of one section image, one per band of rows: `global`, or higher where the
/// brightest layers of the band reach it with their noise. Section images often grow brighter or
/// darker along the cell, and a threshold from the levels of the whole cell then takes the noise
/// on the brightest layers for thousands of particles. `samples` are grey values in the electrode
/// stack per band, `steps` differences between pixels two apart along the layers, which see the
/// same layer and, unlike direct neighbours, mostly independent noise.
std::vector<double> imageParticleThresholds(std::vector<std::vector<double>> samples,
                                            std::vector<double> steps, double global) {
  constexpr std::size_t kMinSamples = 500;
  // Noise above the 99th percentile of the stack, that is above nearly every layer pixel.
  constexpr double kNoiseMargin = 6.0;
  std::vector<double> thresholds(samples.size(), global);
  if (steps.size() < kMinSamples) {
    return thresholds;
  }
  for (double& step : steps) {
    step = std::abs(step);
  }
  // Median absolute difference of two pixels: 0.6745 * sqrt(2) noise.
  const double noise = median(std::move(steps)) / (0.6745 * std::numbers::sqrt2);
  // Bands with few samples take the levels of the whole image.
  std::vector<double> all;
  for (const std::vector<double>& band : samples) {
    all.insert(all.end(), band.begin(), band.end());
  }
  if (all.size() < kMinSamples) {
    return thresholds;
  }
  const double whole = quantile(all, 0.99);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double top = samples[i].size() >= kMinSamples ? quantile(samples[i], 0.99) : whole;
    thresholds[i] = std::max(global, top + kNoiseMargin * noise);
  }
  return thresholds;
}

/// Inside the winding the gaps hold electrolyte, and nearly no pixel of the stack is as dark as
/// gas. Where the electrode ends were not measured, the region searched can reach beyond them,
/// into cross-sections where the electrodes end one after another and the space between them
/// looks like gas: the innermost of the cross-sections at either end with more than a percent of
/// their stack as dark as gas marks an end, and voids near or beyond it are dropped.
void leaveOutVoidsBeyondTheEnds(const ImageCell& cell, const std::vector<double>& gas_fraction,
                                std::vector<std::vector<CellFinding>>& dark,
                                BatteryResult& result) {
  constexpr double kMaxGasFraction = 0.01;
  constexpr double kEndMarginMm = 0.5;
  std::vector<std::size_t> inside;
  for (std::size_t c = 0; c < gas_fraction.size(); ++c) {
    if (!std::isnan(gas_fraction[c])) {
      inside.push_back(c);
    }
  }
  if (inside.empty()) {
    return;
  }
  const std::size_t mid = inside[inside.size() / 2];
  double low = -std::numeric_limits<double>::infinity();
  double high = std::numeric_limits<double>::infinity();
  // An end only where every cross-section beyond it is as dark too: a large void in the middle
  // darkens a few cross-sections, with clear ones beyond.
  const auto beyond = [&](std::size_t c) { return !(gas_fraction[c] <= kMaxGasFraction); };
  for (std::size_t c = 0; c < mid && beyond(c); ++c) {
    if (!std::isnan(gas_fraction[c])) {
      low = static_cast<double>(cell.cross_index[c]) * cell.pixel + kEndMarginMm;
    }
  }
  for (std::size_t c = gas_fraction.size() - 1; c > mid && beyond(c); --c) {
    if (!std::isnan(gas_fraction[c])) {
      high = static_cast<double>(cell.cross_index[c]) * cell.pixel - kEndMarginMm;
    }
  }
  std::size_t dropped = 0;
  for (std::vector<CellFinding>& found : dark) {
    dropped += std::erase_if(
        found, [&](const CellFinding& f) { return f.center_mm[2] < low || f.center_mm[2] > high; });
  }
  if (dropped > 0) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << "Cross-sections";
    if (std::isfinite(low)) {
      text << " below z " << low - kEndMarginMm << " mm";
    }
    if (std::isfinite(low) && std::isfinite(high)) {
      text << " and";
    }
    if (std::isfinite(high)) {
      text << " above z " << high + kEndMarginMm << " mm";
    }
    text << " are as dark as gas in more than 1 % of the electrode stack, where the electrodes "
            "end; "
         << dropped << " void sections within " << kEndMarginMm << " mm of the ends or beyond "
         << "are not counted";
    result.warnings.push_back(text.str());
  }
}

/// Particles and voids as their sections in the images; one finding seen in several images is
/// kept once, from the image where it is largest.
void imageFindings(const ImageCell& cell, const InRegion& in_region, const CylinderData& data,
                   BatteryResult& result, const BatteryOptions& options) {
  const TelemetryPhase phase("particles and voids");
  options.progress.check();
  const auto particle = static_cast<float>(result.particle_threshold);
  const auto gas = static_cast<float>(result.void_threshold);
  const VoxelSize pitch(cell.pixel);
  using Found = std::vector<std::pair<std::uint64_t, float>>;
  // Jobs: every cross-section, and each half of every longitudinal section.
  const std::size_t n_cross = cell.cross.size();
  const std::size_t n_jobs = n_cross + 2 * cell.along.size();
  std::vector<std::vector<CellFinding>> bright(n_jobs);
  std::vector<std::vector<CellFinding>> dark(n_jobs);
  std::atomic<bool> too_many{false};
  // Every fourth pixel along both image axes is enough for the levels of an image; the levels of
  // longitudinal sections are taken in bands this long along the axis.
  constexpr std::int64_t kSampleStep = 4;
  constexpr double kBandMm = 2.0;
  const std::int64_t band_rows = std::max<std::int64_t>(1, std::lround(kBandMm / cell.pixel));
  std::vector<double> thresholds(n_jobs, result.particle_threshold);
  // Share of the electrode stack in each cross-section as dark as gas.
  std::vector<double> gas_fraction(n_cross, kNaN);
  // Raises the threshold of `job` per band of `rows` rows from its samples and keeps the pixels
  // above it in `b`.
  const auto adapt = [&](std::size_t job, Found& b, std::vector<std::vector<double>>& samples,
                         std::vector<double>& steps, std::int64_t width, std::int64_t rows) {
    if (options.particle_threshold) {
      return;
    }
    const std::vector<double> t =
        imageParticleThresholds(std::move(samples), std::move(steps), result.particle_threshold);
    thresholds[job] = *std::max_element(t.begin(), t.end());
    std::erase_if(b, [&](const auto& pixel) {
      const auto band = static_cast<std::int64_t>(pixel.first) / width / rows;
      return pixel.second < static_cast<float>(t[at(band)]);
    });
  };
  // Next to the can the stack looks brighter, most at the ends where the can bends: bright
  // pixels this close to the housing outside the winding belong to its edge, not to a particle.
  constexpr double kHousingMarginMm = 0.5;
  const auto margin = std::max<std::int64_t>(1, std::lround(kHousingMarginMm / cell.pixel));
  const auto housing = static_cast<float>(
      0.5 * (result.levels.housing + std::max(result.levels.cathode, result.levels.anode)));
  std::atomic<std::size_t> at_housing{0};
  // Drops the pixels of `b` near a pixel of `image` as bright as the housing where `outside`.
  const auto drop_at_housing = [&](Found& b, const CellImage& image, const auto& outside) {
    if (!(result.levels.housing > result.particle_threshold)) {
      return;
    }
    at_housing += std::erase_if(b, [&](const auto& pixel) {
      const auto x = static_cast<std::int64_t>(pixel.first) % image.width;
      const auto y = static_cast<std::int64_t>(pixel.first) / image.width;
      for (std::int64_t ny = std::max<std::int64_t>(0, y - margin);
           ny <= std::min(image.height - 1, y + margin); ++ny) {
        for (std::int64_t nx = std::max<std::int64_t>(0, x - margin);
             nx <= std::min(image.width - 1, x + margin); ++nx) {
          if (static_cast<float>(image.values[at(nx + image.width * ny)]) >= housing &&
              outside(nx, ny)) {
            return true;
          }
        }
      }
      return false;
    });
  };
  const auto finish = [&](std::size_t job, Found& b, Found& d, std::int64_t width,
                          std::int64_t height, const auto& place) {
    if (b.size() + d.size() > kMaxFindingVoxels / 16) {
      too_many = true;
      return;
    }
    const Index3 dims{width, height, 1};
    for (int kind = 0; kind < 2; ++kind) {
      std::vector<CellFinding> found =
          components(std::move(kind == 0 ? b : d), dims, pitch,
                     kind == 0 ? options.min_particle_voxels : options.min_void_voxels, kind == 0);
      for (CellFinding& f : found) {
        const double area = static_cast<double>(f.voxels) * cell.pixel * cell.pixel;
        f.volume_mm3 = kNaN;
        f.equivalent_diameter_mm = 2.0 * std::sqrt(area / kPi);
        f.center_mm = place(f.center_mm[0] / cell.pixel, f.center_mm[1] / cell.pixel);
      }
      (kind == 0 ? bright : dark)[job] = std::move(found);
    }
  };
  tbb::parallel_for(std::size_t{0}, n_jobs, [&](std::size_t job) {
    Found b;
    Found d;
    if (job < n_cross) {
      const CellImage& image = *cell.cross[job];
      const std::int64_t k = cell.cross_index[job];
      const auto centre = data.centre(static_cast<double>(k) * cell.pixel);
      const std::vector<char> core = darkFrom(image, 0, image.width, gas,
                                              {std::lround(centre[0] / cell.pixel) +
                                               image.width * std::lround(centre[1] / cell.pixel)});
      std::vector<std::vector<double>> samples(1);
      std::vector<double> steps;
      std::size_t sampled = 0;
      std::size_t gas_like = 0;
      const double cx = centre[0] / cell.pixel;
      const double cy = centre[1] / cell.pixel;
      for (std::int64_t y = 0; y < image.height; ++y) {
        for (std::int64_t x = 0; x < image.width; ++x) {
          const auto value = static_cast<float>(image.values[at(x + image.width * y)]);
          if (x % kSampleStep == 0 && y % kSampleStep == 0 && in_region({x, y, k}, true)) {
            ++sampled;
            if (value <= gas || core[at(x + image.width * y)] != 0) {
              ++gas_like;
            } else {
              samples[0].push_back(value);
              // Pixels around the axis lie on the same turn.
              const double dx = static_cast<double>(x) - cx;
              const double dy = static_cast<double>(y) - cy;
              const double r = std::max(std::hypot(dx, dy), 1.0);
              const std::int64_t nx = x - std::lround(2.0 * dy / r);
              const std::int64_t ny = y + std::lround(2.0 * dx / r);
              if (nx >= 0 && nx < image.width && ny >= 0 && ny < image.height) {
                const auto next = static_cast<float>(image.values[at(nx + image.width * ny)]);
                if (next > gas) {
                  steps.push_back(static_cast<double>(value) - next);
                }
              }
            }
          }
          if ((value < particle && value > gas) || core[at(x + image.width * y)] != 0 ||
              !in_region({x, y, k}, value >= particle)) {
            continue;
          }
          (value >= particle ? b : d)
              .emplace_back(static_cast<std::uint64_t>(x + image.width * y), value);
        }
      }
      adapt(job, b, samples, steps, image.width, image.height);
      drop_at_housing(b, image, [&](std::int64_t nx, std::int64_t ny) {
        return !in_region({nx, ny, k}, true);
      });
      gas_fraction[job] =
          sampled > 0 ? static_cast<double>(gas_like) / static_cast<double>(sampled) : kNaN;
      finish(job, b, d, image.width, image.height, [&](double x, double y) {
        return Vec3{x * cell.pixel, y * cell.pixel, static_cast<double>(k) * cell.pixel};
      });
      return;
    }
    const std::size_t i = (job - n_cross) / 2;
    const double sign = (job - n_cross) % 2 == 0 ? 1.0 : -1.0;
    const CellImage& image = *cell.along[i];
    const double axis = cell.axis_column[i];
    const double angle = (image.position + (sign > 0.0 ? 0.0 : 180.0)) * kPi / 180.0;
    const double cos_a = std::cos(angle);
    const double sin_a = std::sin(angle);
    const auto to3d = [&](double col, double row) {
      const double r = sign * (col - axis) * cell.pixel;
      const double z = (static_cast<double>(image.height - 1) - row) * cell.pixel;
      const auto c = data.centre(z);
      return Vec3{c[0] + r * cos_a, c[1] + r * sin_a, z};
    };
    const auto from = static_cast<std::int64_t>(sign > 0.0 ? std::ceil(axis) : 0.0);
    const auto to =
        static_cast<std::int64_t>(sign > 0.0 ? static_cast<double>(image.width) : std::ceil(axis));
    std::vector<std::int64_t> seeds;
    const std::int64_t seed_col = sign > 0.0 ? from : to - 1;
    if (seed_col >= 0 && seed_col < image.width) {
      for (std::int64_t row = 0; row < image.height; ++row) {
        seeds.push_back(seed_col + image.width * row);
      }
    }
    const std::vector<char> core = darkFrom(image, from, to, gas, seeds);
    std::vector<std::vector<double>> samples(at((image.height + band_rows - 1) / band_rows));
    std::vector<double> steps;
    const auto voxel = [&](std::int64_t col, std::int64_t row) {
      const Vec3 p = to3d(static_cast<double>(col), static_cast<double>(row));
      return Index3{std::lround(p[0] / cell.pixel), std::lround(p[1] / cell.pixel),
                    std::lround(p[2] / cell.pixel)};
    };
    for (std::int64_t row = 0; row < image.height; ++row) {
      for (std::int64_t col = from; col < to; ++col) {
        const auto value = static_cast<float>(image.values[at(col + image.width * row)]);
        if (col % kSampleStep == 0 && row % kSampleStep == 0 && row + 2 < image.height &&
            value > gas && core[at(col + image.width * row)] == 0 &&
            in_region(voxel(col, row), true)) {
          samples[at(row / band_rows)].push_back(value);
          // The layers run along the axis, down the image.
          const auto next = static_cast<float>(image.values[at(col + image.width * (row + 2))]);
          if (next > gas) {
            steps.push_back(static_cast<double>(value) - next);
          }
        }
        if ((value < particle && value > gas) || core[at(col + image.width * row)] != 0 ||
            !in_region(voxel(col, row), value >= particle)) {
          continue;
        }
        (value >= particle ? b : d)
            .emplace_back(static_cast<std::uint64_t>(col + image.width * row), value);
      }
    }
    adapt(job, b, samples, steps, image.width, band_rows);
    drop_at_housing(b, image, [&](std::int64_t nx, std::int64_t ny) {
      return !in_region(voxel(nx, ny), true);
    });
    finish(job, b, d, image.width, image.height, to3d);
  });
  if (too_many) {
    throw std::runtime_error(
        "Too many pixels above the particle threshold or below the void threshold in the "
        "electrode stack; set the thresholds by hand");
  }
  leaveOutVoidsBeyondTheEnds(cell, gas_fraction, dark, result);
  if (at_housing > 0) {
    std::ostringstream text;
    text << at_housing.load() << " bright pixels within " << kHousingMarginMm
         << " mm of the housing are taken for its edge, not for particles";
    result.warnings.push_back(text.str());
  }
  const auto raised = std::count_if(thresholds.begin(), thresholds.end(),
                                    [&](double t) { return t > result.particle_threshold; });
  if (raised > 0) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << "In " << raised << " of " << n_jobs
         << " section images (halves of longitudinal sections counted apart) the brightest layers "
            "with their noise reach the particle threshold; there it is raised up to "
         << *std::max_element(thresholds.begin(), thresholds.end());
    result.warnings.push_back(text.str());
  }
  // Neighbouring cross-sections lie up to this far apart along the axis.
  double spacing = cell.pixel;
  for (std::size_t c = 1; c < n_cross; ++c) {
    spacing = std::max(
        spacing, static_cast<double>(cell.cross_index[c] - cell.cross_index[c - 1]) * cell.pixel);
  }
  // Sections of one finding in several images are one finding: those that overlap across the
  // axis and lie in neighbouring cross-sections along it. Tabs and welds run along the axis
  // through many cross-sections; together their sections show how long they are.
  const auto merge = [&](std::vector<std::vector<CellFinding>>& per_job, bool brightest) {
    std::vector<CellFinding> all;
    for (auto& found : per_job) {
      all.insert(all.end(), found.begin(), found.end());
    }
    std::sort(all.begin(), all.end(),
              [](const CellFinding& a, const CellFinding& b) { return a.voxels > b.voxels; });
    std::vector<std::size_t> parent(all.size());
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    const auto root = [&](std::size_t i) {
      while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
      }
      return i;
    };
    const double gap = 1.5 * spacing;
    const auto link = [&](std::size_t i, std::size_t j) {
      const CellFinding& a = all[i];
      const CellFinding& b = all[j];
      const double reach = 0.5 * (a.size_mm + b.size_mm);
      if (std::hypot(a.center_mm[0] - b.center_mm[0], a.center_mm[1] - b.center_mm[1]) < reach &&
          std::abs(a.center_mm[2] - b.center_mm[2]) <= std::max(reach, gap)) {
        const std::size_t ri = root(i);
        const std::size_t rj = root(j);
        parent[std::max(ri, rj)] = std::min(ri, rj);
      }
    };
    // Findings up to `small` across reach each other only from neighbouring grid cells; the few
    // larger ones are compared with all.
    constexpr double kSmallMm = 1.0;
    const double cell_z = std::max(kSmallMm, gap);
    using Key = std::array<std::int64_t, 3>;
    const auto key = [&](const CellFinding& f) {
      return Key{static_cast<std::int64_t>(std::floor(f.center_mm[0] / kSmallMm)),
                 static_cast<std::int64_t>(std::floor(f.center_mm[1] / kSmallMm)),
                 static_cast<std::int64_t>(std::floor(f.center_mm[2] / cell_z))};
    };
    std::map<Key, std::vector<std::size_t>> grid;
    std::vector<std::size_t> large;
    for (std::size_t i = 0; i < all.size(); ++i) {
      if (all[i].size_mm > kSmallMm) {
        large.push_back(i);
      } else {
        grid[key(all[i])].push_back(i);
      }
    }
    for (const auto& [k, members] : grid) {
      for (std::int64_t dx = -1; dx <= 1; ++dx) {
        for (std::int64_t dy = -1; dy <= 1; ++dy) {
          for (std::int64_t dz = -1; dz <= 1; ++dz) {
            const auto other = grid.find(Key{k[0] + dx, k[1] + dy, k[2] + dz});
            if (other == grid.end()) {
              continue;
            }
            for (const std::size_t i : members) {
              for (const std::size_t j : other->second) {
                if (i < j) {
                  link(i, j);
                }
              }
            }
          }
        }
      }
    }
    for (const std::size_t i : large) {
      for (std::size_t j = 0; j < all.size(); ++j) {
        if (j != i) {
          link(i, j);
        }
      }
    }
    // The largest section stands for the finding; the extent along the axis spans all.
    std::vector<CellFinding> kept;
    std::vector<std::size_t> kept_of(all.size());
    std::vector<std::array<double, 2>> extent;
    for (std::size_t i = 0; i < all.size(); ++i) {
      const CellFinding& f = all[i];
      const double lo = f.center_mm[2] - 0.5 * f.size_mm;
      const double hi = f.center_mm[2] + 0.5 * f.size_mm;
      if (root(i) == i) {
        kept_of[i] = kept.size();
        kept.push_back(f);
        extent.push_back({lo, hi});
        continue;
      }
      const std::size_t k = kept_of[root(i)];
      extent[k] = {std::min(extent[k][0], lo), std::max(extent[k][1], hi)};
      kept[k].extreme_value = brightest ? std::max(kept[k].extreme_value, f.extreme_value)
                                        : std::min(kept[k].extreme_value, f.extreme_value);
    }
    for (std::size_t i = 0; i < kept.size(); ++i) {
      kept[i].id = static_cast<int>(i + 1);
      kept[i].size_mm = std::max(kept[i].size_mm, extent[i][1] - extent[i][0]);
      kept[i].center_mm[2] = 0.5 * (extent[i][0] + extent[i][1]);
    }
    return kept;
  };
  result.particles = merge(bright, true);
  result.voids = merge(dark, false);
  separateStructures(result, options);
}

/// Fewest pixels between neighbouring layers for an overhang measured in section images.
constexpr double kMinPixelsPerLayer = 6.0;

}  // namespace

BatteryResult analyzeImages(const CellImages& images, const BatteryOptions& options) {
  if (options.cell_type && *options.cell_type != CellType::kCylindrical) {
    throw std::invalid_argument("Cells given as section images must be cylindrical");
  }
  BatteryResult result;
  result.from_images = true;
  const ImageCell cell = placeImages(images, options, result.warnings);
  const VolumeView cross = crossView(cell);
  result.dims = cell.dims;
  result.voxel_size = VoxelSize(cell.pixel);
  result.levels.air = cell.air;
  result.min_overhang_mm = kNaN;
  result.type = CellType::kCylindrical;
  result.axis = 2;
  Extras extras;
  CylinderData data;
  data.axis = 2;
  data.u = 0;
  data.v = 1;
  data.box = imageOverview(cell);
  extras.box = data.box;
  for (std::size_t a = 0; a < 3; ++a) {
    result.bounds_min_mm[a] = static_cast<double>(data.box.lo[a]) * cell.pixel;
    result.bounds_max_mm[a] = static_cast<double>(data.box.hi[a] - 1) * cell.pixel;
  }
  result.length_mm = static_cast<double>(data.box.hi[2] - data.box.lo[2]) * cell.pixel;
  cylinderSections(cross, options, data, result);
  const double centre_z = 0.5 * (result.bounds_min_mm[2] + result.bounds_max_mm[2]);
  const VolumeView along = alongView(cell, data.centre(centre_z)[0]);
  cylinderOuterSize(cross, along, data, result);
  imageOverhang(cell, options, data, result, extras.worst);
  summarizeEdges(result);
  cylinderEnds(data, result);
  extras.points = data.points;
  extras.cross_axis = 2;
  extras.cross_index = static_cast<std::int64_t>(
      std::lround(result.sections[result.sections.size() / 2].position_mm[0] / cell.pixel));
  extras.long_axis = 1;
  extras.long_index = static_cast<std::int64_t>(std::lround(data.centre(centre_z)[1] / cell.pixel));
  summarizeSections(result);
  // Where the layers lie only a few pixels apart, the ends of cathode and anode blur into each
  // other: an overhang of 0.1 mm is then two or three pixels, as large as that blur.
  if (!result.overhang.empty() && result.pitch_mm < kMinPixelsPerLayer * cell.pixel) {
    result.overhang.clear();
    result.min_overhang_mm = kNaN;
    for (BatteryEdge& edge : result.edges) {
      edge.samples = 0;
      edge.worst = -1;
    }
    summarizeEdges(result);
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << "The layers lie " << result.pitch_mm / cell.pixel
         << " pixels apart, fewer than " << kMinPixelsPerLayer << "; the overhang is not measured";
    result.warnings.push_back(text.str());
  }
  setThresholds(result, options);
  imageFindings(cell, cylinderRegion(data, result.voxel_size), data, result, options);
  placeFindings(result);
  renderImages(cross, along, extras, result);
  return result;
}

}  // namespace voxelsieve::detail::battery
