#include "voxelsieve/materials.hpp"

#include <openvdb/io/File.h>
#include <tbb/combinable.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "detail/material_volume.hpp"
#include "detail/transform.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {
namespace {

using Index3 = std::array<std::int64_t, 3>;
using Json = nlohmann::json;

constexpr int kFormatVersion = 1;
constexpr std::size_t kCachedBricks = 64;
constexpr int kMaxMaterials = 8;

// Colours of the material classes, from light to dense.
constexpr std::array<std::array<std::uint8_t, 3>, kMaxMaterials> kColors = {{{66, 146, 198},
                                                                             {230, 126, 34},
                                                                             {46, 160, 67},
                                                                             {196, 60, 80},
                                                                             {142, 99, 190},
                                                                             {214, 190, 40},
                                                                             {23, 170, 170},
                                                                             {140, 110, 80}}};

std::int64_t ceilDiv(std::int64_t a, std::int64_t b) { return (a + b - 1) / b; }

std::size_t product(const Index3& dims) {
  return static_cast<std::size_t>(dims[0] * dims[1] * dims[2]);
}

Index3 unravel(std::size_t index, const Index3& dims) {
  const auto i = static_cast<std::int64_t>(index);
  return {i % dims[0], (i / dims[0]) % dims[1], i / (dims[0] * dims[1])};
}

std::filesystem::path brickFile(const std::filesystem::path& dir, const Index3& brick) {
  return detail::materialBrickFile(dir, brick);
}

/// Box filter of radius `r` along one axis of a dense x-fastest array, in place.
void boxFilterAxis(std::vector<float>& data, const Index3& dims, std::size_t axis, int r) {
  const std::int64_t n = dims[axis];
  const std::int64_t stride = axis == 0 ? 1 : (axis == 1 ? dims[0] : dims[0] * dims[1]);
  const std::int64_t lines = static_cast<std::int64_t>(data.size()) / n;
  tbb::parallel_for(std::int64_t{0}, lines, [&](std::int64_t line) {
    // Start of the line: the index with the axis coordinate 0.
    std::int64_t start = 0;
    if (axis == 0) {
      start = line * n;
    } else if (axis == 1) {
      start = (line % dims[0]) + (line / dims[0]) * dims[0] * dims[1];
    } else {
      start = line;
    }
    std::vector<double> prefix(static_cast<std::size_t>(n + 1), 0.0);
    for (std::int64_t i = 0; i < n; ++i) {
      prefix[static_cast<std::size_t>(i + 1)] =
          prefix[static_cast<std::size_t>(i)] + data[static_cast<std::size_t>(start + i * stride)];
    }
    for (std::int64_t i = 0; i < n; ++i) {
      const std::int64_t lo = std::max<std::int64_t>(i - r, 0);
      const std::int64_t hi = std::min<std::int64_t>(i + r + 1, n);
      data[static_cast<std::size_t>(start + i * stride)] = static_cast<float>(
          (prefix[static_cast<std::size_t>(hi)] - prefix[static_cast<std::size_t>(lo)]) /
          static_cast<double>(hi - lo));
    }
  });
}

int classOf(float value, const std::vector<Material>& materials) {
  int id = materials.front().id;
  for (const Material& material : materials) {
    if (value >= material.lower) {
      id = material.id;
    }
  }
  return id;
}

Json toJson(const MaterialVolumeInfo& info) {
  Json materials = Json::array();
  for (const Material& m : info.materials) {
    materials.push_back({{"id", m.id},
                         {"name", m.name},
                         {"color", m.color},
                         {"lower", m.lower},
                         {"voxel_count", m.voxel_count},
                         {"volume_mm3", m.volume_mm3}});
  }
  Json json = {
      {"format", "voxelsieve-materials"}, {"version", kFormatVersion}, {"dims", info.dims}};
  writeVoxelSize(json, info.voxel_size);
  json.update({{"brick_size", info.brick_size},
               {"air_level", info.air_level},
               {"air_threshold", info.air_threshold},
               {"grow_threshold", info.grow_threshold},
               {"grow_steps", info.grow_steps},
               {"min_neighbours", info.min_neighbours},
               {"materials", materials},
               {"bricks", info.bricks}});
  if (!info.model.empty()) {
    json["model"] = info.model;
  }
  return json;
}

/// Material thresholds from the grey values above the air threshold.
std::vector<float> estimateMaterialThresholds(const Dataset& dataset, float air_threshold,
                                              int classes) {
  const int level = 0;
  tbb::combinable<std::vector<std::uint64_t>> histograms(
      [] { return std::vector<std::uint64_t>(65536, 0); });
  dataset.forEachBrick(level, [&](const Index3& /*brick*/, const openvdb::FloatGrid& grid) {
    auto& histogram = histograms.local();
    for (auto it = grid.cbeginValueOn(); it; ++it) {
      if (*it > air_threshold) {
        ++histogram[static_cast<std::size_t>(std::clamp(*it, 0.0F, 65535.0F))];
      }
    }
  });
  std::vector<std::uint64_t> histogram(65536, 0);
  histograms.combine_each([&](const std::vector<std::uint64_t>& local) {
    for (std::size_t i = 0; i < histogram.size(); ++i) {
      histogram[i] += local[i];
    }
  });
  return multiOtsu(histogram, classes);
}

}  // namespace

