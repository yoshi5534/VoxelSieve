#pragma once

// Fibre orientation of fibre-reinforced parts from the grey value structure (ADR 0028). Fibres
// and fibre bundles show as streaks: the grey value changes across them and hardly along them.
// The structure tensor of the grey value gradients, summed over a block, has its smallest
// eigenvalue along the fibres. Blocks are grouped into fibre families (warp and weft of a fabric,
// the plies of a laminate) by their direction.

#include <array>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

struct OrientationOptions {
  /// Edge of the blocks in voxels that get one direction each.
  int block_size = 16;
  /// Standard deviation in voxels of the Gaussian that smooths the noise before the gradients.
  double smoothing_sigma = 1.0;
  /// Material segmentation of the dataset (not owned) and the material whose fibres are
  /// analysed, for example the fibre bundles of a fabric; 0 takes the material with the highest
  /// grey value. Without a segmentation, every voxel above the air threshold counts.
  const MaterialVolume* materials = nullptr;
  int material = 0;
  /// Blocks with fewer analysed voxels than this share of the block are left out.
  double min_fill = 0.25;
  /// Blocks whose structure is less directional than this (coherence, 0 to 1) are left out of
  /// the fibre families: resin, voids, isotropic noise.
  double min_coherence = 0.1;
  /// Number of fibre families, 1 to 6 (2 for a woven fabric, one per ply direction for a
  /// laminate).
  int families = 2;
};

/// Direction of the fibres of one block.
struct OrientationBlock {
  std::array<std::int64_t, 3> block{};  // block index; level-0 voxels [block * size, ...)
  std::array<double, 3> direction{};    // unit vector in mm space, sign arbitrary
  /// 1 - smallest / middle eigenvalue of the structure tensor: 0 for isotropic structure, 1 for
  /// perfectly aligned streaks.
  double coherence = 0.0;
  std::int64_t voxels = 0;  // analysed voxels
  int family = 0;           // 1 and up; 0 when below min_coherence
};

/// Blocks of the same fibre direction.
struct FibreFamily {
  int id = 0;
  std::array<double, 3> direction{};  // unit vector, mean direction of its blocks
  /// Share of the analysed voxels of the coherent blocks.
  double fraction = 0.0;
  /// Mean angle between the directions of its blocks and the family direction, in degrees.
  double spread_deg = 0.0;
  std::int64_t blocks = 0;
};

struct OrientationResult {
  VoxelSize voxel_size;
  int block_size = 0;
  int material = 0;  // analysed material id, 0 without a segmentation
  /// Second-order orientation tensor a_ij = <p_i p_j> over the coherent blocks, weighted by
  /// their analysed voxels (row major).
  std::array<double, 9> tensor{};
  /// Eigenvalues (descending) and eigenvectors (rows) of the tensor.
  std::array<double, 3> tensor_values{};
  std::array<std::array<double, 3>, 3> tensor_axes{};
  std::vector<FibreFamily> families;  // largest fraction first
  std::vector<OrientationBlock> blocks;
  std::int64_t analysed_voxels = 0;
  double mean_coherence = 0.0;
};

/// Fibre directions of `dataset` (level 0), streamed tile by tile.
[[nodiscard]] OrientationResult analyzeOrientation(const Dataset& dataset,
                                                   const OrientationOptions& options = {});

/// The result without the blocks (they go into the VDB file).
[[nodiscard]] nlohmann::json toJson(const OrientationResult& result);

/// Writes orientation.json, slices through the middle of the volume along each axis with the
/// blocks coloured by direction (|x| red, |y| green, |z| blue over the grey values,
/// orientation_[xyz].png) and orientation.vdb with the grids "direction" (Vec3s), "coherence"
/// and "family" at block resolution in the dataset's world space.
void writeOrientationResult(const Dataset& dataset, const OrientationResult& result,
                            const std::filesystem::path& dir);

}  // namespace voxelsieve
