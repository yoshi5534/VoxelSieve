#include "detail/components.hpp"

#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace voxelsieve::detail {
namespace {

using Coord = openvdb::Coord;
using Leaf = VoxelComponents::Leaf;
using Id = VoxelComponents::Id;
constexpr int kDim = static_cast<int>(Leaf::DIM);
constexpr std::uint8_t kUnlabelled = std::numeric_limits<std::uint8_t>::max();

/// The 26 neighbour offsets.
const std::array<Coord, 26>& neighbours26() {
  static const std::array<Coord, 26> offsets = [] {
    std::array<Coord, 26> result{};
    std::size_t i = 0;
    for (int z = -1; z <= 1; ++z) {
      for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
          if (x != 0 || y != 0 || z != 0) {
            result[i++] = Coord(x, y, z);
          }
        }
      }
    }
    return result;
  }();
  return offsets;
}

/// Index of the neighbour cell in direction (-1..1 per axis), 13 for the cell itself.
int directionIndex(int dx, int dy, int dz) { return (dx + 1) + 3 * ((dy + 1) + 3 * (dz + 1)); }

/// Whether cell origin `a` comes before `b` in the tree order of a 5-4-3 tree: the root orders
/// its 4096^3 nodes by origin, and each internal node its children x slowest, z fastest.
bool treeOrder(const Coord& a, const Coord& b) {
  for (const int mask : {~4095, ~127, ~7}) {
    const Coord ka(a.x() & mask, a.y() & mask, a.z() & mask);
    const Coord kb(b.x() & mask, b.y() & mask, b.z() & mask);
    if (ka != kb) {
      return ka < kb;
    }
  }
  return false;
}

static_assert(
    std::is_same_v<openvdb::BoolTree::RootNodeType::ChildNodeType::ChildNodeType::ChildNodeType,
                   Leaf> &&
        openvdb::BoolTree::RootNodeType::ChildNodeType::LOG2DIM == 5 &&
        openvdb::BoolTree::RootNodeType::ChildNodeType::ChildNodeType::LOG2DIM == 4,
    "treeOrder assumes the standard 5-4-3 tree");

/// Labels the active voxels of one leaf by 26-connected pieces; returns the number of pieces.
std::size_t labelLeaf(const Leaf& leaf, std::uint8_t* labels) {
  std::fill(labels, labels + Leaf::SIZE, kUnlabelled);
  std::array<openvdb::Index, Leaf::SIZE> stack{};
  std::size_t pieces = 0;
  for (auto it = leaf.cbeginValueOn(); it; ++it) {
    const openvdb::Index start = it.pos();
    if (labels[start] != kUnlabelled) {
      continue;
    }
    const auto piece = static_cast<std::uint8_t>(pieces++);
    labels[start] = piece;
    std::size_t top = 0;
    stack[top++] = start;
    while (top > 0) {
      const Coord c = Leaf::offsetToLocalCoord(stack[--top]);
      for (const Coord& d : neighbours26()) {
        const Coord n = c + d;
        if (n.x() < 0 || n.y() < 0 || n.z() < 0 || n.x() >= kDim || n.y() >= kDim ||
            n.z() >= kDim) {
          continue;
        }
        const openvdb::Index offset = Leaf::coordToOffset(n);
        if (labels[offset] == kUnlabelled && leaf.isValueOn(offset)) {
          labels[offset] = piece;
          stack[top++] = offset;
        }
      }
    }
  }
  return pieces;
}

/// Union-find over the pieces that many threads join at once. Roots link to the smaller index,
/// so the root of a set is its smallest piece and every parent is at most its child.
class ConcurrentUnionFind {
 public:
  explicit ConcurrentUnionFind(std::size_t size) : parent_(size) {
    tbb::parallel_for(std::size_t{0}, size, [&](std::size_t i) {
      parent_[i].store(static_cast<Id>(i), std::memory_order_relaxed);
    });
  }

  Id find(Id x) {
    while (true) {
      Id parent = parent_[x].load();
      if (parent == x) {
        return x;
      }
      const Id grandparent = parent_[parent].load();
      // Path halving; losing the race to another thread only skips the shortcut.
      parent_[x].compare_exchange_weak(parent, grandparent);
      x = grandparent;
    }
  }

