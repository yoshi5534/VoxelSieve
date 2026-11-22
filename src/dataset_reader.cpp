#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <future>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "detail/memory.hpp"
#include "detail/sweep.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/resources.hpp"

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

struct BrickCache::Impl {
  /// Dataset, level, brick index.
  using Key = std::array<std::uint64_t, 5>;
  struct Entry {
    Key key;
    Dataset::BrickPtr grid;
    std::size_t bytes = 0;
  };

  std::size_t budget = 0;
  /// Whether this is the shared cache, whose budget is counted while it is closed as well.
  bool shared = false;
  std::mutex mutex;
  std::list<Entry> lru;  // most recently used first
  std::map<Key, std::list<Entry>::iterator> entries;
  /// Bricks a thread is loading; other threads that need one wait for it.
  std::map<Key, std::shared_future<Dataset::BrickPtr>> in_flight;
  CacheStats stats;  // bricks and bytes; hits and misses are counted per dataset
  std::atomic<std::uint64_t> next_dataset{1};

  /// Moves the least recently used bricks over the budget to `evicted`; the caller holds the lock.
  /// Pinned bricks, which a caller still holds (ADR 0034), stay: dropping them would free no
  /// memory, and a caller asking for them again would load a second copy. The most recently used
  /// brick stays as well.
  void evict(std::list<Entry>& evicted) {
    auto it = lru.end();
    while (stats.bytes > budget && it != lru.begin() && std::prev(it) != lru.begin()) {
      --it;
      if (it->grid.use_count() > 1) {
        continue;
      }
      stats.bytes -= it->bytes;
      entries.erase(it->key);
      const auto victim = it++;
      evicted.splice(evicted.end(), lru, victim);
    }
    stats.bricks = lru.size();
  }
};

namespace {

std::mutex g_shared_cache_mutex;
std::weak_ptr<BrickCache> g_shared_cache;  // guarded by g_shared_cache_mutex
std::size_t g_shared_cache_bytes = 0;      // 0: the default; guarded by g_shared_cache_mutex

/// The budgets of all caches that exist, and whether the shared cache is one of them.
std::atomic<std::uint64_t> g_cache_budgets{0};
std::atomic<int> g_shared_cache_open{0};

/// Bytes of evicted bricks freed since the freed memory last went back to the system.
std::atomic<std::size_t> g_freed_brick_bytes{0};
constexpr std::size_t kMinReleaseBytes = std::size_t{64} << 20U;

std::size_t sharedCacheBudget(std::size_t set) {
  return set > 0 ? set : Dataset::defaultCacheBytes();
}

}  // namespace

namespace detail {

std::uint64_t cacheBudgetBytes() {
  std::uint64_t bytes = g_cache_budgets.load();
  if (g_shared_cache_open.load() == 0) {
    const std::scoped_lock lock(g_shared_cache_mutex);
    bytes += sharedCacheBudget(g_shared_cache_bytes);
  }
  return bytes;
}

void sharedCacheFollowsBudget() {
  std::shared_ptr<BrickCache> cache;
  std::size_t bytes = 0;
  {
    const std::scoped_lock lock(g_shared_cache_mutex);
    cache = g_shared_cache.lock();
    bytes = sharedCacheBudget(g_shared_cache_bytes);
  }
  if (cache) {
    cache->setBudget(bytes);
  }
}

}  // namespace detail

BrickCache::BrickCache(std::size_t budget_bytes) : impl_(std::make_unique<Impl>()) {
  impl_->budget = budget_bytes;
  g_cache_budgets += budget_bytes;
}

BrickCache::~BrickCache() {
  g_cache_budgets -= impl_->budget;
  if (impl_->shared) {
    --g_shared_cache_open;
  }
}

std::size_t BrickCache::budget() const {
  const std::scoped_lock lock(impl_->mutex);
  return impl_->budget;
}

void BrickCache::setBudget(std::size_t budget_bytes) {
  std::list<Impl::Entry> evicted;  // freed after the lock is released, see Dataset::Impl::brick
  const std::scoped_lock lock(impl_->mutex);
  g_cache_budgets += budget_bytes;
  g_cache_budgets -= impl_->budget;
  impl_->budget = budget_bytes;
  impl_->evict(evicted);
}

