#include "voxelsieve/slice.hpp"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>

namespace voxelsieve {
namespace {

constexpr std::int64_t kZoneBlock = 8;  // zone_blocks holds one value per 8^3 block

/// Level-0 box covered by the slice rectangle (clipped to the volume).
struct Footprint {
  std::array<std::int64_t, 3> min{};
  std::array<std::int64_t, 3> max{};
  [[nodiscard]] bool intersects(const openvdb::CoordBBox& box) const {
    for (std::size_t a = 0; a < 3; ++a) {
      if (box.max()[a] < min[a] || box.min()[a] >= max[a]) {
        return false;
      }
    }
    return true;
  }
};

}  // namespace

std::array<int, 2> sliceAxes(int axis) {
  switch (axis) {
    case 0:
      return {1, 2};
    case 1:
      return {0, 2};
    case 2:
      return {0, 1};
    default:
      throw std::invalid_argument("Slice axis must be 0 (x), 1 (y) or 2 (z)");
  }
}

SliceImage readSlice(const Dataset& dataset, const SliceRequest& request,
                     const PorosityResult* porosity, const MaterialVolume* materials) {
  const auto [u, v] = sliceAxes(request.axis);
  const auto normal = static_cast<std::size_t>(request.axis);
  const auto ua = static_cast<std::size_t>(u);
  const auto va = static_cast<std::size_t>(v);
  const DatasetInfo& info = dataset.info();
  if (request.level < 0 || static_cast<std::size_t>(request.level) >= info.levels.size()) {
    throw std::invalid_argument("Level " + std::to_string(request.level) + " does not exist");
  }
  if (request.index < 0 || request.index >= info.dims[normal]) {
    throw std::invalid_argument("Slice " + std::to_string(request.index) +
                                " is outside the volume");
  }
  constexpr std::int64_t kMaxPixels = std::int64_t{4096} * 4096;
  if (request.size[0] <= 0 || request.size[1] <= 0 ||
      request.size[0] * request.size[1] > kMaxPixels) {
    throw std::invalid_argument("Slice size must be positive and at most 4096 x 4096");
  }
  const LevelInfo& level = dataset.level(request.level);
  const auto shift = static_cast<unsigned>(request.level);

  SliceImage image;
  image.width = request.size[0];
  image.height = request.size[1];
  const auto pixels = static_cast<std::size_t>(image.width * image.height);
  image.grey.assign(pixels, info.air_level);
  image.overlay.assign(pixels, 0);

  Box box;
  box.min[normal] = std::min(request.index >> shift, level.dims[normal] - 1);
  box.max[normal] = box.min[normal] + 1;
  box.min[ua] = std::clamp<std::int64_t>(request.origin[0], 0, level.dims[ua]);
  box.max[ua] = std::clamp<std::int64_t>(request.origin[0] + image.width, 0, level.dims[ua]);
  box.min[va] = std::clamp<std::int64_t>(request.origin[1], 0, level.dims[va]);
  box.max[va] = std::clamp<std::int64_t>(request.origin[1] + image.height, 0, level.dims[va]);
  if (box.voxelCount() <= 0) {
    return image;
  }
  // The normal axis has size 1 and u < v, so the region is stored u fastest, like the image.
  std::vector<float> region(static_cast<std::size_t>(box.voxelCount()));
  dataset.readRegion(request.level, box, region, info.air_level);
  const std::int64_t nu = box.size(ua);
  for (std::int64_t y = 0; y < box.size(va); ++y) {
    const std::int64_t row = box.min[va] - request.origin[1] + y;
    const std::int64_t column = box.min[ua] - request.origin[0];
    std::copy_n(region.begin() + y * nu, nu, image.grey.begin() + row * image.width + column);
  }

  if (materials != nullptr) {
    // Every 2^level-th level-0 voxel; the normal axis at the requested slice.
    const std::int64_t stride = std::int64_t{1} << shift;
    Box footprint0;
    for (std::size_t a = 0; a < 3; ++a) {
      footprint0.min[a] = box.min[a] << shift;
      footprint0.max[a] = std::min(box.max[a] << shift, info.dims[a]);
    }
    footprint0.min[normal] = request.index;
    footprint0.max[normal] = request.index + 1;
    std::vector<std::uint8_t> ids(static_cast<std::size_t>(box.voxelCount()));
    materials->readRegion(footprint0, ids, stride);
    for (std::int64_t y = 0; y < box.size(va); ++y) {
      const std::int64_t row = box.min[va] - request.origin[1] + y;
      const std::int64_t column = box.min[ua] - request.origin[0];
      for (std::int64_t x = 0; x < nu; ++x) {
        const std::uint8_t id = ids[static_cast<std::size_t>(y * nu + x)];
        if (id != 0) {
          image.overlay[static_cast<std::size_t>(row * image.width + column + x)] =
              static_cast<std::uint8_t>(static_cast<int>(SliceOverlay::kMaterial) + id);
        }
      }
    }
  }
  if (porosity == nullptr) {
    return image;
  }
  Footprint footprint;
  for (std::size_t a = 0; a < 3; ++a) {
    footprint.min[a] = box.min[a] << shift;
    footprint.max[a] = std::min(box.max[a] << shift, info.dims[a]);
  }
  const auto mark = [&](const openvdb::Coord& voxel, SliceOverlay value) {
    const std::int64_t px = (std::int64_t{voxel[ua]} >> shift) - request.origin[0];
    const std::int64_t py = (std::int64_t{voxel[va]} >> shift) - request.origin[1];
    auto& pixel = image.overlay[static_cast<std::size_t>(py * image.width + px)];
    if (pixel != static_cast<std::uint8_t>(SliceOverlay::kPore)) {  // pores win over the rest
      pixel = static_cast<std::uint8_t>(value);
    }
  };
  if (porosity->pore_labels) {
    for (auto leaf = porosity->pore_labels->tree().cbeginLeaf(); leaf; ++leaf) {
      if (!footprint.intersects(leaf->getNodeBoundingBox())) {
        continue;
      }
      for (auto voxel = leaf->cbeginValueOn(); voxel; ++voxel) {
        const openvdb::Coord xyz = voxel.getCoord();
        if (footprint.intersects(openvdb::CoordBBox(xyz, xyz))) {
          mark(xyz, SliceOverlay::kPore);
        }
      }
    }
  }
  if (porosity->zone_blocks) {
    for (auto block = porosity->zone_blocks->cbeginValueOn(); block; ++block) {
      openvdb::CoordBBox blocks;
      block.getBoundingBox(blocks);
      constexpr int kEdge = static_cast<int>(kZoneBlock);
      const openvdb::CoordBBox voxels(
          openvdb::Coord(blocks.min().asVec3i() * kEdge),
          openvdb::Coord((blocks.max().asVec3i() + openvdb::Vec3i(1)) * kEdge - openvdb::Vec3i(1)));
      if (!footprint.intersects(voxels)) {
        continue;
      }
      openvdb::Coord xyz;
      xyz[normal] = static_cast<int>(footprint.min[normal]);
      const std::int64_t step = std::int64_t{1} << shift;
      for (std::int64_t b = std::max<std::int64_t>(voxels.min()[va], footprint.min[va]);
           b <= std::min<std::int64_t>(voxels.max()[va], footprint.max[va] - 1); b += step) {
        for (std::int64_t a = std::max<std::int64_t>(voxels.min()[ua], footprint.min[ua]);
             a <= std::min<std::int64_t>(voxels.max()[ua], footprint.max[ua] - 1); a += step) {
          xyz[ua] = static_cast<int>(a);
          xyz[va] = static_cast<int>(b);
          mark(xyz, SliceOverlay::kZone);
        }
      }
    }
  }
  return image;
}

PlaneImage samplePlane(const Dataset& dataset, const PlaneRequest& request) {
  const DatasetInfo& info = dataset.info();
  if (request.level < 0 || static_cast<std::size_t>(request.level) >= info.levels.size()) {
    throw std::invalid_argument("Level " + std::to_string(request.level) + " does not exist");
  }
  constexpr std::int64_t kMaxPixels = std::int64_t{4096} * 4096;
  if (request.width <= 0 || request.height <= 0 || request.width * request.height > kMaxPixels) {
    throw std::invalid_argument("Plane size must be positive and at most 4096 x 4096");
  }
  PlaneImage image;
  image.width = request.width;
  image.height = request.height;
  const auto pixels = static_cast<std::size_t>(image.width * image.height);
  image.grey.assign(pixels, info.air_level);
  image.inside.assign(pixels, kPlaneOutside);
  const float threshold = info.threshold;
  image.threshold = threshold;
  const LevelInfo& level = dataset.level(request.level);
  const double scale = std::ldexp(1.0, request.level);
  const std::int64_t brick_size = info.brick_size;
  tbb::parallel_for(std::int64_t{0}, image.height, [&](std::int64_t y) {
    // Neighbouring pixels mostly share a brick: keep it and its accessor between them.
    std::array<std::int64_t, 3> current{-1, -1, -1};
    Dataset::BrickPtr brick;
    std::variant<std::monostate, openvdb::FloatGrid::ConstAccessor, UInt16Grid::ConstAccessor,
                 UInt8Grid::ConstAccessor>
        accessor;
    const auto probe = [&](const std::array<std::int64_t, 3>& voxel) -> std::optional<float> {
      for (std::size_t a = 0; a < 3; ++a) {
        if (voxel[a] < 0 || voxel[a] >= level.dims[a]) {
          return std::nullopt;
        }
      }
      const std::array<std::int64_t, 3> index{voxel[0] / brick_size, voxel[1] / brick_size,
                                              voxel[2] / brick_size};
      if (index != current) {
        current = index;
        brick = dataset.brick(request.level, index);
        accessor = std::monostate{};
        if (brick) {
          brick->visit([&](const auto& grid) { accessor = grid.getConstAccessor(); });
        }
      }
      const openvdb::Coord coord(static_cast<int>(voxel[0]), static_cast<int>(voxel[1]),
                                 static_cast<int>(voxel[2]));
      return std::visit(
          [&](const auto& typed) -> std::optional<float> {
            if constexpr (std::is_same_v<std::decay_t<decltype(typed)>, std::monostate>) {
              return std::nullopt;
            } else {
              typename std::decay_t<decltype(typed)>::ValueType value{};
              if (typed.probeValue(coord, value)) {
                return static_cast<float>(value);
              }
              return std::nullopt;
            }
          },
          accessor);
    };
    for (std::int64_t x = 0; x < image.width; ++x) {
      // mm to the continuous level index, whose whole numbers are voxel centres: level voxel j
      // is centred on level-0 index (j + 0.5) * 2^level - 0.5.
      std::array<double, 3> c{};
      for (std::size_t a = 0; a < 3; ++a) {
        const double mm = request.origin_mm[a] + static_cast<double>(x) * request.du_mm[a] +
                          static_cast<double>(y) * request.dv_mm[a];
        c[a] = (mm / info.voxel_size[a] + 0.5) / scale - 0.5;
      }
      const auto i = static_cast<std::size_t>(y * image.width + x);
      if (!request.linear) {
        const std::optional<float> value =
            probe({static_cast<std::int64_t>(std::floor(c[0] + 0.5)),
                   static_cast<std::int64_t>(std::floor(c[1] + 0.5)),
                   static_cast<std::int64_t>(std::floor(c[2] + 0.5))});
        if (value) {
          image.grey[i] = *value;
          image.inside[i] = *value >= threshold ? kPlaneMaterial : kPlaneVoid;
        }
        continue;
      }
      std::array<std::int64_t, 3> low{};
      std::array<double, 3> t{};
      for (std::size_t a = 0; a < 3; ++a) {
        const double floor = std::floor(c[a]);
        low[a] = static_cast<std::int64_t>(floor);
        t[a] = c[a] - floor;
      }
      double sum = 0.0;
      double active = 0.0;
      for (unsigned corner = 0; corner < 8; ++corner) {
        double weight = 1.0;
        std::array<std::int64_t, 3> voxel = low;
        for (std::size_t a = 0; a < 3; ++a) {
          const bool high = ((corner >> a) & 1U) != 0;
          voxel[a] += high ? 1 : 0;
          weight *= high ? t[a] : 1.0 - t[a];
        }
        if (weight == 0.0) {
          continue;
        }
        const std::optional<float> value = probe(voxel);
        sum += weight * (value ? *value : info.air_level);
        active += value ? weight : 0.0;
      }
      if (active > 0.0) {
        const auto value = static_cast<float>(sum);
        image.grey[i] = value;
        image.inside[i] = value >= threshold ? kPlaneMaterial : kPlaneVoid;
      }
    }
  });
  return image;
}

int levelForPixel(const DatasetInfo& info, double pixel_mm) {
  int chosen = 0;
  for (const LevelInfo& level : info.levels) {
    if (level.voxel_size.minMm() <= pixel_mm * (1.0 + 1e-9)) {
      chosen = level.level;
    }
  }
  return chosen;
}

std::vector<float> cutMesh(const IndexedMesh& mesh, int axis, double position) {
  return cutMesh(mesh.points, mesh.triangles, axis, position);
}

std::vector<float> cutMesh(std::span<const std::array<float, 3>> points,
                           std::span<const std::array<std::uint32_t, 3>> triangles, int axis,
                           double position) {
  const auto [u, v] = sliceAxes(axis);
  const auto n = static_cast<std::size_t>(axis);
  const auto ua = static_cast<std::size_t>(u);
  const auto va = static_cast<std::size_t>(v);
  std::vector<float> segments;
  for (const auto& triangle : triangles) {
    std::array<std::array<double, 2>, 2> ends{};
    int found = 0;
    for (std::size_t k = 0; k < 3 && found < 2; ++k) {
      const auto& a = points[triangle[k]];
      const auto& b = points[triangle[(k + 1) % 3]];
      const double da = a[n] - position;
      const double db = b[n] - position;
      // Half-open, so a vertex on the plane counts for one of its two edges only.
      if ((da < 0.0) == (db < 0.0)) {
        continue;
      }
      const double t = da / (da - db);
      ends[static_cast<std::size_t>(found)] = {a[ua] + t * (b[ua] - a[ua]),
                                               a[va] + t * (b[va] - a[va])};
      ++found;
    }
    if (found == 2) {
      for (const auto& end : ends) {
        segments.push_back(static_cast<float>(end[0]));
        segments.push_back(static_cast<float>(end[1]));
      }
    }
  }
  return segments;
}

VolumePreview readVolumePreview(const Dataset& dataset, const VolumeRequest& request,
                                const PorosityResult* porosity, const MaterialVolume* materials) {
  const DatasetInfo& info = dataset.info();
  const Box whole{{0, 0, 0}, info.dims};
  Box region = request.region.value_or(whole);
  for (std::size_t a = 0; a < 3; ++a) {
    region.min[a] = std::clamp<std::int64_t>(region.min[a], 0, info.dims[a]);
    region.max[a] = std::clamp<std::int64_t>(region.max[a], region.min[a], info.dims[a]);
  }
  if (region.voxelCount() == 0) {
    throw std::invalid_argument("The region holds no voxel of the volume");
  }
  // The region at a level: the level voxels that cover it.
  const auto at_level = [&region](int level) {
    Box box;
    for (std::size_t a = 0; a < 3; ++a) {
      box.min[a] = region.min[a] >> static_cast<unsigned>(level);
      box.max[a] = ((region.max[a] - 1) >> static_cast<unsigned>(level)) + 1;
    }
    return box;
  };
  VolumePreview preview;
  preview.level = static_cast<int>(info.levels.size()) - 1;
  for (const LevelInfo& level : info.levels) {
    const Box box = at_level(level.level);
    if (std::max({box.size(0), box.size(1), box.size(2)}) <= request.max_size) {
      preview.level = level.level;
      break;
    }
  }
  const LevelInfo& level = dataset.level(preview.level);
  const Box box = at_level(preview.level);
  preview.origin = box.min;
  preview.dims = {box.size(0), box.size(1), box.size(2)};
  preview.voxel_size = level.voxel_size;
  const auto plane = static_cast<std::size_t>(box.size(0) * box.size(1));
  const auto depth = static_cast<std::size_t>(box.size(2));
  std::vector<float> grey(plane * depth);
  preview.overlay.resize(plane * depth);
  tbb::parallel_for(std::size_t{0}, depth, [&](std::size_t z) {
    SliceRequest slice_request;
    slice_request.axis = 2;
    slice_request.level = preview.level;
    slice_request.index = (box.min[2] + static_cast<std::int64_t>(z))
                          << static_cast<unsigned>(preview.level);
    slice_request.origin = {box.min[0], box.min[1]};
    slice_request.size = {box.size(0), box.size(1)};
    const SliceImage slice = readSlice(dataset, slice_request, porosity, materials);
    std::copy(slice.grey.begin(), slice.grey.end(),
              grey.begin() + static_cast<std::ptrdiff_t>(z * plane));
    std::copy(slice.overlay.begin(), slice.overlay.end(),
              preview.overlay.begin() + static_cast<std::ptrdiff_t>(z * plane));
  });

  if (request.window) {
    preview.low = (*request.window)[0];
    preview.high = std::max((*request.window)[1], preview.low + 1.0F);
  } else {
    std::vector<float> sample;
    for (std::size_t i = 0; i < grey.size(); i += 7) {
      sample.push_back(grey[i]);
    }
    std::sort(sample.begin(), sample.end());
    const auto at = [&sample](double fraction) {
      return sample[static_cast<std::size_t>(fraction * static_cast<double>(sample.size() - 1))];
    };
    preview.low = at(0.005);
    preview.high = std::max(at(0.995), preview.low + 1.0F);
  }
  preview.grey.resize(grey.size());
  const float scale = 255.0F / (preview.high - preview.low);
  for (std::size_t i = 0; i < grey.size(); ++i) {
    preview.grey[i] = static_cast<std::uint8_t>(
        std::lround(std::clamp((grey[i] - preview.low) * scale, 0.0F, 255.0F)));
  }
  return preview;
}

VolumePreview readVolumePreview(const Dataset& dataset, std::int64_t max_size,
                                const PorosityResult* porosity, const MaterialVolume* materials) {
  VolumeRequest request;
  request.max_size = max_size;
  return readVolumePreview(dataset, request, porosity, materials);
}

}  // namespace voxelsieve
