#include <tbb/parallel_for.h>

#include <algorithm>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "voxelsieve/dataset.hpp"

namespace voxelsieve {
namespace {

using Index3 = std::array<std::int64_t, 3>;
/// Level followed by the brick index.
using BrickKey = std::array<std::int64_t, 4>;

openvdb::Coord toCoord(const Index3& index) {
  return {static_cast<openvdb::Int32>(index[0]), static_cast<openvdb::Int32>(index[1]),
          static_cast<openvdb::Int32>(index[2])};
}

bool insideLevel(const LevelInfo& level, const Index3& voxel) {
  for (std::size_t i = 0; i < 3; ++i) {
    if (voxel[i] < 0 || voxel[i] >= level.dims[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace

struct Dataset::Impl {
  struct Entry {
    BrickKey key;
    BrickPtr grid;
    std::size_t bytes = 0;
  };

  std::filesystem::path dir;
  DatasetInfo info;
  std::vector<std::set<Index3>> stored;  // per level
  std::size_t cache_bytes = 0;

  mutable std::mutex mutex;
  mutable std::list<Entry> lru;  // most recently used first
  mutable std::map<BrickKey, std::list<Entry>::iterator> entries;
  mutable CacheStats stats;

  const LevelInfo& level(int level) const {
    if (level < 0 || static_cast<std::size_t>(level) >= info.levels.size()) {
      throw std::out_of_range("Dataset has no level " + std::to_string(level));
    }
    return info.levels[static_cast<std::size_t>(level)];
  }

  bool hasBrick(int level_index, const Index3& brick) const {
    (void)level(level_index);
    return stored[static_cast<std::size_t>(level_index)].contains(brick);
  }

  BrickPtr brick(int level_index, const Index3& brick) const {
    if (!hasBrick(level_index, brick)) {
      return nullptr;
    }
    const BrickKey key{level_index, brick[0], brick[1], brick[2]};
    {
      const std::scoped_lock lock(mutex);
      if (const auto it = entries.find(key); it != entries.end()) {
        lru.splice(lru.begin(), lru, it->second);
        ++stats.hits;
        return it->second->grid;
      }
      ++stats.misses;
    }

    // Load outside the lock so other threads keep using the cache. Fully loaded, so the memory
    // accounted for does not grow later through delayed loading.
    BrickPtr grid = readBrick(brickPath(dir, level_index, brick), /*delay_load=*/false);
    const auto bytes = static_cast<std::size_t>(grid->memUsage());

    const std::scoped_lock lock(mutex);
    if (const auto it = entries.find(key); it != entries.end()) {
      return it->second->grid;  // another thread loaded it meanwhile
    }
    lru.push_front({key, grid, bytes});
    entries.emplace(key, lru.begin());
    stats.bytes += bytes;
    while (stats.bytes > cache_bytes && lru.size() > 1) {
      stats.bytes -= lru.back().bytes;
      entries.erase(lru.back().key);
      lru.pop_back();
    }
    stats.bricks = lru.size();
    return grid;
  }

  Box brickBox(int level_index, const Index3& brick) const {
    const LevelInfo& info_level = level(level_index);
    Box box;
    for (std::size_t i = 0; i < 3; ++i) {
      box.min[i] = brick[i] * info.brick_size;
      box.max[i] = std::min(box.min[i] + info.brick_size, info_level.dims[i]);
    }
    return box;
  }
};

Dataset::Dataset(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Dataset::Dataset(Dataset&&) noexcept = default;
Dataset& Dataset::operator=(Dataset&&) noexcept = default;
Dataset::~Dataset() = default;

Dataset Dataset::open(const std::filesystem::path& dir, std::size_t cache_bytes) {
  openvdb::initialize();
  auto impl = std::make_unique<Impl>();
  impl->dir = dir;
  impl->info = readDatasetInfo(dir);
  impl->cache_bytes = cache_bytes;
  for (const LevelInfo& level : impl->info.levels) {
    impl->stored.emplace_back(level.bricks.begin(), level.bricks.end());
  }
  return Dataset(std::move(impl));
}

const DatasetInfo& Dataset::info() const { return impl_->info; }

const std::filesystem::path& Dataset::dir() const { return impl_->dir; }

const LevelInfo& Dataset::level(int level) const { return impl_->level(level); }

Box Dataset::brickBox(int level, const Index3& brick) const {
  return impl_->brickBox(level, brick);
}

bool Dataset::hasBrick(int level, const Index3& brick) const {
  return impl_->hasBrick(level, brick);
}

Dataset::BrickPtr Dataset::brick(int level, const Index3& brick) const {
  return impl_->brick(level, brick);
}

std::optional<float> Dataset::sample(int level, const Index3& voxel) const {
  if (!insideLevel(impl_->level(level), voxel)) {
    return std::nullopt;
  }
  const std::int64_t size = impl_->info.brick_size;
  const BrickPtr grid = impl_->brick(level, {voxel[0] / size, voxel[1] / size, voxel[2] / size});
  if (!grid) {
    return std::nullopt;
  }
  float value = 0.0F;
  if (!grid->tree().probeValue(toCoord(voxel), value)) {
    return std::nullopt;
  }
  return value;
}

void Dataset::readRegion(int level, const Box& box, std::span<float> out, float fill) const {
  const LevelInfo& info_level = impl_->level(level);
  for (std::size_t i = 0; i < 3; ++i) {
    if (box.min[i] < 0 || box.max[i] > info_level.dims[i] || box.min[i] > box.max[i]) {
      throw std::out_of_range("Region outside dataset level " + std::to_string(level));
    }
  }
  if (out.size() != static_cast<std::size_t>(box.voxelCount())) {
    throw std::invalid_argument("Output size does not match the region");
  }
  std::fill(out.begin(), out.end(), fill);
  if (box.voxelCount() == 0) {
    return;
  }

  const std::int64_t size = impl_->info.brick_size;
  Index3 first{};
  Index3 last{};
  for (std::size_t i = 0; i < 3; ++i) {
    first[i] = box.min[i] / size;
    last[i] = (box.max[i] - 1) / size;
  }
  for (std::int64_t bz = first[2]; bz <= last[2]; ++bz) {
    for (std::int64_t by = first[1]; by <= last[1]; ++by) {
      for (std::int64_t bx = first[0]; bx <= last[0]; ++bx) {
        const BrickPtr grid = impl_->brick(level, {bx, by, bz});
        if (!grid) {
          continue;
        }
        const Box brick_box = impl_->brickBox(level, {bx, by, bz});
        Box part;
        for (std::size_t i = 0; i < 3; ++i) {
          part.min[i] = std::max(box.min[i], brick_box.min[i]);
          part.max[i] = std::min(box.max[i], brick_box.max[i]);
        }
        const auto accessor = grid->getConstAccessor();
        float value = 0.0F;
        for (std::int64_t z = part.min[2]; z < part.max[2]; ++z) {
          for (std::int64_t y = part.min[1]; y < part.max[1]; ++y) {
            auto index = static_cast<std::size_t>(
                (part.min[0] - box.min[0]) +
                box.size(0) * ((y - box.min[1]) + box.size(1) * (z - box.min[2])));
            for (std::int64_t x = part.min[0]; x < part.max[0]; ++x, ++index) {
              if (accessor.probeValue(toCoord({x, y, z}), value)) {
                out[index] = value;
              }
            }
          }
        }
      }
    }
  }
}

void Dataset::forEachBrick(int level, const BrickFunction& function) const {
  const std::vector<Index3>& bricks = impl_->level(level).bricks;
  tbb::parallel_for(std::size_t{0}, bricks.size(), [&](std::size_t i) {
    const BrickPtr grid = impl_->brick(level, bricks[i]);
    function(bricks[i], *grid);
  });
}

CacheStats Dataset::cacheStats() const {
  const std::scoped_lock lock(impl_->mutex);
  return impl_->stats;
}

}  // namespace voxelsieve