  void unite(Id a, Id b) {
    while (true) {
      a = find(a);
      b = find(b);
      if (a == b) {
        return;
      }
      if (a < b) {
        std::swap(a, b);
      }
      Id expected = a;
      if (parent_[a].compare_exchange_strong(expected, b)) {
        return;
      }
    }
  }

 private:
  std::vector<std::atomic<Id>> parent_;
};

}  // namespace

VoxelComponents::VoxelComponents(const openvdb::BoolTree& mask) {
  // Cells: the leaves with active voxels and the leaf-sized parts of the active tiles.
  std::vector<std::pair<Coord, const Leaf*>> cells;
  for (auto it = mask.cbeginLeaf(); it; ++it) {
    if (!it->isEmpty()) {
      cells.emplace_back(it->origin(), it.getLeaf());
    }
  }
  auto tile = mask.cbeginValueOn();
  tile.setMaxDepth(openvdb::BoolTree::ValueOnCIter::LEAF_DEPTH - 1);
  for (; tile; ++tile) {
    openvdb::CoordBBox box;
    tile.getBoundingBox(box);
    for (int x = box.min().x(); x <= box.max().x(); x += kDim) {
      for (int y = box.min().y(); y <= box.max().y(); y += kDim) {
        for (int z = box.min().z(); z <= box.max().z(); z += kDim) {
          cells.emplace_back(Coord(x, y, z), nullptr);
        }
      }
    }
  }
  tbb::parallel_sort(cells.begin(), cells.end(),
                     [](const auto& a, const auto& b) { return treeOrder(a.first, b.first); });
  const std::size_t n = cells.size();
  origins_.resize(n);
  leaves_.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    origins_[i] = cells[i].first;
    leaves_[i] = cells[i].second;
  }
  cells = {};

  // Pieces of each cell on its own. Cells of one piece store nothing per voxel; the others a
  // byte per active voxel, written in a second pass once the offsets are known.
  std::vector<std::uint16_t> pieces(n);
  tbb::parallel_for(std::size_t{0}, n, [&](std::size_t i) {
    if (cellFull(i)) {
      pieces[i] = 1;
      return;
    }
    std::array<std::uint8_t, Leaf::SIZE> local{};
    pieces[i] = static_cast<std::uint16_t>(labelLeaf(*leaves_[i], local.data()));
  });
  piece_begin_.resize(n + 1);
  local_begin_.resize(n);
  std::uint64_t total = 0;
  std::uint64_t local_size = 0;
  for (std::size_t i = 0; i < n; ++i) {
    piece_begin_[i] = static_cast<Id>(total);
    total += pieces[i];
    if (pieces[i] > 1) {
      local_begin_[i] = local_size;
      local_size += leaves_[i]->onVoxelCount();
    } else {
      local_begin_[i] = kSinglePiece;
    }
  }
  if (total > std::numeric_limits<Id>::max()) {
    throw std::length_error("A mask of more than 2^32 connected pieces");
  }
  piece_begin_[n] = static_cast<Id>(total);
  pieces = {};
  local_.resize(local_size);
  std::vector<std::uint16_t> piece_sizes(total);
  tbb::parallel_for(std::size_t{0}, n, [&](std::size_t i) {
    const Id first = piece_begin_[i];
    if (local_begin_[i] == kSinglePiece) {
      piece_sizes[first] = static_cast<std::uint16_t>(
          leaves_[i] == nullptr ? Leaf::SIZE : leaves_[i]->onVoxelCount());
      return;
    }
    std::array<std::uint8_t, Leaf::SIZE> local{};
    labelLeaf(*leaves_[i], local.data());
    std::uint64_t next = local_begin_[i];
    for (auto it = leaves_[i]->cbeginValueOn(); it; ++it) {
      const std::uint8_t piece = local[it.pos()];
      local_[next++] = piece;
      ++piece_sizes[first + piece];
    }
  });

  // Join the pieces that touch across cell boundaries. Each pair of voxels is visited from both
  // sides; joining twice does no harm and keeps the loop simple. Two cells of one piece each need
  // joining once: whole cells at once when both are full, otherwise at the first touching voxel.
  ConcurrentUnionFind sets(total);
  tbb::parallel_for(std::size_t{0}, n, [&](std::size_t i) {
    const Coord origin = origins_[i];
    const bool single = local_begin_[i] == kSinglePiece;
    std::array<std::int64_t, 27> neighbour{};
    neighbour.fill(-1);
    std::array<bool, 27> done{};
    bool open = false;
    for (int dz = -1; dz <= 1; ++dz) {
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0 && dz == 0) {
            continue;
          }
          const std::int64_t j = findCell(origin.offsetBy(dx * kDim, dy * kDim, dz * kDim));
          if (j < 0) {
            continue;
          }
          const auto d = static_cast<std::size_t>(directionIndex(dx, dy, dz));
          neighbour[d] = j;
          const auto other = static_cast<std::size_t>(j);
          if (single && cellFull(i) && cellFull(other)) {
            sets.unite(piece_begin_[i], piece_begin_[other]);
            done[d] = true;
          } else {
            open = true;
          }
        }
      }
    }
    if (!open) {
      return;
    }
    forEachOn(i, [&](openvdb::Index pos) {
      const Coord c = Leaf::offsetToLocalCoord(pos);
      if (c.x() > 0 && c.y() > 0 && c.z() > 0 && c.x() < kDim - 1 && c.y() < kDim - 1 &&
          c.z() < kDim - 1) {
        return;
      }
      const Id own = piece_begin_[i] + localPiece(i, pos);
      for (const Coord& e : neighbours26()) {
        const Coord p = c + e;
        const auto side = [](int v) { return v < 0 ? -1 : (v >= kDim ? 1 : 0); };
        const int dx = side(p.x());
        const int dy = side(p.y());
        const int dz = side(p.z());
        if (dx == 0 && dy == 0 && dz == 0) {
          continue;
        }
        const auto d = static_cast<std::size_t>(directionIndex(dx, dy, dz));
        const std::int64_t j = neighbour[d];
        if (j < 0 || done[d]) {
          continue;
        }
        const auto other = static_cast<std::size_t>(j);
        const openvdb::Index offset = Leaf::coordToOffset(p);  // wraps into the neighbour cell
        if (isOn(other, offset)) {
          sets.unite(own, piece_begin_[other] + localPiece(other, offset));
          if (single && local_begin_[other] == kSinglePiece) {
            done[d] = true;
          }
        }
      }
    });
  });

  // Number the sets by their smallest piece, which holds their first voxel in tree order. Roots
  // come before their pieces, so the labels can replace the roots in one ascending pass.
  labels_.resize(total);
  tbb::parallel_for(std::uint64_t{0}, total,
                    [&](std::uint64_t k) { labels_[k] = sets.find(static_cast<Id>(k)); });
  Id count = 0;
  for (std::uint64_t k = 0; k < total; ++k) {
    labels_[k] = labels_[k] == k ? count++ : labels_[labels_[k]];
  }
  sizes_.assign(count, 0);
  for (std::uint64_t k = 0; k < total; ++k) {
    sizes_[labels_[k]] += piece_sizes[k];
  }
}

