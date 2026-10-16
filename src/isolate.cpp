#include "voxelsieve/isolate.hpp"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {
namespace {

using Index3 = std::array<std::int64_t, 3>;

// Bits of a cell's mark.
constexpr std::uint8_t kMaterial = 1;
constexpr std::uint8_t kSeen = 2;
constexpr std::uint8_t kKept = 4;
constexpr std::uint8_t kOutside = 8;
constexpr std::uint8_t kCore = 16;  // material left after the erosion that separates objects

/// The search grid: cells of `size` level-0 voxels per axis.
struct Cells {
  std::int64_t size = 1;
  Index3 dims{};

  [[nodiscard]] std::size_t count() const {
    return static_cast<std::size_t>(dims[0] * dims[1] * dims[2]);
  }
  [[nodiscard]] std::size_t index(std::int64_t x, std::int64_t y, std::int64_t z) const {
    return static_cast<std::size_t>(x + dims[0] * (y + dims[1] * z));
  }
  [[nodiscard]] std::size_t ofVoxel(const openvdb::Coord& c) const {
    return index(c.x() / size, c.y() / size, c.z() / size);
  }
  /// Calls `function(neighbour)` for the cells that share a face with cell `i`.
  template <class Function>
  void forNeighbours(std::size_t i, Function&& function) const {
    const auto n = static_cast<std::int64_t>(i);
    const std::int64_t x = n % dims[0];
    const std::int64_t y = (n / dims[0]) % dims[1];
    const std::int64_t z = n / (dims[0] * dims[1]);
    const std::int64_t row = dims[0];
    const std::int64_t layer = dims[0] * dims[1];
    const std::array<std::pair<bool, std::int64_t>, 6> steps{{{x > 0, -1},
                                                              {x + 1 < dims[0], 1},
                                                              {y > 0, -row},
                                                              {y + 1 < dims[1], row},
                                                              {z > 0, -layer},
                                                              {z + 1 < dims[2], layer}}};
    for (const auto& [valid, step] : steps) {
      if (valid) {
        function(static_cast<std::size_t>(n + step));
      }
    }
  }
  [[nodiscard]] bool onBorder(std::size_t i) const {
    const auto n = static_cast<std::int64_t>(i);
    const std::int64_t x = n % dims[0];
    const std::int64_t y = (n / dims[0]) % dims[1];
    const std::int64_t z = n / (dims[0] * dims[1]);
    return x == 0 || y == 0 || z == 0 || x + 1 == dims[0] || y + 1 == dims[1] || z + 1 == dims[2];
  }
};

/// What isolatePart keeps at most: the box, cut by the planes and the cylinder.
struct Region {
  Box box;
  std::vector<CutPlane> planes;
  std::optional<RegionCylinder> cylinder;

  [[nodiscard]] bool contains(const openvdb::Coord& c) const {
    if (!(c.x() >= box.min[0] && c.x() < box.max[0] && c.y() >= box.min[1] && c.y() < box.max[1] &&
          c.z() >= box.min[2] && c.z() < box.max[2])) {
      return false;
    }
    const std::array<double, 3> p{static_cast<double>(c.x()), static_cast<double>(c.y()),
                                  static_cast<double>(c.z())};
    for (const CutPlane& plane : planes) {
      double side = 0.0;
      for (std::size_t a = 0; a < 3; ++a) {
        side += (p[a] - plane.point[a]) * plane.normal[a];
      }
      if (side > 0.0) {
        return false;
      }
    }
    if (cylinder) {
      const auto axis = static_cast<std::size_t>(cylinder->axis);
      const std::size_t u = axis == 0 ? 1 : 0;
      const std::size_t v = axis == 2 ? 1 : 2;
      const double du = p[u] - cylinder->center[0];
      const double dv = p[v] - cylinder->center[1];
      if (du * du + dv * dv > cylinder->radius * cylinder->radius) {
        return false;
      }
    }
    return true;
  }
};

/// Material voxels per cell, saturating at the largest uint16 value.
std::vector<std::uint16_t> countMaterial(const Dataset& dataset, const Cells& cells,
                                         const Region& region, float threshold) {
  std::vector<std::uint16_t> counts(cells.count(), 0);
  dataset.forEachGrid(0, [&](const Index3& /*brick*/, const auto& grid) {
    for (auto it = grid.cbeginValueOn(); it; ++it) {
      if (!(static_cast<float>(*it) > threshold)) {
        continue;
      }
      openvdb::CoordBBox bbox;
      it.getBoundingBox(bbox);  // one voxel, or every voxel of a tile
      for (auto c = bbox.begin(); c; ++c) {
        if (!region.contains(*c)) {
          continue;
        }
        // Cells can straddle bricks, which run in parallel.
        const std::atomic_ref<std::uint16_t> count(counts[cells.ofVoxel(*c)]);
        std::uint16_t value = count.load(std::memory_order_relaxed);
        while (value < std::numeric_limits<std::uint16_t>::max() &&
               !count.compare_exchange_weak(value, static_cast<std::uint16_t>(value + 1),
                                            std::memory_order_relaxed)) {
        }
      }
    }
  });
  return counts;
}

}  // namespace

