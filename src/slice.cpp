#include "voxelsieve/slice.hpp"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

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

VolumePreview readVolumePreview(const Dataset& dataset, std::int64_t max_size,
                                const PorosityResult* porosity, const MaterialVolume* materials) {
  const DatasetInfo& info = dataset.info();
  VolumePreview preview;
  preview.level = static_cast<int>(info.levels.size()) - 1;
  for (const LevelInfo& level : info.levels) {
    if (*std::max_element(level.dims.begin(), level.dims.end()) <= max_size) {
      preview.level = level.level;
      break;
    }
  }
  const LevelInfo& level = dataset.level(preview.level);
  preview.dims = level.dims;
  preview.voxel_size = level.voxel_size;
  const auto plane = static_cast<std::size_t>(level.dims[0] * level.dims[1]);
  const auto depth = static_cast<std::size_t>(level.dims[2]);
  std::vector<float> grey(plane * depth);
  preview.overlay.resize(plane * depth);
  tbb::parallel_for(std::size_t{0}, depth, [&](std::size_t z) {
    SliceRequest request;
    request.axis = 2;
    request.level = preview.level;
    request.index = static_cast<std::int64_t>(z) << static_cast<unsigned>(preview.level);
    request.size = {level.dims[0], level.dims[1]};
    const SliceImage slice = readSlice(dataset, request, porosity, materials);
    std::copy(slice.grey.begin(), slice.grey.end(),
              grey.begin() + static_cast<std::ptrdiff_t>(z * plane));
    std::copy(slice.overlay.begin(), slice.overlay.end(),
              preview.overlay.begin() + static_cast<std::ptrdiff_t>(z * plane));
  });

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
  preview.grey.resize(grey.size());
  const float scale = 255.0F / (preview.high - preview.low);
  for (std::size_t i = 0; i < grey.size(); ++i) {
    preview.grey[i] = static_cast<std::uint8_t>(
        std::lround(std::clamp((grey[i] - preview.low) * scale, 0.0F, 255.0F)));
  }
  return preview;
}

}  // namespace voxelsieve
