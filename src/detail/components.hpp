#pragma once

// Connected components of the active voxels of a mask, labelled in parallel.

#include <openvdb/openvdb.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace voxelsieve::detail {

/// Connected components (26-neighbourhood) of the active voxels of a mask.
///
/// Every leaf of the mask is labelled on its own, in parallel, and the pieces that touch across
/// leaf faces, edges and corners are joined in a concurrent union-find. Components are numbered
/// in the order of their first voxel in the tree order of the mask, the order a serial flood fill
/// that starts at each unvisited voxel in turn would find them. Memory: one byte per voxel of the
/// leaves that hold active voxels, plus a few bytes per piece.
class VoxelComponents {
 public:
  using Leaf = openvdb::BoolTree::LeafNodeType;

  /// Labels the active voxels of `mask`. Active tiles are voxelized in a copy first.
  explicit VoxelComponents(const openvdb::BoolTree& mask);
  VoxelComponents(const VoxelComponents&) = delete;
  VoxelComponents& operator=(const VoxelComponents&) = delete;
  VoxelComponents(VoxelComponents&&) = delete;
  VoxelComponents& operator=(VoxelComponents&&) = delete;
  ~VoxelComponents() = default;

  /// Number of components.
  [[nodiscard]] std::size_t size() const { return count_; }

  /// Leaves of the (voxelized) mask, in tree order.
  [[nodiscard]] std::size_t leafCount() const { return leaves_.size(); }
  [[nodiscard]] const Leaf& leaf(std::size_t index) const { return *leaves_[index]; }

  /// Component of voxel `offset` of leaf `index`; the voxel must be active.
  [[nodiscard]] std::size_t component(std::size_t index, openvdb::Index offset) const {
    return labels_[piece_begin_[index] + local_[index * Leaf::SIZE + offset]];
  }

  /// Component of an active voxel, or -1 when `voxel` is not active in the mask.
  [[nodiscard]] std::int64_t at(const openvdb::Coord& voxel) const;

  /// Components whose pieces lie in leaf `index`.
  [[nodiscard]] std::span<const std::size_t> componentsInLeaf(std::size_t index) const {
    return {labels_.data() + piece_begin_[index], piece_begin_[index + 1] - piece_begin_[index]};
  }

 private:
  struct CoordHash {
    std::size_t operator()(const openvdb::Coord& c) const {
      return std::hash<std::int64_t>()((static_cast<std::int64_t>(c.x()) * 73856093) ^
                                       (static_cast<std::int64_t>(c.y()) * 19349663) ^
                                       (static_cast<std::int64_t>(c.z()) * 83492791));
    }
  };

  openvdb::BoolTree tree_;
  std::vector<const Leaf*> leaves_;  // the leaves of tree_ that hold active voxels
  std::unordered_map<openvdb::Coord, std::size_t, CoordHash> leaf_index_;
  /// Local piece of every voxel of every leaf (one byte: a leaf holds at most 64 pieces).
  std::vector<std::uint8_t> local_;
  /// First piece of each leaf, plus the total at the end.
  std::vector<std::size_t> piece_begin_;
  /// Component of every piece.
  std::vector<std::size_t> labels_;
  std::size_t count_ = 0;
};

}  // namespace voxelsieve::detail