IsolateInfo isolatePart(const Dataset& dataset, const std::filesystem::path& dir,
                        const IsolateOptions& options) {
  const DatasetInfo& data = dataset.info();
  if (options.keep_objects < 0 || options.max_cells < 1) {
    throw std::invalid_argument("keep_objects must not be negative and max_cells positive");
  }
  if (options.separate_voxels < 0) {
    throw std::invalid_argument("separate_voxels must not be negative");
  }
  const Region region{options.region.value_or(Box{{0, 0, 0}, data.dims}), options.cut_planes,
                      options.cylinder};
  for (std::size_t a = 0; a < 3; ++a) {
    if (region.box.min[a] < 0 || region.box.max[a] > data.dims[a] ||
        region.box.min[a] >= region.box.max[a]) {
      throw std::invalid_argument("The region must be a non-empty box inside the volume");
    }
  }
  for (const CutPlane& plane : options.cut_planes) {
    if (plane.normal == std::array<double, 3>{0.0, 0.0, 0.0}) {
      throw std::invalid_argument("A cut plane needs a normal");
    }
  }
  if (options.cylinder && (options.cylinder->axis < 0 || options.cylinder->axis > 2 ||
                           !(options.cylinder->radius > 0.0))) {
    throw std::invalid_argument("The cylinder needs an axis 0 to 2 and a positive radius");
  }
  const float threshold = options.threshold.value_or(data.threshold);

  IsolateInfo info;
  Cells cells;
  // Every cell must be addressable with 32-bit indices in the queue.
  const std::int64_t max_cells = std::min<std::int64_t>(options.max_cells, 0xffffffffLL);
  for (;; cells.size *= 2) {
    for (std::size_t a = 0; a < 3; ++a) {
      cells.dims[a] = (data.dims[a] + cells.size - 1) / cells.size;
    }
    if (static_cast<std::int64_t>(cells.count()) <= max_cells) {
      break;
    }
  }
  info.cell_size = cells.size;

  std::vector<std::uint8_t> marks;
  if (options.keep_objects > 0) {
    const TelemetryPhase phase("objects");
    std::vector<std::uint16_t> counts = countMaterial(dataset, cells, region, threshold);
    if (options.progress) {
      options.progress("objects", 0.5);
    }
    marks.assign(cells.count(), 0);
    tbb::parallel_for(std::size_t{0}, marks.size(), [&](std::size_t i) {
      if (counts[i] >= cells.size) {
        marks[i] = kMaterial;
      }
    });

    // Objects: material cells connected across faces.
    struct Object {
      std::int64_t material_voxels = 0;
      std::size_t seed = 0;
    };
    std::vector<Object> objects;
    std::vector<std::uint32_t> queue;
    const auto grow = [&](std::size_t seed, std::uint8_t through, std::uint8_t mark) {
      std::int64_t voxels = 0;
      queue.assign(1, static_cast<std::uint32_t>(seed));
      marks[seed] |= mark;
      while (!queue.empty()) {
        const std::size_t i = queue.back();
        queue.pop_back();
        voxels += counts[i];
        cells.forNeighbours(i, [&](std::size_t n) {
          if ((marks[n] & through) == through && (marks[n] & mark) == 0) {
            marks[n] |= mark;
            queue.push_back(static_cast<std::uint32_t>(n));
          }
        });
      }
      return voxels;
    };
    // With separation, objects are found in the core of the material: what is left after
    // eroding it, so that necks thinner than twice the erosion no longer connect objects.
    const std::int64_t erosion = (options.separate_voxels + cells.size - 1) / cells.size;
    std::vector<std::uint16_t> next(marks.size(), 0);
    for (std::size_t i = 0; i < marks.size(); ++i) {
      marks[i] |= (marks[i] & kMaterial) != 0 ? kCore : 0;
    }
    for (std::int64_t step = 0; step < erosion; ++step) {
      tbb::parallel_for(std::size_t{0}, marks.size(), [&](std::size_t i) {
        bool core = (marks[i] & kCore) != 0;
        cells.forNeighbours(i, [&](std::size_t n) { core = core && (marks[n] & kCore) != 0; });
        next[i] = core ? 1 : 0;
      });
      tbb::parallel_for(std::size_t{0}, marks.size(), [&](std::size_t i) {
        if (next[i] == 0) {
          marks[i] &= static_cast<std::uint8_t>(~kCore);
        }
      });
    }
    for (std::size_t i = 0; i < marks.size(); ++i) {
      if ((marks[i] & (kCore | kSeen)) == kCore) {
        objects.push_back({grow(i, kCore, kSeen), i});
      }
    }
    info.objects = static_cast<std::int64_t>(objects.size());
    std::ranges::sort(objects, std::greater<>(), &Object::material_voxels);
    objects.resize(std::min(objects.size(), static_cast<std::size_t>(options.keep_objects)));
    for (const Object& object : objects) {
      (void)grow(object.seed, kCore, kKept);
      info.kept_material_voxels.push_back(object.material_voxels);
    }
    // The kept cores grow back by the erosion, but only within the material.
    for (std::int64_t step = 0; step < erosion; ++step) {
      tbb::parallel_for(std::size_t{0}, marks.size(), [&](std::size_t i) {
        bool kept = (marks[i] & kKept) != 0;
        if ((marks[i] & kMaterial) != 0) {
          cells.forNeighbours(i, [&](std::size_t n) { kept = kept || (marks[n] & kKept) != 0; });
        }
        next[i] = kept ? 1 : 0;
      });
      tbb::parallel_for(std::size_t{0}, marks.size(), [&](std::size_t i) {
        if (next[i] != 0) {
          marks[i] |= kKept;
        }
      });
    }

    // The dataset's margin of air and a voxel around the kept objects, in whole cells, so that
    // their surface keeps the voxels of partial volume and the air the sieve keeps around a part.
    const std::int64_t steps = (data.margin_voxels + 1 + cells.size - 1) / cells.size;
    for (std::int64_t step = 0; step < steps; ++step) {
      tbb::parallel_for(std::size_t{0}, marks.size(), [&](std::size_t i) {
        bool kept = (marks[i] & kKept) != 0;
        cells.forNeighbours(i, [&](std::size_t n) { kept = kept || (marks[n] & kKept) != 0; });
        next[i] = kept ? 1 : 0;
      });
      tbb::parallel_for(std::size_t{0}, marks.size(), [&](std::size_t i) {
        if (next[i] != 0) {
          marks[i] |= kKept;
        }
      });
    }

    // Outside: what the volume's border reaches without crossing a kept cell. The rest are the
    // kept objects and what they enclose.
    for (std::size_t i = 0; i < marks.size(); ++i) {
      if (cells.onBorder(i) && (marks[i] & (kKept | kOutside)) == 0) {
        marks[i] |= kOutside;
        queue.assign(1, static_cast<std::uint32_t>(i));
        while (!queue.empty()) {
          const std::size_t c = queue.back();
          queue.pop_back();
          cells.forNeighbours(c, [&](std::size_t n) {
            if ((marks[n] & (kKept | kOutside)) == 0) {
              marks[n] |= kOutside;
              queue.push_back(static_cast<std::uint32_t>(n));
            }
          });
        }
      }
    }
    if (options.progress) {
      options.progress("objects", 1.0);
    }
  }

  DerivedDatasetOptions derived;
  derived.progress = options.progress;
  const float removed = std::numeric_limits<float>::quiet_NaN();
  info.dataset = writeDerivedDataset(
      dataset, dir,
      [&](const Index3& /*brick*/, const Box& box, std::vector<float>& values) {
        dataset.readRegion(0, box, values);
        std::size_t i = 0;
        for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
          for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
            for (std::int64_t x = box.min[0]; x < box.max[0]; ++x, ++i) {
              const openvdb::Coord c(static_cast<openvdb::Int32>(x), static_cast<openvdb::Int32>(y),
                                     static_cast<openvdb::Int32>(z));
              if (!region.contains(c) ||
                  (!marks.empty() && (marks[cells.ofVoxel(c)] & kOutside) != 0)) {
                values[i] = removed;
              }
            }
          }
        }
      },
      derived);
  info.removed_voxels = data.active_voxel_count - info.dataset.active_voxel_count;
  return info;
}

}  // namespace voxelsieve
