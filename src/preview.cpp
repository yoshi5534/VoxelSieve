#include "voxelsieve/preview.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

#include "detail/blocks.hpp"
#include "detail/png.hpp"
#include "detail/preview.hpp"

namespace voxelsieve {
namespace {

using detail::Histogram;
using detail::kHistogramBins;

/// Grey value below which `fraction` of the histogram's entries lie.
float percentile(const Histogram& histogram, std::uint64_t total, double fraction) {
  const auto target = static_cast<std::uint64_t>(fraction * static_cast<double>(total));
  std::uint64_t sum = 0;
  for (std::size_t v = 0; v < histogram.size(); ++v) {
    sum += histogram[v];
    if (sum > target) {
      return static_cast<float>(v);
    }
  }
  return static_cast<float>(histogram.size() - 1);
}

/// Lowest and highest grey value that occurs.
std::array<std::size_t, 2> occupiedRange(const std::vector<std::uint64_t>& histogram) {
  std::size_t low = 0;
  while (low + 1 < histogram.size() && histogram[low] == 0) {
    ++low;
  }
  std::size_t high = histogram.size() - 1;
  while (high > low && histogram[high] == 0) {
    --high;
  }
  return {low, high};
}

}  // namespace

double ImportPreview::fractionRead() const {
  const auto& d = source_dims;
  const double total =
      static_cast<double>(d[0]) * static_cast<double>(d[1]) * static_cast<double>(d[2]);
  return total > 0.0 ? static_cast<double>(slices.size()) * static_cast<double>(d[0]) *
                           static_cast<double>(d[1]) / total
                     : 0.0;
}

std::vector<std::int64_t> previewSlices(std::int64_t depth, std::int64_t count) {
  if (depth <= 0 || count <= 0) {
    return {};
  }
  count = std::min(count, depth);
  std::vector<std::int64_t> slices;
  slices.reserve(static_cast<std::size_t>(count));
  for (std::int64_t k = 0; k < count; ++k) {
    // Middle of part k of `count` equal parts: (k + 0.5) * depth / count, in integers.
    slices.push_back(((2 * k + 1) * depth) / (2 * count));
  }
  return slices;
}

namespace detail {

PreviewBuilder::PreviewBuilder(const std::array<std::int64_t, 3>& dims, const VoxelSize& voxel_size,
                               const ValueMapping& mapping, const PreviewOptions& options)
    : histograms_([] { return Histogram(kHistogramBins, 0); }) {
  if (options.slices < 1 || options.size < 1) {
    throw std::invalid_argument("A preview needs at least one slice and one voxel per axis");
  }
  preview_.source_dims = dims;
  preview_.value_mapping = mapping;
  preview_.slices = previewSlices(dims[2], options.slices);
  const std::int64_t stride = ceilDiv(std::max(dims[0], dims[1]), options.size);
  preview_.pixel_stride = std::max<std::int64_t>(stride, 1);
  const std::array<std::int64_t, 3> preview_dims{ceilDiv(dims[0], preview_.pixel_stride),
                                                 ceilDiv(dims[1], preview_.pixel_stride),
                                                 static_cast<std::int64_t>(preview_.slices.size())};
  const double spacing = preview_dims[2] > 0
                             ? static_cast<double>(dims[2]) / static_cast<double>(preview_dims[2])
                             : 1.0;
  const auto s = static_cast<double>(preview_.pixel_stride);
  preview_.volume = Volume16(
      preview_dims, VoxelSize(voxel_size[0] * s, voxel_size[1] * s, voxel_size[2] * spacing));
}

void PreviewBuilder::add(std::size_t k, const std::uint16_t* slice) {
  const auto& d = preview_.source_dims;
  const auto& p = preview_.volume.dims;
  const std::int64_t s = preview_.pixel_stride;
  Histogram& histogram = histograms_.local();
  std::uint16_t* plane = &preview_.volume.data[k * static_cast<std::size_t>(p[0] * p[1])];
  std::vector<std::uint64_t> sums(static_cast<std::size_t>(p[0]));
  for (std::int64_t py = 0; py < p[1]; ++py) {
    std::fill(sums.begin(), sums.end(), 0);
    const std::int64_t y0 = py * s;
    const std::int64_t y1 = std::min(y0 + s, d[1]);
    for (std::int64_t y = y0; y < y1; ++y) {
      const std::uint16_t* row = slice + static_cast<std::size_t>(y * d[0]);
      for (std::int64_t px = 0; px < p[0]; ++px) {
        std::uint64_t sum = 0;
        for (std::int64_t x = px * s; x < std::min((px + 1) * s, d[0]); ++x) {
          const std::uint16_t value = row[x];
          ++histogram[value];
          sum += value;
        }
        sums[static_cast<std::size_t>(px)] += sum;
      }
    }
    for (std::int64_t px = 0; px < p[0]; ++px) {
      const auto count =
          static_cast<std::uint64_t>((std::min((px + 1) * s, d[0]) - px * s) * (y1 - y0));
      plane[static_cast<std::size_t>(px + p[0] * py)] =
          static_cast<std::uint16_t>((sums[static_cast<std::size_t>(px)] + count / 2) / count);
    }
  }
}

ImportPreview PreviewBuilder::finish() {
  preview_.histogram.assign(kHistogramBins, 0);
  histograms_.combine_each([this](const Histogram& local) {
    for (std::size_t v = 0; v < kHistogramBins; ++v) {
      preview_.histogram[v] += local[v];
    }
  });
  std::uint64_t total = 0;
  for (const std::uint64_t count : preview_.histogram) {
    total += count;
  }
  if (total > 0) {
    const ThresholdResult estimate = airThreshold(preview_.histogram);
    preview_.threshold = estimate.threshold;
    preview_.air_level = estimate.air_level;
    preview_.window = {percentile(preview_.histogram, total, 0.005),
                       percentile(preview_.histogram, total, 0.995)};
  }
  return std::move(preview_);
}

}  // namespace detail

ImportPreview readImportPreview(const VolumeSource& source, const PreviewOptions& options) {
  const auto dims = source.dims();
  detail::PreviewBuilder builder(dims, source.voxelSize(), source.valueMapping(), options);
  const auto& slices = builder.slices();
  const auto slice_voxels = static_cast<std::size_t>(dims[0] * dims[1]);
  // One slice per task: on a network share several requests are in flight at once.
  tbb::parallel_for(tbb::blocked_range<std::size_t>(0, slices.size(), 1), [&](const auto& range) {
    std::vector<std::uint16_t> buffer(slice_voxels);
    for (std::size_t k = range.begin(); k != range.end(); ++k) {
      const std::int64_t z = slices[k];
      source.readRegion(Box{{0, 0, z}, {dims[0], dims[1], z + 1}}, buffer);
      builder.add(k, buffer.data());
    }
  });
  return builder.finish();
}

PreviewImage renderPreview(const ImportPreview& preview) {
  constexpr int kLargest = 256;  // pixels of the longest section edge
  constexpr int kGap = 4;
  constexpr int kHistogramHeight = 96;
  constexpr std::uint8_t kBackground = 24;
  const Volume16& v = preview.volume;
  const auto& d = v.dims;
  const VoxelSize& pitch = v.voxel_size;

  // Sections normal to z, y and x: their axes (u right, w down) and the fixed voxel.
  struct Section {
    std::size_t u_axis;
    std::size_t w_axis;
    std::array<std::int64_t, 3> fixed;
    int width = 0;
    int height = 0;
  };
  const std::array<std::int64_t, 3> centre{d[0] / 2, d[1] / 2, d[2] / 2};
  std::array<Section, 3> sections{Section{0, 1, centre}, Section{0, 2, centre},
                                  Section{1, 2, centre}};
  double largest_mm = 0.0;
  for (std::size_t a = 0; a < 3; ++a) {
    largest_mm = std::max(largest_mm, static_cast<double>(d[a]) * pitch[a]);
  }
  const double mm_per_pixel = largest_mm > 0.0 ? largest_mm / kLargest : 1.0;
  int width = 0;
  int height = 0;
  for (Section& section : sections) {
    const auto pixels = [&](std::size_t axis) {
      return std::max(1, static_cast<int>(std::lround(static_cast<double>(d[axis]) * pitch[axis] /
                                                      mm_per_pixel)));
    };
    section.width = pixels(section.u_axis);
    section.height = pixels(section.w_axis);
    width += section.width + kGap;
    height = std::max(height, section.height);
  }
  width = std::max(width - kGap, kLargest);
  PreviewImage image;
  image.width = width;
  image.height = height + kGap + kHistogramHeight;
  image.rgb.assign(static_cast<std::size_t>(image.width * image.height) * 3, kBackground);
  const auto set = [&image](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const auto i = static_cast<std::size_t>(x + image.width * y) * 3;
    image.rgb[i] = r;
    image.rgb[i + 1] = g;
    image.rgb[i + 2] = b;
  };

  const float low = preview.window[0];
  const float span = std::max(preview.window[1] - low, 1.0F);
  if (v.voxelCount() > 0) {
    int left = 0;
    for (const Section& section : sections) {
      for (int py = 0; py < section.height; ++py) {
        for (int px = 0; px < section.width; ++px) {
          std::array<std::int64_t, 3> voxel = section.fixed;
          voxel[section.u_axis] =
              std::min<std::int64_t>(d[section.u_axis] - 1, px * d[section.u_axis] / section.width);
          voxel[section.w_axis] = std::min<std::int64_t>(d[section.w_axis] - 1,
                                                         py * d[section.w_axis] / section.height);
          const float t = (static_cast<float>(v.at(voxel[0], voxel[1], voxel[2])) - low) / span;
          const auto grey =
              static_cast<std::uint8_t>(std::lround(std::clamp(t, 0.0F, 1.0F) * 255.0F));
          set(left + px, py, grey, grey, grey);
        }
      }
      left += section.width + kGap;
    }
  }

  // Histogram on a logarithmic scale over the grey values that occur.
  const auto [first, last] = occupiedRange(preview.histogram);
  std::vector<std::uint64_t> bins(static_cast<std::size_t>(image.width), 0);
  const double bin_width = static_cast<double>(last - first + 1) / image.width;
  for (std::size_t value = first; value <= last && !preview.histogram.empty(); ++value) {
    const auto bin = std::min<std::size_t>(
        bins.size() - 1, static_cast<std::size_t>(static_cast<double>(value - first) / bin_width));
    bins[bin] += preview.histogram[value];
  }
  const std::uint64_t highest = bins.empty() ? 0 : *std::max_element(bins.begin(), bins.end());
  const int top = height + kGap;
  for (int x = 0; x < image.width && highest > 0; ++x) {
    const double fraction = std::log1p(static_cast<double>(bins[static_cast<std::size_t>(x)])) /
                            std::log1p(static_cast<double>(highest));
    const int bar = static_cast<int>(std::lround(fraction * kHistogramHeight));
    for (int y = kHistogramHeight - bar; y < kHistogramHeight; ++y) {
      set(x, top + y, 170, 170, 170);
    }
  }
  if (!preview.histogram.empty() && preview.threshold >= static_cast<float>(first) &&
      preview.threshold <= static_cast<float>(last)) {
    const int x =
        std::min(image.width - 1, static_cast<int>((preview.threshold - static_cast<float>(first)) /
                                                   static_cast<float>(bin_width)));
    for (int y = 0; y < kHistogramHeight; ++y) {
      set(x, top + y, 230, 60, 50);
    }
  }
  return image;
}

std::vector<std::uint8_t> previewPng(const ImportPreview& preview) {
  const PreviewImage image = renderPreview(preview);
  return detail::encodePng(static_cast<std::uint32_t>(image.width),
                           static_cast<std::uint32_t>(image.height), 3, image.rgb);
}

nlohmann::json previewSummary(const ImportPreview& preview, int bins) {
  nlohmann::json summary = {{"slices_read", preview.slices.size()},
                            {"slices", preview.source_dims[2]},
                            {"source_dims", preview.source_dims},
                            {"dims", preview.volume.dims},
                            {"pixel_stride", preview.pixel_stride},
                            {"fraction_read", preview.fractionRead()},
                            {"threshold", preview.threshold},
                            {"air_level", preview.air_level},
                            {"window", preview.window}};
  writeVoxelSize(summary, preview.volume.voxel_size);
  if (!preview.value_mapping.isIdentity()) {
    summary["value_mapping"] = {{"offset", preview.value_mapping.offset},
                                {"scale", preview.value_mapping.scale}};
  }
  if (preview.histogram.empty() || bins < 1) {
    return summary;
  }
  const auto [first, last] = occupiedRange(preview.histogram);
  std::vector<std::uint64_t> counts(static_cast<std::size_t>(bins), 0);
  const double bin_width = static_cast<double>(last - first + 1) / bins;
  for (std::size_t value = first; value <= last; ++value) {
    const auto bin = std::min<std::size_t>(
        counts.size() - 1,
        static_cast<std::size_t>(static_cast<double>(value - first) / bin_width));
    counts[bin] += preview.histogram[value];
  }
  summary["histogram"] = {{"min", first}, {"max", last}, {"counts", counts}};
  return summary;
}

}  // namespace voxelsieve
