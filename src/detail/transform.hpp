#pragma once

#include <openvdb/math/Transform.h>

#include <memory>

#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve::detail {

/// Index-to-world transform in mm for voxels of `size` (ADR 0012): a uniform scale for cubic
/// voxels, a scale per axis otherwise. `level` > 0 is a level whose voxels cover 2^level level-0
/// voxels per axis, centred on them.
inline openvdb::math::Transform::Ptr voxelTransform(const VoxelSize& size, int level = 0) {
  const auto scale = static_cast<double>(std::int64_t{1} << level);
  const openvdb::Vec3d pitch(size[0], size[1], size[2]);
  auto transform = size.isotropic()
                       ? openvdb::math::Transform::createLinearTransform(size[0] * scale)
                       : std::make_shared<openvdb::math::Transform>(
                             std::make_shared<openvdb::math::ScaleMap>(pitch * scale));
  // Level-L voxel i covers level-0 voxels [i * 2^L, (i + 1) * 2^L).
  transform->postTranslate(pitch * ((scale - 1.0) / 2.0));
  return transform;
}

/// Voxel size of a grid's transform (the inverse of voxelTransform, without slice thickness).
inline VoxelSize voxelSizeOf(const openvdb::math::Transform& transform) {
  const openvdb::Vec3d size = transform.voxelSize();
  return {size[0], size[1], size[2]};
}

}  // namespace voxelsieve::detail
