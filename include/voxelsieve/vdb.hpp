#pragma once

#include <openvdb/openvdb.h>

#include "voxelsieve/volume.hpp"

namespace voxelsieve {

/// Copies every voxel into an OpenVDB float grid without removing any air. This is the naive
/// baseline that the sieve is benchmarked against. Float is lossless for 16-bit grey values
/// (see docs/adr/0002-float-voxels.md). Voxel (i, j, k) of the volume maps to VDB index (i, j, k);
/// the grid transform carries the voxel size.
[[nodiscard]] openvdb::FloatGrid::Ptr toDenseFloatGrid(const Volume16& volume);

}  // namespace voxelsieve
