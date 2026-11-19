#pragma once

// Connected components of the active voxels of a mask, labelled in parallel.

#include <openvdb/openvdb.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace voxelsieve::detail {

/// Connected components (26-neighbourhood) of the active voxels of a mask.
///
/// The mask is split into cells the size of a leaf: its leaves, and the leaf-sized parts of its
/// active tiles. Every cell is labelled on its own, in parallel, and the pieces that touch across
/// cell faces, edges and corners are joined in a concurrent union-find. Components are numbered in
/// the order of their first voxel in the tree order of the mask, the order a serial flood fill
/// that starts at each unvisited voxel in turn would find them.
///
/// The mask is not copied and must outlive this object. Memory: about 32 bytes per cell and 8 per
/// piece and component; only cells of several pieces store a byte per active voxel. A cell that
/// is completely active (a tile, or a full leaf) is one piece and stores nothing more.
class VoxelComponents {
 public:
  using Leaf = openvdb::BoolTree::LeafNodeType;
  /// Component and piece ids are 32 bits wide.
  using Id = std::uint32_t;

  explicit VoxelComponents(const openvdb::BoolTree& mask);
  VoxelComponents(const VoxelComponents&) = delete;
  VoxelComponents& operator=(const VoxelComponents&) = delete;
  VoxelComponents(VoxelComponents&&) = delete;
  VoxelComponents& operator=(VoxelComponents&&) = delete;
  ~VoxelComponents() = default;

  /// Number of components.
  [[nodiscard]] std::size_t size() const { return sizes_.size(); }

  /// Active voxels of component `c`.
  [[nodiscard]] std::uint64_t componentSize(std::size_t c) const { return sizes_[c]; }

  /// Cells with active voxels, in tree order.
  [[nodiscard]] std::size_t cellCount() const { return origins_.size(); }
  [[nodiscard]] const openvdb::Coord& cellOrigin(std::size_t cell) const { return origins_[cell]; }
  /// The leaf of a cell, or nullptr for a part of an active tile, whose voxels are all active.
  [[nodiscard]] const Leaf* cellLeaf(std::size_t cell) const { return leaves_[cell]; }
  /// Whether every voxel of the cell is active.
  [[nodiscard]] bool cellFull(std::size_t cell) const {
    return leaves_[cell] == nullptr || leaves_[cell]->isValueMaskOn();
  }
  [[nodiscard]] bool isOn(std::size_t cell, openvdb::Index offset) const {
    return leaves_[cell] == nullptr || leaves_[cell]->isValueOn(offset);
  }

  /// Calls `f(offset)` for every active voxel of a cell, in offset order.
  template <typename F>
  void forEachOn(std::size_t cell, F&& f) const {
    if (leaves_[cell] == nullptr) {
      for (openvdb::Index offset = 0; offset < Leaf::SIZE; ++offset) {
        f(offset);
      }
    } else {
      for (auto it = leaves_[cell]->cbeginValueOn(); it; ++it) {
        f(it.pos());
      }
    }
  }

  /// Component of voxel `offset` of a cell; the voxel must be active.
  [[nodiscard]] Id component(std::size_t cell, openvdb::Index offset) const {
    return labels_[piece_begin_[cell] + localPiece(cell, offset)];
  }

  /// Component of an active voxel, or -1 when `voxel` is not active in the mask.
  [[nodiscard]] std::int64_t at(const openvdb::Coord& voxel) const;

  /// Components whose pieces lie in a cell, one entry per piece.
  [[nodiscard]] std::span<const Id> componentsInCell(std::size_t cell) const {
    return {labels_.data() + piece_begin_[cell], piece_begin_[cell + 1] - piece_begin_[cell]};
  }

  /// Index of the cell at `origin`, or -1 when the mask has no active voxel there.
  [[nodiscard]] std::int64_t findCell(const openvdb::Coord& origin) const;

 private:
  static constexpr std::uint64_t kSinglePiece = std::numeric_limits<std::uint64_t>::max();

  /// Piece of an active voxel within its cell.
  [[nodiscard]] Id localPiece(std::size_t cell, openvdb::Index offset) const;

  std::vector<openvdb::Coord> origins_;  // in tree order
  std::vector<const Leaf*> leaves_;
  /// First piece of each cell, plus the total at the end.
  std::vector<Id> piece_begin_;
  /// Where the pieces of the active voxels of a cell of several pieces start in local_, in the
  /// order of the voxels; kSinglePiece for cells of one piece.
  std::vector<std::uint64_t> local_begin_;
  std::vector<std::uint8_t> local_;
  /// Component of every piece.
  std::vector<Id> labels_;
  /// Voxels of every component.
  std::vector<std::uint64_t> sizes_;
};

}  // namespace voxelsieve::detail