std::vector<float> multiOtsu(const std::vector<std::uint64_t>& histogram, int classes) {
  if (classes < 1) {
    throw std::invalid_argument("multiOtsu needs at least one class");
  }
  if (classes == 1) {
    return {};
  }
  std::size_t first = histogram.size();
  std::size_t last = 0;
  for (std::size_t i = 0; i < histogram.size(); ++i) {
    if (histogram[i] > 0) {
      first = std::min(first, i);
      last = i;
    }
  }
  if (first > last) {
    throw std::invalid_argument("Cannot split an empty histogram into materials");
  }
  // Coarse bins keep the search at O(classes * bins^2).
  constexpr std::size_t kBins = 256;
  const double width = std::max(1.0, static_cast<double>(last + 1 - first) / kBins);
  std::vector<double> weight(kBins + 1, 0.0);  // prefix sums
  std::vector<double> sum(kBins + 1, 0.0);
  {
    std::vector<double> w(kBins, 0.0);
    std::vector<double> s(kBins, 0.0);
    for (std::size_t i = first; i <= last; ++i) {
      const auto bin =
          std::min(static_cast<std::size_t>(static_cast<double>(i - first) / width), kBins - 1);
      w[bin] += static_cast<double>(histogram[i]);
      s[bin] += static_cast<double>(histogram[i]) * static_cast<double>(i);
    }
    for (std::size_t b = 0; b < kBins; ++b) {
      weight[b + 1] = weight[b] + w[b];
      sum[b + 1] = sum[b] + s[b];
    }
  }
  // Between-class variance up to a constant: sum over classes of S^2 / W.
  const auto score = [&](std::size_t a, std::size_t b) {  // bins [a, b)
    const double w = weight[b] - weight[a];
    const double s = sum[b] - sum[a];
    return w > 0.0 ? s * s / w : 0.0;
  };
  const auto c = static_cast<std::size_t>(classes);
  constexpr double kNone = -1.0;
  std::vector<std::vector<double>> best(c + 1, std::vector<double>(kBins + 1, kNone));
  std::vector<std::vector<std::size_t>> cut(c + 1, std::vector<std::size_t>(kBins + 1, 0));
  best[0][0] = 0.0;
  for (std::size_t k = 1; k <= c; ++k) {
    for (std::size_t j = k; j <= kBins; ++j) {
      for (std::size_t i = k - 1; i < j; ++i) {
        if (best[k - 1][i] == kNone) {
          continue;
        }
        const double value = best[k - 1][i] + score(i, j);
        if (value > best[k][j]) {
          best[k][j] = value;
          cut[k][j] = i;
        }
      }
    }
  }
  std::vector<float> thresholds(c - 1);
  std::size_t j = kBins;
  for (std::size_t k = c; k > 1; --k) {
    j = cut[k][j];
    thresholds[k - 2] =
        static_cast<float>(static_cast<double>(first) + static_cast<double>(j) * width);
  }
  return thresholds;
}