CacheStats BrickCache::stats() const {
  const std::scoped_lock lock(impl_->mutex);
  CacheStats stats = impl_->stats;
  for (const Impl::Entry& entry : impl_->lru) {
    if (entry.grid.use_count() > 1) {
      ++stats.pinned;
      stats.pinned_bytes += entry.bytes;
    }
  }
  return stats;
}

struct Dataset::Impl {
  using Key = BrickCache::Impl::Key;
  using Entry = BrickCache::Impl::Entry;

  std::filesystem::path dir;
  DatasetInfo info;
  std::vector<std::set<Index3>> stored;  // per level
  BrickLoading loading = BrickLoading::kFull;
  std::shared_ptr<BrickCache> shared_cache;
  BrickCache::Impl* cache = nullptr;
  std::uint64_t id = 0;  // this dataset's part of the cache keys
  mutable std::atomic<std::uint64_t> hits{0};
  mutable std::atomic<std::uint64_t> misses{0};

  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  /// Takes this dataset's bricks out of a shared cache; they are freed after the lock is released.
  ~Impl() {
    if (cache == nullptr) {
      return;
    }
    std::list<Entry> removed;
    const std::scoped_lock lock(cache->mutex);
    for (auto it = cache->lru.begin(); it != cache->lru.end();) {
      const auto next = std::next(it);
      if (it->key[0] == id) {
        cache->stats.bytes -= it->bytes;
        cache->entries.erase(it->key);
        removed.splice(removed.end(), cache->lru, it);
      }
      it = next;
    }
    cache->stats.bricks = cache->lru.size();
  }

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
    const Key key{id, static_cast<std::uint64_t>(level_index), static_cast<std::uint64_t>(brick[0]),
                  static_cast<std::uint64_t>(brick[1]), static_cast<std::uint64_t>(brick[2])};
    std::promise<BrickPtr> loaded;
    std::shared_future<BrickPtr> pending;
    {
      const std::scoped_lock lock(cache->mutex);
      if (const auto it = cache->entries.find(key); it != cache->entries.end()) {
        cache->lru.splice(cache->lru.begin(), cache->lru, it->second);
        ++hits;
        return it->second->grid;
      }
      if (const auto it = cache->in_flight.find(key); it != cache->in_flight.end()) {
        pending = it->second;
        ++hits;
      } else {
        cache->in_flight.emplace(key, loaded.get_future().share());
        ++misses;
      }
    }
    // Another thread loads this brick: wait for it instead of decoding it a second time. The
    // loader runs isolated (below), so it never waits for a task of this thread.
    if (pending.valid()) {
      return pending.get();
    }