VoxelComponents::Id VoxelComponents::localPiece(std::size_t cell, openvdb::Index offset) const {
  const std::uint64_t begin = local_begin_[cell];
  if (begin == kSinglePiece) {
    return 0;
  }
  // Rank of the voxel among the active voxels of its leaf.
  const auto& mask = leaves_[cell]->getValueMask();
  const openvdb::Index word = offset >> 6U;
  std::uint64_t rank = 0;
  for (openvdb::Index w = 0; w < word; ++w) {
    rank += static_cast<std::uint64_t>(std::popcount(mask.getWord<std::uint64_t>(w)));
  }
  const std::uint64_t below = (std::uint64_t{1} << (offset & 63U)) - 1U;
  rank += static_cast<std::uint64_t>(std::popcount(mask.getWord<std::uint64_t>(word) & below));
  return local_[begin + rank];
}

std::int64_t VoxelComponents::findCell(const Coord& origin) const {
  const auto it = std::lower_bound(origins_.begin(), origins_.end(), origin, treeOrder);
  if (it == origins_.end() || *it != origin) {
    return -1;
  }
  return it - origins_.begin();
}

std::int64_t VoxelComponents::at(const Coord& voxel) const {
  const std::int64_t cell = findCell(voxel & ~(kDim - 1));
  if (cell < 0) {
    return -1;
  }
  const auto index = static_cast<std::size_t>(cell);
  const openvdb::Index offset = Leaf::coordToOffset(voxel);
  if (!isOn(index, offset)) {
    return -1;
  }
  return component(index, offset);
}

}  // namespace voxelsieve::detail