MaterialVolumeInfo segmentMaterials(const Dataset& dataset, const std::filesystem::path& dir,
                                    const SegmentationOptions& options) {
  if (options.materials < 1 || options.materials > kMaxMaterials) {
    throw std::invalid_argument("materials must be between 1 and 8");
  }
  if (options.min_neighbours < 1 || options.min_neighbours > 27 || options.grow_steps < 0 ||
      options.grow_fraction < 0.0F || options.grow_fraction > 1.0F) {
    throw std::invalid_argument("Invalid neighbour or growing options");
  }
  if (!options.material_thresholds.empty() &&
      (options.material_thresholds.size() != static_cast<std::size_t>(options.materials - 1) ||
       !std::is_sorted(options.material_thresholds.begin(), options.material_thresholds.end()))) {
    throw std::invalid_argument("material_thresholds needs materials - 1 ascending values");
  }
  if (std::filesystem::exists(dir) && !std::filesystem::is_empty(dir)) {
    throw std::invalid_argument("Output directory is not empty: " + dir.string());
  }
  openvdb::initialize();
  const DatasetInfo& data = dataset.info();

  MaterialVolumeInfo info;
  info.dims = data.dims;
  info.voxel_size = data.voxel_size;
  info.brick_size = data.brick_size;
  info.air_level = data.air_level;
  info.air_threshold = options.air_threshold.value_or(data.threshold);
  info.grow_threshold =
      info.air_level + options.grow_fraction * (info.air_threshold - info.air_level);
  info.grow_steps = options.grow_steps;
  info.min_neighbours = options.min_neighbours;
  std::optional<TelemetryPhase> phase(std::in_place, "thresholds");
  const std::vector<float> thresholds =
      options.material_thresholds.empty()
          ? estimateMaterialThresholds(dataset, info.air_threshold, options.materials)
          : options.material_thresholds;
  phase.reset();
  phase.emplace("bricks");
  for (int m = 0; m < options.materials; ++m) {
    Material material;
    material.id = m + 1;
    material.name = "Material " + std::to_string(m + 1);
    material.color = detail::materialColor(m + 1);
    material.lower = m == 0 ? info.air_threshold : thresholds[static_cast<std::size_t>(m - 1)];
    info.materials.push_back(material);
  }

  std::filesystem::create_directories(dir / "level0");
  const Index3 brick_dims{ceilDiv(info.dims[0], info.brick_size),
                          ceilDiv(info.dims[1], info.brick_size),
                          ceilDiv(info.dims[2], info.brick_size)};
  const std::int64_t halo = options.grow_steps + 1;
  tbb::combinable<std::vector<std::int64_t>> counts(
      [&] { return std::vector<std::int64_t>(info.materials.size() + 1, 0); });
  std::mutex mutex;
  // Bricks one after another; the filters inside a brick run in parallel, so memory stays at
  // one brick with its halo.
  for (std::size_t i = 0; i < product(brick_dims); ++i) {
    const Index3 brick = unravel(i, brick_dims);
    if (!dataset.hasBrick(0, brick)) {
      continue;
    }
    const Box inner = dataset.brickBox(0, brick);
    Box outer;
    for (std::size_t a = 0; a < 3; ++a) {
      outer.min[a] = std::max<std::int64_t>(inner.min[a] - halo, 0);
      outer.max[a] = std::min<std::int64_t>(inner.max[a] + halo, info.dims[a]);
    }
    const Index3 size{outer.size(0), outer.size(1), outer.size(2)};
    std::vector<float> grey(static_cast<std::size_t>(outer.voxelCount()));
    dataset.readRegion(0, outer, grey, info.air_level);
    // Above the threshold, and how many of the 3^3 neighbourhood are.
    std::vector<float> above(grey.size());
    std::transform(grey.begin(), grey.end(), above.begin(),
                   [&](float value) { return value > info.air_threshold ? 1.0F : 0.0F; });
    std::vector<float> neighbours = above;
    std::vector<float> material_sum(grey.size());
    std::transform(grey.begin(), grey.end(), above.begin(), material_sum.begin(),
                   std::multiplies<>());
    for (std::size_t a = 0; a < 3; ++a) {
      boxFilterAxis(neighbours, size, a, 1);
      boxFilterAxis(material_sum, size, a, 1);
    }
    // Box filters give means; the counts are these times the neighbourhood size, which is 27
    // except at the region border. Using means keeps the criterion the same there.
    const float min_fraction = static_cast<float>(options.min_neighbours) / 27.0F;
    // 2: material, 1: may grow into material, 0: air.
    std::vector<std::uint8_t> state(grey.size());
    for (std::size_t v = 0; v < grey.size(); ++v) {
      state[v] = above[v] > 0.0F && neighbours[v] >= min_fraction - 1e-4F ? 2
                 : grey[v] > info.grow_threshold                          ? 1
                                                                          : 0;
    }
    const std::array<std::int64_t, 3> step{1, size[0], size[0] * size[1]};
    for (int g = 0; g < options.grow_steps; ++g) {
      std::vector<std::uint8_t> next = state;
      tbb::parallel_for(std::int64_t{0}, size[2], [&](std::int64_t z) {
        for (std::int64_t y = 0; y < size[1]; ++y) {
          for (std::int64_t x = 0; x < size[0]; ++x) {
            const std::int64_t index = x + step[1] * y + step[2] * z;
            if (state[static_cast<std::size_t>(index)] != 1) {
              continue;
            }
            const Index3 p{x, y, z};
            for (std::size_t a = 0; a < 3; ++a) {
              if ((p[a] > 0 && state[static_cast<std::size_t>(index - step[a])] == 2) ||
                  (p[a] + 1 < size[a] && state[static_cast<std::size_t>(index + step[a])] == 2)) {
                next[static_cast<std::size_t>(index)] = 2;
                break;
              }
            }
          }
        }
      });
      state.swap(next);
    }

    auto grid = openvdb::Int32Grid::create(0);
    auto accessor = grid->getAccessor();
    auto& local = counts.local();
    for (std::int64_t z = inner.min[2]; z < inner.max[2]; ++z) {
      for (std::int64_t y = inner.min[1]; y < inner.max[1]; ++y) {
        for (std::int64_t x = inner.min[0]; x < inner.max[0]; ++x) {
          const auto index = static_cast<std::size_t>(
              (x - outer.min[0]) + step[1] * (y - outer.min[1]) + step[2] * (z - outer.min[2]));
          if (state[index] != 2) {
            continue;
          }
          // Mean grey value of the material voxels around it; a grown voxel has none above the
          // threshold nearby only if the growth passed through several, so fall back to itself.
          const float value =
              neighbours[index] > 0.0F ? material_sum[index] / neighbours[index] : grey[index];
          const int id = classOf(value, info.materials);
          ++local[static_cast<std::size_t>(id)];
          accessor.setValue(
              openvdb::Coord(static_cast<int>(x), static_cast<int>(y), static_cast<int>(z)), id);
        }
      }
    }
    if (grid->activeVoxelCount() == 0) {
      continue;
    }
    grid->setName("material");
    grid->setTransform(detail::voxelTransform(info.voxel_size, 0));
    writeVdb(brickFile(dir, brick), {grid});
    const std::scoped_lock lock(mutex);
    info.bricks.push_back(brick);
  }
  std::sort(info.bricks.begin(), info.bricks.end());
  counts.combine_each([&](const std::vector<std::int64_t>& local) {
    for (Material& material : info.materials) {
      material.voxel_count += local[static_cast<std::size_t>(material.id)];
    }
  });
  const double voxel_mm3 = info.voxel_size.volumeMm3();
  for (Material& material : info.materials) {
    material.volume_mm3 = static_cast<double>(material.voxel_count) * voxel_mm3;
  }
  detail::writeMaterialVolumeInfo(dir, info);
  return info;
}

