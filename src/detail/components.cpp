#include "detail/components.hpp"

#include <tbb/parallel_for.h>

#include <array>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace voxelsieve::detail {
namespace {

using Coord = openvdb::Coord;
using Leaf = VoxelComponents::Leaf;
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

/// Index of the neighbour leaf in direction (-1..1 per axis), 13 for the leaf itself.
int directionIndex(int dx, int dy, int dz) { return (dx + 1) + 3 * ((dy + 1) + 3 * (dz + 1)); }

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
    tbb::parallel_for(std::size_t{0}, size,
                      [&](std::size_t i) { parent_[i].store(i, std::memory_order_relaxed); });
  }

  std::size_t find(std::size_t x) {
    while (true) {
      std::size_t parent = parent_[x].load();
      if (parent == x) {
        return x;
      }
      const std::size_t grandparent = parent_[parent].load();
      // Path halving; losing the race to another thread only skips the shortcut.
      parent_[x].compare_exchange_weak(parent, grandparent);
      x = grandparent;
    }
  }

  void unite(std::size_t a, std::size_t b) {
    while (true) {
      a = find(a);
      b = find(b);
      if (a == b) {
        return;
      }
      if (a < b) {
        std::swap(a, b);
      }
      std::size_t expected = a;
      if (parent_[a].compare_exchange_strong(expected, b)) {
        return;
      }
    }
  }

 private:
  std::vector<std::atomic<std::size_t>> parent_;
};

}  // namespace

VoxelComponents::VoxelComponents(const openvdb::BoolTree& mask) : tree_(mask) {
  tree_.voxelizeActiveTiles();
  for (auto it = tree_.cbeginLeaf(); it; ++it) {
    if (!it->isEmpty()) {
      leaves_.push_back(it.getLeaf());
    }
  }
  const std::size_t n = leaves_.size();
  for (std::size_t i = 0; i < n; ++i) {
    leaf_index_.emplace(leaves_[i]->origin(), i);
  }

  // Pieces of each leaf on its own.
  local_.resize(n * Leaf::SIZE);
  std::vector<std::size_t> pieces(n);
  tbb::parallel_for(std::size_t{0}, n, [&](std::size_t i) {
    pieces[i] = labelLeaf(*leaves_[i], &local_[i * Leaf::SIZE]);
  });
  piece_begin_.resize(n + 1);
  for (std::size_t i = 0; i < n; ++i) {
    piece_begin_[i + 1] = piece_begin_[i] + pieces[i];
  }
  const std::size_t total = piece_begin_[n];

  // Join the pieces that touch across leaf boundaries. Each pair of voxels is visited from both
  // sides; joining twice does no harm and keeps the loop simple.
  ConcurrentUnionFind sets(total);
  tbb::parallel_for(std::size_t{0}, n, [&](std::size_t i) {
    const Leaf& leaf = *leaves_[i];
    const Coord origin = leaf.origin();
    std::array<std::int64_t, 27> neighbour{};
    neighbour.fill(-1);
    bool any = false;
    for (int dz = -1; dz <= 1; ++dz) {
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0 && dz == 0) {
            continue;
          }
          const auto found = leaf_index_.find(origin.offsetBy(dx * kDim, dy * kDim, dz * kDim));
          if (found != leaf_index_.end()) {
            neighbour[static_cast<std::size_t>(directionIndex(dx, dy, dz))] =
                static_cast<std::int64_t>(found->second);
            any = true;
          }
        }
      }
    }
    if (!any) {
      return;
    }
    for (auto it = leaf.cbeginValueOn(); it; ++it) {
      const Coord c = Leaf::offsetToLocalCoord(it.pos());
      if (c.x() > 0 && c.y() > 0 && c.z() > 0 && c.x() < kDim - 1 && c.y() < kDim - 1 &&
          c.z() < kDim - 1) {
        continue;
      }
      const std::size_t own = piece_begin_[i] + local_[i * Leaf::SIZE + it.pos()];
      for (const Coord& d : neighbours26()) {
        const Coord p = c + d;
        const auto side = [](int v) { return v < 0 ? -1 : (v >= kDim ? 1 : 0); };
        const int dx = side(p.x());
        const int dy = side(p.y());
        const int dz = side(p.z());
        if (dx == 0 && dy == 0 && dz == 0) {
          continue;
        }
        const std::int64_t j = neighbour[static_cast<std::size_t>(directionIndex(dx, dy, dz))];
        if (j < 0) {
          continue;
        }
        const auto other_leaf = static_cast<std::size_t>(j);
        const openvdb::Index offset = Leaf::coordToOffset(p);  // wraps into the neighbour leaf
        if (leaves_[other_leaf]->isValueOn(offset)) {
          sets.unite(own, piece_begin_[other_leaf] + local_[other_leaf * Leaf::SIZE + offset]);
        }
      }
    }
  });

  // Number the sets by their smallest piece, which holds their first voxel in tree order.
  std::vector<std::size_t> roots(total);
  tbb::parallel_for(std::size_t{0}, total, [&](std::size_t k) { roots[k] = sets.find(k); });
  std::vector<std::size_t> number(total);
  for (std::size_t k = 0; k < total; ++k) {
    if (roots[k] == k) {
      number[k] = count_++;
    }
  }
  labels_.resize(total);
  tbb::parallel_for(std::size_t{0}, total, [&](std::size_t k) { labels_[k] = number[roots[k]]; });
}

std::int64_t VoxelComponents::at(const Coord& voxel) const {
  const auto found = leaf_index_.find(voxel & ~(kDim - 1));
  if (found == leaf_index_.end()) {
    return -1;
  }
  const openvdb::Index offset = Leaf::coordToOffset(voxel);
  if (!leaves_[found->second]->isValueOn(offset)) {
    return -1;
  }
  return static_cast<std::int64_t>(component(found->second, offset));
}

}  // namespace voxelsieve::detail