    // Load outside the lock so other threads keep using the cache. Evicted bricks are destroyed
    // only after the waiting threads have their brick.
    std::list<Entry> evicted;
    BrickPtr grid;
    try {
      tbb::this_task_arena::isolate([&] { grid = load(key, level_index, brick, evicted); });
    } catch (...) {
      {
        const std::scoped_lock lock(cache->mutex);
        cache->in_flight.erase(key);
      }
      loaded.set_exception(std::current_exception());
      throw;
    }
    loaded.set_value(grid);
    releaseEvicted(evicted);
    return grid;
  }

  /// Frees evicted bricks. Their leaves are thousands of small blocks, freed into the heap of
  /// whichever thread loaded them, while other threads load new bricks into theirs; each heap
  /// would grow towards the size of the cache (ADR 0034). So whenever bricks of a quarter of the
  /// cache budget have been freed, the freed memory goes back to the system.
  void releaseEvicted(std::list<Entry>& evicted) const {
    if (evicted.empty()) {
      return;
    }
    std::size_t bytes = 0;
    for (const Entry& entry : evicted) {
      bytes += entry.bytes;
    }
    std::size_t budget = 0;
    {
      const std::scoped_lock lock(cache->mutex);
      budget = cache->budget;
    }
    evicted.clear();
    const std::size_t freed = g_freed_brick_bytes += bytes;
    if (freed >= std::max(budget / 4, kMinReleaseBytes)) {
      g_freed_brick_bytes = 0;
      releaseFreedMemory();
    }
  }

  /// Reads a brick and puts it into the cache, under the budget; bricks pushed out go to
  /// `evicted`, to be destroyed after the lock is released: OpenVDB frees a tree with TBB tasks,
  /// and a thread waiting for them may pick up another brick() call on this cache.
  BrickPtr load(const Key& key, int level_index, const Index3& brick,
                std::list<Entry>& evicted) const {
    const bool on_access = loading == BrickLoading::kOnAccess;
    BrickPtr grid =
        std::make_shared<const GreyGrid>(readBrick(brickPath(dir, level_index, brick), on_access));
    // A brick loaded on access grows as its leaves are read; it is charged its full size at once
    // (ADR 0034), so the budget holds without measuring the cached bricks again.
    std::size_t bytes = 0;
    grid->visit([&](const auto& typed) {
      bytes = static_cast<std::size_t>(typed.memUsage());
      if (on_access) {
        using Value = typename std::decay_t<decltype(typed)>::ValueType;
        bytes += static_cast<std::size_t>(typed.tree().leafCount()) * 512 * sizeof(Value);
      }
    });

    const std::scoped_lock lock(cache->mutex);
    CacheStats& stats = cache->stats;
    cache->in_flight.erase(key);
    cache->lru.push_front({key, grid, bytes});
    cache->entries.emplace(key, cache->lru.begin());
    stats.bytes += bytes;
    cache->evict(evicted);
    return grid;
  }

  /// readRegion for each output type: brick by brick in parallel, and in each brick leaf by leaf.
  template <class Out>
  void readRegion(int level_index, const Box& box, std::span<Out> out, Out fill) const {
    const LevelInfo& info_level = level(level_index);
    for (std::size_t i = 0; i < 3; ++i) {
      if (box.min[i] < 0 || box.max[i] > info_level.dims[i] || box.min[i] > box.max[i]) {
        throw std::out_of_range("Region outside dataset level " + std::to_string(level_index));
      }
    }
    if (out.size() != static_cast<std::size_t>(box.voxelCount())) {
      throw std::invalid_argument("Output size does not match the region");
    }
    if constexpr (std::is_same_v<Out, std::uint16_t>) {
      if (info.value_type == ValueType::kFloat) {
        throw std::invalid_argument("A float dataset cannot be read into 16-bit values");
      }
    } else if constexpr (std::is_same_v<Out, std::uint8_t>) {
      if (info.value_type != ValueType::kUInt8) {
        throw std::invalid_argument("Only an 8-bit dataset can be read into 8-bit values");
      }
    }
    std::fill(out.begin(), out.end(), fill);
    if (box.voxelCount() == 0) {
      return;
    }

    const std::int64_t size = info.brick_size;
    Index3 first{};
    Index3 last{};
    for (std::size_t i = 0; i < 3; ++i) {
      first[i] = box.min[i] / size;
      last[i] = (box.max[i] - 1) / size;
    }
    std::vector<Index3> bricks;
    for (std::int64_t bz = first[2]; bz <= last[2]; ++bz) {
      for (std::int64_t by = first[1]; by <= last[1]; ++by) {
        for (std::int64_t bx = first[0]; bx <= last[0]; ++bx) {
          if (hasBrick(level_index, {bx, by, bz})) {
            bricks.push_back({bx, by, bz});
          }
        }
      }
    }
    // The bricks fill disjoint parts of `out`, so a region of many bricks is read in parallel.
    // Isolated: a caller may hold a lock, and while it waits here it must not pick up other work
    // that takes the same lock.
    tbb::this_task_arena::isolate([&] {
      tbb::parallel_for(std::size_t{0}, bricks.size(), [&](std::size_t b) {
        const BrickPtr grid = brick(level_index, bricks[b]);
        const Box brick_box = brickBox(level_index, bricks[b]);
        Box part;
        for (std::size_t i = 0; i < 3; ++i) {
          part.min[i] = std::max(box.min[i], brick_box.min[i]);
          part.max[i] = std::min(box.max[i], brick_box.max[i]);
        }
        grid->visit([&](const auto& typed) {
          using Value = typename std::decay_t<decltype(typed)>::ValueType;
          // Only the combinations checked above: a wider value never goes into a narrower type.
          if constexpr (std::is_same_v<Out, float> || sizeof(Value) <= sizeof(Out)) {
            copyBrick<Out>(typed.tree(), box, part, out);
          }
        });
      });
    });
  }

  /// Copies the active voxels of `part` from `tree` into `out` (the region `box`), one 8^3 leaf
  /// (or tile of that size) after the other.
  template <class Out, class Tree>
  static void copyBrick(const Tree& tree, const Box& box, const Box& part, std::span<Out> out) {
    using Value = typename Tree::ValueType;
    const auto index = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
      return static_cast<std::size_t>(
          (x - box.min[0]) + box.size(0) * ((y - box.min[1]) + box.size(1) * (z - box.min[2])));
    };
    constexpr std::int64_t kLeaf = 8;
    const auto floor8 = [](std::int64_t v) { return v & ~std::int64_t{kLeaf - 1}; };
    for (std::int64_t z0 = floor8(part.min[2]); z0 < part.max[2]; z0 += kLeaf) {
      const std::int64_t z_begin = std::max(z0, part.min[2]);
      const std::int64_t z_end = std::min(z0 + kLeaf, part.max[2]);
      for (std::int64_t y0 = floor8(part.min[1]); y0 < part.max[1]; y0 += kLeaf) {
        const std::int64_t y_begin = std::max(y0, part.min[1]);
        const std::int64_t y_end = std::min(y0 + kLeaf, part.max[1]);
        for (std::int64_t x0 = floor8(part.min[0]); x0 < part.max[0]; x0 += kLeaf) {
          const std::int64_t x_begin = std::max(x0, part.min[0]);
          const std::int64_t x_end = std::min(x0 + kLeaf, part.max[0]);
          const openvdb::Coord origin = toCoord({x0, y0, z0});
          if (const auto* leaf = tree.probeConstLeaf(origin)) {
            const auto& mask = leaf->getValueMask();
            const Value* data = leaf->buffer().data();
            for (std::int64_t z = z_begin; z < z_end; ++z) {
              for (std::int64_t y = y_begin; y < y_end; ++y) {
                std::size_t at = index(x_begin, y, z);
                for (std::int64_t x = x_begin; x < x_end; ++x, ++at) {
                  // Leaf voxels are stored x slowest: offset = 64 x + 8 y + z.
                  const auto offset =
                      static_cast<openvdb::Index>(((x & 7) << 6) | ((y & 7) << 3) | (z & 7));
                  if (mask.isOn(offset)) {
                    out[at] = static_cast<Out>(data[offset]);
                  }
                }
              }
            }
            continue;
          }
          Value value{};
          if (!tree.probeValue(origin, value)) {
            continue;  // inactive tile or background
          }
          for (std::int64_t z = z_begin; z < z_end; ++z) {
            for (std::int64_t y = y_begin; y < y_end; ++y) {
              const std::size_t at = index(x_begin, y, z);
              std::fill_n(out.begin() + static_cast<std::ptrdiff_t>(at), x_end - x_begin,
                          static_cast<Out>(value));
            }
          }
        }
      }
    }
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