MaterialVolumeInfo readMaterialVolumeInfo(const std::filesystem::path& dir) {
  std::ifstream in(dir / "materials.json");
  if (!in) {
    throw std::runtime_error("No materials.json in " + dir.string());
  }
  const Json json = Json::parse(in);
  if (json.at("format") != "voxelsieve-materials" || json.at("version") != kFormatVersion) {
    throw std::runtime_error("Unsupported material volume in " + dir.string());
  }
  MaterialVolumeInfo info;
  info.dims = json.at("dims").get<Index3>();
  info.voxel_size = readVoxelSize(json);
  info.brick_size = json.at("brick_size").get<std::int64_t>();
  info.air_level = json.at("air_level").get<float>();
  info.air_threshold = json.at("air_threshold").get<float>();
  info.grow_threshold = json.at("grow_threshold").get<float>();
  info.grow_steps = json.at("grow_steps").get<int>();
  info.min_neighbours = json.at("min_neighbours").get<int>();
  for (const Json& m : json.at("materials")) {
    Material material;
    material.id = m.at("id").get<int>();
    material.name = m.at("name").get<std::string>();
    material.color = m.at("color").get<std::array<std::uint8_t, 3>>();
    material.lower = m.at("lower").get<float>();
    material.voxel_count = m.at("voxel_count").get<std::int64_t>();
    material.volume_mm3 = m.at("volume_mm3").get<double>();
    info.materials.push_back(material);
  }
  info.bricks = json.at("bricks").get<std::vector<Index3>>();
  info.model = json.value("model", std::string());
  return info;
}

struct MaterialVolume::Impl {
  std::filesystem::path dir;
  MaterialVolumeInfo info;
  std::vector<std::array<std::int64_t, 3>> sorted_bricks;
  mutable std::mutex mutex;
  mutable std::map<Index3, openvdb::Int32Grid::ConstPtr> cache;

  [[nodiscard]] openvdb::Int32Grid::ConstPtr brick(const Index3& index) const {
    {
      const std::scoped_lock lock(mutex);
      if (const auto it = cache.find(index); it != cache.end()) {
        return it->second;
      }
    }
    openvdb::Int32Grid::ConstPtr grid;
    if (std::binary_search(sorted_bricks.begin(), sorted_bricks.end(), index)) {
      openvdb::io::File file(brickFile(dir, index).string());
      file.open(false);
      grid = openvdb::gridConstPtrCast<openvdb::Int32Grid>(file.readGrid("material"));
      file.close();
    }
    const std::scoped_lock lock(mutex);
    if (cache.size() >= kCachedBricks) {
      cache.clear();
    }
    cache[index] = grid;
    return grid;
  }
};

MaterialVolume::MaterialVolume(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MaterialVolume::MaterialVolume(MaterialVolume&&) noexcept = default;
MaterialVolume& MaterialVolume::operator=(MaterialVolume&&) noexcept = default;
MaterialVolume::~MaterialVolume() = default;

MaterialVolume MaterialVolume::open(const std::filesystem::path& dir) {
  openvdb::initialize();
  auto impl = std::make_unique<Impl>();
  impl->dir = dir;
  impl->info = readMaterialVolumeInfo(dir);
  impl->sorted_bricks = impl->info.bricks;
  std::sort(impl->sorted_bricks.begin(), impl->sorted_bricks.end());
  return MaterialVolume(std::move(impl));
}

const MaterialVolumeInfo& MaterialVolume::info() const { return impl_->info; }

void MaterialVolume::readRegion(const Box& box, std::span<std::uint8_t> out,
                                std::int64_t stride) const {
  if (stride < 1) {
    throw std::invalid_argument("stride must be positive");
  }
  const Index3 count{ceilDiv(box.size(0), stride), ceilDiv(box.size(1), stride),
                     ceilDiv(box.size(2), stride)};
  if (static_cast<std::int64_t>(out.size()) != count[0] * count[1] * count[2]) {
    throw std::invalid_argument("Output buffer size does not match the region");
  }
  const std::int64_t b = impl_->info.brick_size;
  Index3 current{-1, -1, -1};
  openvdb::Int32Grid::ConstPtr grid;
  std::optional<openvdb::Int32Grid::ConstAccessor> accessor;
  std::size_t offset = 0;
  for (std::int64_t k = 0; k < count[2]; ++k) {
    for (std::int64_t j = 0; j < count[1]; ++j) {
      for (std::int64_t i = 0; i < count[0]; ++i) {
        const Index3 voxel{box.min[0] + i * stride, box.min[1] + j * stride,
                           box.min[2] + k * stride};
        const Index3 brick{voxel[0] / b, voxel[1] / b, voxel[2] / b};
        if (brick != current) {
          current = brick;
          grid = impl_->brick(brick);
          accessor.reset();
          if (grid) {
            accessor.emplace(grid->getConstAccessor());
          }
        }
        out[offset++] = accessor ? static_cast<std::uint8_t>(accessor->getValue(openvdb::Coord(
                                       static_cast<int>(voxel[0]), static_cast<int>(voxel[1]),
                                       static_cast<int>(voxel[2]))))
                                 : std::uint8_t{0};
      }
    }
  }
}

MaterialScore scoreMaterials(const MaterialVolume& segmentation, const VolumeSource& labels,
                             const Dataset& dataset, const std::vector<std::int64_t>& part_starts,
                             int part_axis, const std::optional<Box>& region) {
  const MaterialVolumeInfo& info = segmentation.info();
  if (labels.dims() != info.dims || dataset.info().dims != info.dims) {
    throw std::invalid_argument("Labels, dataset and segmentation differ in size");
  }
  const auto axis = static_cast<std::size_t>(part_axis);
  const auto part = [&](std::int64_t position) -> std::uint32_t {
    const auto it = std::upper_bound(part_starts.begin(), part_starts.end(), position);
    return it == part_starts.begin() ? 0U
                                     : static_cast<std::uint32_t>(it - part_starts.begin() - 1);
  };
  Box scored{{0, 0, 0}, info.dims};
  if (region) {
    for (std::size_t a = 0; a < 3; ++a) {
      scored.min[a] = std::clamp<std::int64_t>(region->min[a], 0, info.dims[a]);
      scored.max[a] = std::clamp<std::int64_t>(region->max[a], scored.min[a], info.dims[a]);
    }
    if (scored.voxelCount() == 0) {
      throw std::invalid_argument("The scoring region lies outside the volume");
    }
  }
  // Slabs of one brick height along z keep memory bounded.
  const std::int64_t slab = info.brick_size;
  const auto slabs = ceilDiv(scored.size(2), slab);
  const auto slab_box = [&](std::int64_t s) {
    return Box{
        {scored.min[0], scored.min[1], scored.min[2] + s * slab},
        {scored.max[0], scored.max[1], std::min(scored.min[2] + (s + 1) * slab, scored.max[2])}};
  };

  // Pass 1: grey-value histogram per component (bins of 16 grey values).
  constexpr std::size_t kBins = 4096;
  using Histograms = std::unordered_map<std::uint64_t, std::vector<std::uint32_t>>;
  Histograms histograms;
  for (std::int64_t s = 0; s < slabs; ++s) {
    const Box box = slab_box(s);
    std::vector<std::uint16_t> label(static_cast<std::size_t>(box.voxelCount()));
    std::vector<float> grey(label.size());
    labels.readRegion(box, label);
    dataset.readRegion(0, box, grey, dataset.info().air_level);
    std::size_t index = 0;
    for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
      for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
        for (std::int64_t x = box.min[0]; x < box.max[0]; ++x, ++index) {
          if (label[index] == 0) {
            continue;
          }
          const Index3 p{x, y, z};
          const std::uint64_t key = (std::uint64_t{part(p[axis])} << 16U) | label[index];
          auto& histogram = histograms[key];
          if (histogram.empty()) {
            histogram.assign(kBins, 0);
          }
          ++histogram[static_cast<std::size_t>(std::clamp(grey[index], 0.0F, 65535.0F)) / 16];
        }
      }
    }
  }
  MaterialScore score;
  const std::size_t classes = info.materials.size() + 1;
  score.components_per_material.assign(classes, 0);
  std::unordered_map<std::uint64_t, std::uint8_t> truth;
  for (const auto& [key, histogram] : histograms) {
    const std::uint64_t total =
        std::accumulate(histogram.begin(), histogram.end(), std::uint64_t{0});
    std::uint64_t seen = 0;
    std::size_t bin = 0;
    while (seen * 2 < total) {
      seen += histogram[bin++];
    }
    const float median = (static_cast<float>(bin) - 0.5F) * 16.0F;
    const auto id = static_cast<std::uint8_t>(classOf(median, info.materials));
    truth[key] = id;
    ++score.components;
    ++score.components_per_material[id];
  }

  // Pass 2: confusion matrix.
  score.confusion.assign(classes, std::vector<std::int64_t>(classes, 0));
  for (std::int64_t s = 0; s < slabs; ++s) {
    const Box box = slab_box(s);
    std::vector<std::uint16_t> label(static_cast<std::size_t>(box.voxelCount()));
    std::vector<std::uint8_t> segmented(label.size());
    labels.readRegion(box, label);
    segmentation.readRegion(box, segmented);
    std::size_t index = 0;
    for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
      for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
        for (std::int64_t x = box.min[0]; x < box.max[0]; ++x, ++index) {
          std::uint8_t t = 0;
          if (label[index] != 0) {
            const Index3 p{x, y, z};
            t = truth.at((std::uint64_t{part(p[axis])} << 16U) | label[index]);
          }
          ++score.confusion[t][std::min<std::size_t>(segmented[index], classes - 1)];
        }
      }
    }
  }
  const auto dice = [](double overlap, double a, double b) {
    return a + b > 0.0 ? 2.0 * overlap / (a + b) : 1.0;
  };
  score.dice.assign(classes, 0.0);
  double labelled = 0.0;
  double material = 0.0;
  double both = 0.0;
  for (std::size_t t = 0; t < classes; ++t) {
    for (std::size_t m = 0; m < classes; ++m) {
      const auto n = static_cast<double>(score.confusion[t][m]);
      labelled += t > 0 ? n : 0.0;
      material += m > 0 ? n : 0.0;
      both += t > 0 && m > 0 ? n : 0.0;
    }
  }
  score.dice[0] = dice(both, labelled, material);
  for (std::size_t k = 1; k < classes; ++k) {
    double row = 0.0;
    double column = 0.0;
    for (std::size_t j = 0; j < classes; ++j) {
      row += static_cast<double>(score.confusion[k][j]);
      column += static_cast<double>(score.confusion[j][k]);
    }
    score.dice[k] = dice(static_cast<double>(score.confusion[k][k]), row, column);
  }
  return score;
}

namespace detail {

std::array<std::uint8_t, 3> materialColor(int id) {
  return kColors[static_cast<std::size_t>(std::clamp(id, 1, kMaxMaterials) - 1)];
}

std::filesystem::path materialBrickFile(const std::filesystem::path& dir,
                                        const std::array<std::int64_t, 3>& brick) {
  return dir / "level0" /
         (std::to_string(brick[0]) + "_" + std::to_string(brick[1]) + "_" +
          std::to_string(brick[2]) + ".vdb");
}

void writeMaterialVolumeInfo(const std::filesystem::path& dir, const MaterialVolumeInfo& info) {
  writeJson(dir / "materials.json", toJson(info));
}

}  // namespace detail

}  // namespace voxelsieve