std::size_t Dataset::defaultCacheBytes() {
  constexpr std::uint64_t kGiB = std::uint64_t{1} << 30U;
  const std::uint64_t budget = memoryBudgetBytes();
  return static_cast<std::size_t>(std::max(budget / 3, std::min(2 * kGiB, budget / 2)));
}

std::shared_ptr<BrickCache> Dataset::sharedCache() {
  const std::scoped_lock lock(g_shared_cache_mutex);
  auto cache = g_shared_cache.lock();
  if (!cache) {
    cache = std::make_shared<BrickCache>(sharedCacheBudget(g_shared_cache_bytes));
    cache->impl_->shared = true;
    ++g_shared_cache_open;
    g_shared_cache = cache;
  }
  return cache;
}

void Dataset::setSharedCacheBytes(std::size_t bytes) {
  std::shared_ptr<BrickCache> cache;
  {
    const std::scoped_lock lock(g_shared_cache_mutex);
    g_shared_cache_bytes = bytes;
    cache = g_shared_cache.lock();
  }
  if (cache) {
    cache->setBudget(sharedCacheBudget(bytes));
  }
}

Dataset Dataset::open(const std::filesystem::path& dir, BrickLoading loading) {
  return open(dir, sharedCache(), loading);
}

Dataset Dataset::open(const std::filesystem::path& dir, std::size_t cache_bytes,
                      BrickLoading loading) {
  return open(dir, std::make_shared<BrickCache>(cache_bytes), loading);
}

Dataset Dataset::open(const std::filesystem::path& dir, std::shared_ptr<BrickCache> cache,
                      BrickLoading loading) {
  if (!cache) {
    throw std::invalid_argument("Dataset::open needs a brick cache");
  }
  initializeVdb();
  auto impl = std::make_unique<Impl>();
  impl->dir = dir;
  impl->info = readDatasetInfo(dir);
  impl->loading = loading;
  for (const LevelInfo& level : impl->info.levels) {
    impl->stored.emplace_back(level.bricks.begin(), level.bricks.end());
  }
  impl->cache = cache->impl_.get();
  impl->id = impl->cache->next_dataset++;
  impl->shared_cache = std::move(cache);
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
  if (!grid->probeValue(toCoord(voxel), value)) {
    return std::nullopt;
  }
  return value;
}

Dataset::Sampler::Sampler(const Dataset& dataset, int level)
    : dataset_(&dataset),
      level_(level),
      dims_(dataset.level(level).dims),
      brick_size_(dataset.info().brick_size) {}

std::optional<float> Dataset::Sampler::operator()(const Index3& voxel) {
  for (std::size_t i = 0; i < 3; ++i) {
    if (voxel[i] < 0 || voxel[i] >= dims_[i]) {
      return std::nullopt;
    }
  }
  const Index3 index{voxel[0] / brick_size_, voxel[1] / brick_size_, voxel[2] / brick_size_};
  if (index != current_) {
    current_ = index;
    accessor_ = std::monostate{};  // before the brick it points into is released
    brick_ = dataset_->brick(level_, index);
    if (brick_) {
      brick_->visit([&](const auto& grid) { accessor_ = grid.getConstAccessor(); });
    }
  }
  return std::visit(
      [&](const auto& accessor) -> std::optional<float> {
        if constexpr (std::is_same_v<std::decay_t<decltype(accessor)>, std::monostate>) {
          return std::nullopt;
        } else {
          typename std::decay_t<decltype(accessor)>::ValueType value{};
          if (accessor.probeValue(toCoord(voxel), value)) {
            return static_cast<float>(value);
          }
          return std::nullopt;
        }
      },
      accessor_);
}

void Dataset::readRegion(int level, const Box& box, std::span<float> out, float fill) const {
  impl_->readRegion(level, box, out, fill);
}

void Dataset::readRegion(int level, const Box& box, std::span<std::uint16_t> out,
                         std::uint16_t fill) const {
  impl_->readRegion(level, box, out, fill);
}

void Dataset::readRegion(int level, const Box& box, std::span<std::uint8_t> out,
                         std::uint8_t fill) const {
  impl_->readRegion(level, box, out, fill);
}

void Dataset::forEachBrick(int level, const BrickFunction& function) const {
  forEachBrick(level, function, BrickSweep{});
}

void Dataset::forEachBrick(int level, const BrickFunction& function,
                           const BrickSweep& sweep) const {
  std::vector<Index3> bricks = impl_->level(level).bricks;
  detail::mortonSort(bricks);
  const std::size_t threads = static_cast<std::size_t>(
      std::max(1, std::min(tbb::this_task_arena::max_concurrency(), threadLimit())));
  const std::size_t in_flight = sweep.task_bytes > 0 ? tasksInMemory(sweep.task_bytes) : threads;
  const std::int64_t size = impl_->info.brick_size;
  const std::int64_t reach = sweep.halo > 0 ? (sweep.halo + size - 1) / size : 0;
  detail::runInFlight(
      bricks.size(), in_flight,
      [&](std::size_t i) {
        const Index3& index = bricks[i];
        const BrickPtr grid = impl_->brick(level, index);
        // The neighbours the halo reaches, pinned for this call.
        std::vector<BrickPtr> neighbours;
        for (std::int64_t dz = -reach; dz <= reach; ++dz) {
          for (std::int64_t dy = -reach; dy <= reach; ++dy) {
            for (std::int64_t dx = -reach; dx <= reach; ++dx) {
              if (dx != 0 || dy != 0 || dz != 0) {
                if (BrickPtr neighbour =
                        impl_->brick(level, {index[0] + dx, index[1] + dy, index[2] + dz})) {
                  neighbours.push_back(std::move(neighbour));
                }
              }
            }
          }
        }
        function(index, *grid);
      },
      sweep.progress);
}
CacheStats Dataset::cacheStats() const {
  CacheStats stats = impl_->shared_cache->stats();
  stats.hits = impl_->hits;
  stats.misses = impl_->misses;
  return stats;
}

const std::shared_ptr<BrickCache>& Dataset::cache() const { return impl_->shared_cache; }

}  // namespace voxelsieve
