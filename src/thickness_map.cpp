// Thickness map: the local thickness of every material voxel (ADR 0030).
//
// The local thickness of a voxel is the diameter of the largest sphere inside the material that
// contains it (Hildebrand and Rüegsegger 1997). Per tile, with a halo of twice the largest radius:
// 1. The Euclidean distance of every material voxel to the nearest voxel that is not material,
//    in mm with the pitch per axis, by the separable transform of Felzenszwalb and Huttenlocher.
// 2. The centres of the spheres that matter: voxels whose sphere no neighbour's sphere contains
//    (the distance ridge). Every other sphere lies inside one of theirs. A ridge that rises
//    steeply leads into a convex edge, where only small spheres fit by definition; its spheres
//    take the thickness at the top of the rise, so that an edge shows the wall it belongs to.
// 3. Every ridge sphere is drawn into the tile, each voxel keeps the largest diameter. Voxels at
//    the maximum radius or beyond would make a thick ridge; they are drawn at once by a second
//    distance transform from all of them.
// The diameter is 2 d - p/2 for a distance d between voxel centres and the finest pitch p, which
// is exact for walls of an odd number of voxels and half a voxel off for the others.

#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "detail/png.hpp"
#include "detail/thickness.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/telemetry.hpp"
#include "voxelsieve/thickness.hpp"

namespace voxelsieve {
namespace {

using Index3 = std::array<std::int64_t, 3>;
using Json = nlohmann::json;

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr int kFineBins = 4096;
constexpr int kHistogramBins = 40;
/// Largest tile including its halo, in voxels (two float arrays of it are held at a time).
constexpr std::int64_t kMaxRegionVoxels = std::int64_t{1} << 26;
/// Largest number of blocks the regions are found on.
constexpr std::int64_t kMaxBlocks = std::int64_t{1} << 24;
/// A ridge that rises at least this much per mm leads into a convex edge whose faces meet at 60
/// degrees or more (sin 30°), rather than lying between opposite faces of a wall.
constexpr float kEdgeSlope = 0.5F;
/// A sphere whose neighbour's distance rises at least this much per mm lies inside that
/// neighbour's sphere but for a shell of a tenth of their spacing: it is left out. Exact
/// containment (1) keeps the ragged ridge of oblique and noisy faces, which costs several times as
/// much to draw.
constexpr float kContainSlope = 0.9F;
/// Largest side of the slice pictures, in pixels.
constexpr std::int64_t kSlicePixels = 2048;

/// Squared Euclidean distance along one line to the nearest site, a sample with a finite value,
/// plus that value: the lower envelope of parabolas (Felzenszwalb and Huttenlocher 2012). Samples
/// are `pitch` apart. A line without sites stays infinite.
class LineTransform {
 public:
  void run(float* data, std::int64_t n, std::int64_t stride, double pitch) {
    const auto count = static_cast<std::size_t>(n);
    f_.resize(count);
    v_.resize(count);
    z_.resize(count + 1);
    for (std::int64_t i = 0; i < n; ++i) {
      f_[static_cast<std::size_t>(i)] = data[i * stride];
    }
    std::int64_t k = -1;
    for (std::int64_t q = 0; q < n; ++q) {
      const double fq = f_[static_cast<std::size_t>(q)];
      if (!(fq < std::numeric_limits<double>::infinity())) {
        continue;
      }
      const double xq = static_cast<double>(q) * pitch;
      double s = -std::numeric_limits<double>::infinity();
      while (k >= 0) {
        const std::int64_t r = v_[static_cast<std::size_t>(k)];
        const double xr = static_cast<double>(r) * pitch;
        s = ((fq + xq * xq) - (f_[static_cast<std::size_t>(r)] + xr * xr)) / (2.0 * (xq - xr));
        if (s <= z_[static_cast<std::size_t>(k)]) {
          --k;
        } else {
          break;
        }
      }
      if (k < 0) {
        s = -std::numeric_limits<double>::infinity();
      }
      ++k;
      v_[static_cast<std::size_t>(k)] = q;
      z_[static_cast<std::size_t>(k)] = s;
    }
    if (k < 0) {
      return;
    }
    z_[static_cast<std::size_t>(k + 1)] = std::numeric_limits<double>::infinity();
    std::size_t j = 0;
    for (std::int64_t q = 0; q < n; ++q) {
      const double x = static_cast<double>(q) * pitch;
      while (z_[j + 1] < x) {
        ++j;
      }
      const std::int64_t site = v_[j];
      // From the index difference, so that a site's own distance is exactly 0 also where the
      // compiler fuses the multiplication into the subtraction.
      const double dx = static_cast<double>(q - site) * pitch;
      data[q * stride] = static_cast<float>(dx * dx + f_[static_cast<std::size_t>(site)]);
    }
  }

 private:
  std::vector<double> f_;
  std::vector<std::int64_t> v_;
  std::vector<double> z_;
};

/// Turns `field` (0 at the sites, infinity elsewhere) into squared distances in mm² to the nearest
/// site, in parallel over the lines of each axis.
void distanceTransform(std::vector<float>& field, const Index3& size, const VoxelSize& pitch) {
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const std::int64_t n = size[axis];
    const std::int64_t lines = (size[0] * size[1] * size[2]) / n;
    const std::int64_t stride = axis == 0 ? 1 : (axis == 1 ? size[0] : size[0] * size[1]);
    tbb::parallel_for(tbb::blocked_range<std::int64_t>(0, lines, 64),
                      [&](const tbb::blocked_range<std::int64_t>& range) {
                        LineTransform line;
                        for (std::int64_t l = range.begin(); l != range.end(); ++l) {
                          std::int64_t base = 0;
                          if (axis == 0) {
                            base = l * size[0];
                          } else if (axis == 1) {
                            base = (l % size[0]) + (l / size[0]) * size[0] * size[1];
                          } else {
                            base = l;
                          }
                          line.run(field.data() + base, n, stride, pitch[axis]);
                        }
                      });
  }
}

/// Statistics of the thickness values of one thread, merged at the end of each tile.
struct Accumulator {
  std::vector<std::int64_t> fine = std::vector<std::int64_t>(kFineBins, 0);
  std::int64_t count = 0;
  double sum = 0.0;
  double sum2 = 0.0;
  float min = kInf;
  float max = -kInf;
  Index3 thickest{};
  std::int64_t below = 0;
  std::int64_t above = 0;
  std::int64_t capped = 0;

  void merge(const Accumulator& other) {
    for (std::size_t i = 0; i < fine.size(); ++i) {
      fine[i] += other.fine[i];
    }
    count += other.count;
    sum += other.sum;
    sum2 += other.sum2;
    min = std::min(min, other.min);
    if (other.max > max) {
      max = other.max;
      thickest = other.thickest;
    }
    below += other.below;
    above += other.above;
    capped += other.capped;
  }
};

/// Thinnest and thickest value and the voxels beyond the limits per block of the volume.
struct BlockStats {
  float min = kInf;
  float max = -kInf;
  std::uint32_t below = 0;
  std::uint32_t above = 0;
};

/// A centre of the distance ridge, in voxels of the tile's region, and its distance in mm.
struct RidgePoint {
  std::int64_t x = 0;
  std::int64_t y = 0;
  float distance = 0.0F;
  float value = 0.0F;  // the thickness it draws
};

class MapBuilder {
 public:
  MapBuilder(const Dataset& dataset, float iso, double max_thickness, double min_wall,
             double max_wall)
      : dataset_(dataset),
        info_(dataset.info()),
        iso_(iso),
        max_thickness_(max_thickness),
        min_wall_(min_wall),
        max_wall_(max_wall) {
    const VoxelSize& v = info_.voxel_size;
    finest_ = std::min({v[0], v[1], v[2]});
    radius_ = 0.5 * (max_thickness_ + 0.5 * finest_);
    for (std::size_t a = 0; a < 3; ++a) {
      candidate_halo_[a] = static_cast<std::int64_t>(std::ceil(radius_ / v[a])) + 1;
      halo_[a] = 2 * candidate_halo_[a];
    }
    block_ = 8;
    while (blockCount(block_) > kMaxBlocks) {
      block_ *= 2;
    }
    for (std::size_t a = 0; a < 3; ++a) {
      block_dims_[a] = (info_.dims[a] + block_ - 1) / block_;
    }
    blocks_.resize(static_cast<std::size_t>(blockCount(block_)));
    tile_ = info_.brick_size;
    while (tile_ > 32 && regionVoxels(tile_) > kMaxRegionVoxels) {
      tile_ = (tile_ + 1) / 2;
    }
  }

  /// The thickness of the material voxels of `box` (NaN elsewhere), tile by tile.
  void brick(const Box& box, std::vector<float>& out) {
    std::fill(out.begin(), out.end(), kNaN);
    for (std::int64_t z = box.min[2]; z < box.max[2]; z += tile_) {
      for (std::int64_t y = box.min[1]; y < box.max[1]; y += tile_) {
        for (std::int64_t x = box.min[0]; x < box.max[0]; x += tile_) {
          const Box core{{x, y, z},
                         {std::min(x + tile_, box.max[0]), std::min(y + tile_, box.max[1]),
                          std::min(z + tile_, box.max[2])}};
          tile(core, box, out);
        }
      }
    }
  }

  [[nodiscard]] const Accumulator& total() const { return total_; }
  [[nodiscard]] const std::vector<BlockStats>& blocks() const { return blocks_; }
  [[nodiscard]] std::int64_t blockSize() const { return block_; }
  [[nodiscard]] const Index3& blockDims() const { return block_dims_; }

 private:
  [[nodiscard]] std::int64_t blockCount(std::int64_t block) const {
    std::int64_t count = 1;
    for (std::size_t a = 0; a < 3; ++a) {
      count *= (info_.dims[a] + block - 1) / block;
    }
    return count;
  }

  [[nodiscard]] std::int64_t regionVoxels(std::int64_t tile) const {
    std::int64_t count = 1;
    for (std::size_t a = 0; a < 3; ++a) {
      count *= std::min(tile, info_.dims[a]) + 2 * halo_[a];
    }
    return count;
  }

  void tile(const Box& core, const Box& box, std::vector<float>& out) {
    const VoxelSize& pitch = info_.voxel_size;
    Box outer;
    Box candidates;
    for (std::size_t a = 0; a < 3; ++a) {
      outer.min[a] = std::max<std::int64_t>(core.min[a] - halo_[a], 0);
      outer.max[a] = std::min(core.max[a] + halo_[a], info_.dims[a]);
      candidates.min[a] = std::max(core.min[a] - candidate_halo_[a], outer.min[a]);
      candidates.max[a] = std::min(core.max[a] + candidate_halo_[a], outer.max[a]);
    }
    const Index3 size{outer.size(0), outer.size(1), outer.size(2)};
    const auto at = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
      return static_cast<std::size_t>(
          (x - outer.min[0]) + size[0] * ((y - outer.min[1]) + size[1] * (z - outer.min[2])));
    };
    std::vector<float> field(static_cast<std::size_t>(outer.voxelCount()));
    // Removed air reads as NaN; it is not material, like grey values below the iso value. Voxels
    // beyond the tile's region count as material: their distances could only be larger.
    dataset_.readRegion(0, outer, field, kNaN);
    bool any = false;
    for (std::int64_t z = core.min[2]; z < core.max[2] && !any; ++z) {
      for (std::int64_t y = core.min[1]; y < core.max[1] && !any; ++y) {
        for (std::int64_t x = core.min[0]; x < core.max[0] && !any; ++x) {
          const float g = field[at(x, y, z)];
          any = !std::isnan(g) && g >= iso_;
        }
      }
    }
    if (!any) {
      return;
    }
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, field.size(), 1 << 16),
                      [&](const tbb::blocked_range<std::size_t>& range) {
                        for (std::size_t i = range.begin(); i != range.end(); ++i) {
                          const float g = field[i];
                          field[i] = !std::isnan(g) && g >= iso_ ? kInf : 0.0F;
                        }
                      });
    distanceTransform(field, size, pitch);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, field.size(), 1 << 16),
                      [&](const tbb::blocked_range<std::size_t>& range) {
                        for (std::size_t i = range.begin(); i != range.end(); ++i) {
                          field[i] = std::sqrt(field[i]);
                        }
                      });

    // The ridge: candidates whose sphere no neighbour's sphere contains, bucketed by z.
    std::array<float, 26> length{};
    {
      std::size_t k = 0;
      for (std::int64_t dz = -1; dz <= 1; ++dz) {
        for (std::int64_t dy = -1; dy <= 1; ++dy) {
          for (std::int64_t dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0 && dz == 0) {
              continue;
            }
            length[k] =
                static_cast<float>(std::sqrt(static_cast<double>(dx * dx) * pitch[0] * pitch[0] +
                                             static_cast<double>(dy * dy) * pitch[1] * pitch[1] +
                                             static_cast<double>(dz * dz) * pitch[2] * pitch[2]));
            ++k;
          }
        }
      }
    }
    const auto radius = static_cast<float>(radius_);
    const float half_pitch = 0.5F * static_cast<float>(finest_);
    // The steepest rise of the distance from (x, y, z) to a neighbour inside `bounds`, per mm,
    // and that neighbour.
    struct Rise {
      float slope = -kInf;
      Index3 to{};
    };
    const auto steepest = [&](const Index3& p, const Box& bounds) {
      Rise rise;
      const float d = field[at(p[0], p[1], p[2])];
      std::size_t k = 0;
      for (std::int64_t dz = -1; dz <= 1; ++dz) {
        for (std::int64_t dy = -1; dy <= 1; ++dy) {
          for (std::int64_t dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0 && dz == 0) {
              continue;
            }
            const std::size_t n = k++;
            const Index3 q{p[0] + dx, p[1] + dy, p[2] + dz};
            if (q[0] < bounds.min[0] || q[0] >= bounds.max[0] || q[1] < bounds.min[1] ||
                q[1] >= bounds.max[1] || q[2] < bounds.min[2] || q[2] >= bounds.max[2]) {
              continue;
            }
            const float slope = (field[at(q[0], q[1], q[2])] - d) / length[n];
            if (slope > rise.slope) {
              rise = {slope, q};
            }
          }
        }
      }
      return rise;
    };
    const std::int64_t max_steps =
        4 * (static_cast<std::int64_t>(std::ceil(radius_ / finest_)) + 1);
    std::vector<std::vector<RidgePoint>> ridge(static_cast<std::size_t>(candidates.size(2)));
    std::vector<std::uint8_t> capped_planes(static_cast<std::size_t>(candidates.size(2)), 0);
    tbb::parallel_for(candidates.min[2], candidates.max[2], [&](std::int64_t z) {
      auto& points = ridge[static_cast<std::size_t>(z - candidates.min[2])];
      for (std::int64_t y = candidates.min[1]; y < candidates.max[1]; ++y) {
        for (std::int64_t x = candidates.min[0]; x < candidates.max[0]; ++x) {
          const float d = field[at(x, y, z)];
          if (d < half_pitch) {
            continue;  // not material: a material voxel is at least a pitch from air
          }
          if (d >= radius) {
            capped_planes[static_cast<std::size_t>(z - candidates.min[2])] = 1;
            continue;
          }
          // A neighbour whose sphere contains this one rises by the distance between them.
          const Rise rise = steepest({x, y, z}, candidates);
          if (rise.slope >= kContainSlope) {
            continue;
          }
          float value = 2.0F * d - half_pitch;
          if (rise.slope >= kEdgeSlope) {
            // A ridge that rises steeply runs into a convex edge, where only small spheres fit:
            // the edge belongs to the wall at the top of the rise.
            Index3 p = rise.to;
            float top = field[at(p[0], p[1], p[2])];
            for (std::int64_t step = 0; step < max_steps && top < radius; ++step) {
              const Rise next = steepest(p, outer);
              if (next.slope < kEdgeSlope) {
                break;
              }
              p = next.to;
              top = field[at(p[0], p[1], p[2])];
            }
            value = top >= radius ? static_cast<float>(max_thickness_)
                                  : std::max(value, 2.0F * top - half_pitch);
          }
          points.push_back({x, y, d, value});
        }
      }
    });

    // Draw the spheres into the core, one z plane per task.
    const Index3 core_size{core.size(0), core.size(1), core.size(2)};
    std::vector<float> thickness(static_cast<std::size_t>(core.voxelCount()), kNaN);
    const auto core_at = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
      return static_cast<std::size_t>((x - core.min[0]) +
                                      core_size[0] *
                                          ((y - core.min[1]) + core_size[1] * (z - core.min[2])));
    };
    const auto reach = [&](std::size_t axis) {
      return static_cast<std::int64_t>(std::ceil(radius_ / pitch[axis]));
    };
    const std::int64_t reach_z = reach(2);
    tbb::parallel_for(core.min[2], core.max[2], [&](std::int64_t z) {
      for (std::int64_t y = core.min[1]; y < core.max[1]; ++y) {
        for (std::int64_t x = core.min[0]; x < core.max[0]; ++x) {
          // At least its own sphere, also where the left out spheres would have been the largest.
          if (const float d = field[at(x, y, z)]; d >= half_pitch) {
            thickness[core_at(x, y, z)] = 2.0F * d - half_pitch;
          }
        }
      }
      const std::int64_t first = std::max(z - reach_z, candidates.min[2]);
      const std::int64_t last = std::min(z + reach_z, candidates.max[2] - 1);
      for (std::int64_t zc = first; zc <= last; ++zc) {
        const double dz = static_cast<double>(z - zc) * pitch[2];
        for (const RidgePoint& c : ridge[static_cast<std::size_t>(zc - candidates.min[2])]) {
          const double d = c.distance;
          const double rest = d * d - dz * dz;
          if (rest <= 0.0) {
            continue;
          }
          const float value = c.value;
          const double ry = std::sqrt(rest) / pitch[1];
          const std::int64_t y0 =
              std::max(static_cast<std::int64_t>(std::floor(static_cast<double>(c.y) - ry)) + 1,
                       core.min[1]);
          const std::int64_t y1 =
              std::min(static_cast<std::int64_t>(std::ceil(static_cast<double>(c.y) + ry)) - 1,
                       core.max[1] - 1);
          for (std::int64_t y = y0; y <= y1; ++y) {
            const double dy = static_cast<double>(y - c.y) * pitch[1];
            const double row = rest - dy * dy;
            if (row <= 0.0) {
              continue;
            }
            const double rx = std::sqrt(row) / pitch[0];
            const std::int64_t x0 =
                std::max(static_cast<std::int64_t>(std::floor(static_cast<double>(c.x) - rx)) + 1,
                         core.min[0]);
            const std::int64_t x1 =
                std::min(static_cast<std::int64_t>(std::ceil(static_cast<double>(c.x) + rx)) - 1,
                         core.max[0] - 1);
            float* line = thickness.data() + core_at(core.min[0], y, z);
            for (std::int64_t x = x0; x <= x1; ++x) {
              // What is not material (NaN) stays as it is.
              float& t = line[x - core.min[0]];
              if (t < value) {
                t = value;
              }
            }
          }
        }
      }
    });

    // Voxels within the maximum radius of a voxel at least that deep get the maximum.
    if (std::any_of(capped_planes.begin(), capped_planes.end(),
                    [](std::uint8_t c) { return c != 0; })) {
      tbb::parallel_for(outer.min[2], outer.max[2], [&](std::int64_t z) {
        for (std::int64_t y = outer.min[1]; y < outer.max[1]; ++y) {
          for (std::int64_t x = outer.min[0]; x < outer.max[0]; ++x) {
            const bool inside = x >= candidates.min[0] && x < candidates.max[0] &&
                                y >= candidates.min[1] && y < candidates.max[1] &&
                                z >= candidates.min[2] && z < candidates.max[2];
            float& f = field[at(x, y, z)];
            f = inside && f >= radius ? 0.0F : kInf;
          }
        }
      });
      distanceTransform(field, size, pitch);
      const float radius2 = radius * radius;
      const auto max_value = static_cast<float>(max_thickness_);
      tbb::parallel_for(core.min[2], core.max[2], [&](std::int64_t z) {
        for (std::int64_t y = core.min[1]; y < core.max[1]; ++y) {
          for (std::int64_t x = core.min[0]; x < core.max[0]; ++x) {
            float& t = thickness[core_at(x, y, z)];
            if (!std::isnan(t) && field[at(x, y, z)] < radius2) {
              t = max_value;
            }
          }
        }
      });
    }

    collect(core, thickness);
    for (std::int64_t z = core.min[2]; z < core.max[2]; ++z) {
      for (std::int64_t y = core.min[1]; y < core.max[1]; ++y) {
        const float* from = thickness.data() + core_at(core.min[0], y, z);
        float* to =
            out.data() + static_cast<std::size_t>(
                             (core.min[0] - box.min[0]) +
                             box.size(0) * ((y - box.min[1]) + box.size(1) * (z - box.min[2])));
        std::copy(from, from + core_size[0], to);
      }
    }
  }

  /// Adds the thickness of the core to the statistics and the blocks.
  void collect(const Box& core, std::vector<float>& thickness) {
    const auto max_value = static_cast<float>(max_thickness_);
    const std::int64_t b = block_;
    const std::int64_t first_block = core.min[2] / b;
    const std::int64_t last_block = (core.max[2] - 1) / b;
    tbb::enumerable_thread_specific<Accumulator> local;
    // One block layer per task, so that no two tasks touch the same block.
    tbb::parallel_for(first_block, last_block + 1, [&](std::int64_t bz) {
      Accumulator& acc = local.local();
      const std::int64_t z0 = std::max(core.min[2], bz * b);
      const std::int64_t z1 = std::min(core.max[2], (bz + 1) * b);
      for (std::int64_t z = z0; z < z1; ++z) {
        for (std::int64_t y = core.min[1]; y < core.max[1]; ++y) {
          for (std::int64_t x = core.min[0]; x < core.max[0]; ++x) {
            float& t = thickness[static_cast<std::size_t>(
                (x - core.min[0]) +
                core.size(0) * ((y - core.min[1]) + core.size(1) * (z - core.min[2])))];
            if (std::isnan(t)) {
              continue;
            }
            t = std::min(t, max_value);
            const int bin =
                std::clamp(static_cast<int>(t / max_value * kFineBins), 0, kFineBins - 1);
            ++acc.fine[static_cast<std::size_t>(bin)];
            ++acc.count;
            acc.sum += t;
            acc.sum2 += static_cast<double>(t) * t;
            acc.min = std::min(acc.min, t);
            if (t > acc.max) {
              acc.max = t;
              acc.thickest = {x, y, z};
            }
            const bool below = min_wall_ > 0.0 && t < min_wall_;
            const bool above = max_wall_ > 0.0 && t > max_wall_;
            acc.below += below ? 1 : 0;
            acc.above += above ? 1 : 0;
            acc.capped += t >= max_value ? 1 : 0;
            BlockStats& block = blocks_[static_cast<std::size_t>(
                x / b + block_dims_[0] * (y / b + block_dims_[1] * bz))];
            block.min = std::min(block.min, t);
            block.max = std::max(block.max, t);
            block.below += below ? 1U : 0U;
            block.above += above ? 1U : 0U;
          }
        }
      }
    });
    const std::scoped_lock lock(mutex_);
    for (const Accumulator& acc : local) {
      total_.merge(acc);
    }
  }

  const Dataset& dataset_;
  const DatasetInfo& info_;
  float iso_;
  double max_thickness_;
  double min_wall_;
  double max_wall_;
  double finest_ = 0.0;
  double radius_ = 0.0;
  Index3 candidate_halo_{};
  Index3 halo_{};
  std::int64_t tile_ = 0;
  std::int64_t block_ = 8;
  Index3 block_dims_{};
  std::vector<BlockStats> blocks_;
  Accumulator total_;
  std::mutex mutex_;
};

/// Connected blocks (26-neighbourhood) that `flagged` selects.
template <class Flagged, class Voxels, class Extreme>
std::vector<ThicknessMapRegion> blockRegions(const std::vector<BlockStats>& blocks,
                                             const Index3& dims, std::int64_t b,
                                             const VoxelSize& pitch, Flagged flagged, Voxels voxels,
                                             Extreme extreme, bool thinnest) {
  std::vector<ThicknessMapRegion> regions;
  std::vector<std::uint8_t> seen(blocks.size(), 0);
  std::vector<std::int64_t> stack;
  for (std::size_t start = 0; start < blocks.size(); ++start) {
    if (seen[start] != 0 || !flagged(blocks[start])) {
      continue;
    }
    ThicknessMapRegion region;
    region.extreme_mm = thinnest ? std::numeric_limits<double>::infinity()
                                 : -std::numeric_limits<double>::infinity();
    Index3 low{dims[0], dims[1], dims[2]};
    Index3 high{-1, -1, -1};
    seen[start] = 1;
    stack.assign(1, static_cast<std::int64_t>(start));
    while (!stack.empty()) {
      const std::int64_t i = stack.back();
      stack.pop_back();
      const Index3 p{i % dims[0], (i / dims[0]) % dims[1], i / (dims[0] * dims[1])};
      const BlockStats& block = blocks[static_cast<std::size_t>(i)];
      region.voxels += voxels(block);
      const double e = extreme(block);
      if (thinnest ? e < region.extreme_mm : e > region.extreme_mm) {
        region.extreme_mm = e;
        for (std::size_t a = 0; a < 3; ++a) {
          region.location_mm[a] =
              (static_cast<double>(p[a] * b) + 0.5 * static_cast<double>(b) - 0.5) * pitch[a];
        }
      }
      for (std::size_t a = 0; a < 3; ++a) {
        low[a] = std::min(low[a], p[a]);
        high[a] = std::max(high[a], p[a]);
      }
      for (std::int64_t dz = -1; dz <= 1; ++dz) {
        for (std::int64_t dy = -1; dy <= 1; ++dy) {
          for (std::int64_t dx = -1; dx <= 1; ++dx) {
            const Index3 q{p[0] + dx, p[1] + dy, p[2] + dz};
            if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] >= dims[0] || q[1] >= dims[1] ||
                q[2] >= dims[2]) {
              continue;
            }
            const auto j = static_cast<std::size_t>(q[0] + dims[0] * (q[1] + dims[1] * q[2]));
            if (seen[j] == 0 && flagged(blocks[j])) {
              seen[j] = 1;
              stack.push_back(static_cast<std::int64_t>(j));
            }
          }
        }
      }
    }
    region.volume_mm3 = static_cast<double>(region.voxels) * pitch[0] * pitch[1] * pitch[2];
    for (std::size_t a = 0; a < 3; ++a) {
      region.bounds_mm[0][a] = static_cast<double>(low[a] * b) * pitch[a];
      region.bounds_mm[1][a] = static_cast<double>((high[a] + 1) * b - 1) * pitch[a];
    }
    regions.push_back(region);
  }
  std::sort(regions.begin(), regions.end(), [thinnest](const auto& a, const auto& b2) {
    return thinnest ? a.extreme_mm < b2.extreme_mm : a.extreme_mm > b2.extreme_mm;
  });
  for (std::size_t i = 0; i < regions.size(); ++i) {
    regions[i].id = static_cast<int>(i + 1);
  }
  return regions;
}

Json mapRegionsJson(const std::vector<ThicknessMapRegion>& regions) {
  Json list = Json::array();
  for (std::size_t i = 0; i < std::min(regions.size(), detail::kListedRegions); ++i) {
    const ThicknessMapRegion& r = regions[i];
    list.push_back({{"id", r.id},
                    {"thickness_mm", r.extreme_mm},
                    {"volume_mm3", r.volume_mm3},
                    {"voxels", r.voxels},
                    {"location_mm", r.location_mm},
                    {"bounds_mm", r.bounds_mm}});
  }
  return list;
}

/// Slices along each axis through `centre` (level-0 voxels): the grey values with the material
/// coloured by its thickness.
void writeMapSlices(const Dataset& dataset, const Dataset& map, const ThicknessMapResult& result,
                    const Index3& centre, const std::filesystem::path& dir) {
  const DatasetInfo& info = dataset.info();
  constexpr std::array<char, 3> kNames{'x', 'y', 'z'};
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const std::size_t u = axis == 0 ? 1 : 0;
    const std::size_t v = axis == 2 ? 1 : 2;
    int level = 0;
    while (level + 1 < static_cast<int>(info.levels.size()) &&
           std::max(info.levels[static_cast<std::size_t>(level)].dims[u],
                    info.levels[static_cast<std::size_t>(level)].dims[v]) > kSlicePixels) {
      ++level;
    }
    const LevelInfo& li = info.levels[static_cast<std::size_t>(level)];
    Box slice;
    slice.max = li.dims;
    slice.min[axis] = std::clamp<std::int64_t>(centre[axis] >> level, 0, li.dims[axis] - 1);
    slice.max[axis] = slice.min[axis] + 1;
    std::vector<float> grey(static_cast<std::size_t>(slice.voxelCount()));
    std::vector<float> thickness(grey.size());
    dataset.readRegion(level, slice, grey, info.air_level);
    map.readRegion(level, slice, thickness, kNaN);
    const float lo = info.air_level;
    float hi = lo + 1.0F;
    for (const float g : grey) {
      hi = std::max(hi, g);
    }
    const std::int64_t width = li.dims[u];
    const std::int64_t height = li.dims[v];
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width * height * 3));
    for (std::int64_t y = 0; y < height; ++y) {
      for (std::int64_t x = 0; x < width; ++x) {
        Index3 p{};
        p[u] = x;
        p[v] = y;
        p[axis] = slice.min[axis];
        const auto i = static_cast<std::size_t>(
            (p[0] - slice.min[0]) +
            slice.size(0) * ((p[1] - slice.min[1]) + slice.size(1) * (p[2] - slice.min[2])));
        const double level_grey = std::clamp((grey[i] - lo) / (hi - lo), 0.0F, 1.0F) * 255.0;
        std::array<double, 3> colour{level_grey, level_grey, level_grey};
        if (!std::isnan(thickness[i])) {
          const auto tint =
              thicknessColor(thickness[i], result.min_wall_mm, result.max_wall_mm, result.range_mm);
          for (std::size_t c = 0; c < 3; ++c) {
            colour[c] = 0.3 * colour[c] + 0.7 * tint[c];
          }
        }
        const auto o = static_cast<std::size_t>((x + width * y) * 3);
        for (std::size_t c = 0; c < 3; ++c) {
          rgb[o + c] = static_cast<std::uint8_t>(std::clamp(colour[c], 0.0, 255.0));
        }
      }
    }
    detail::writeRgbPng(dir / (std::string("thickness_map_") + kNames[axis] + ".png"),
                        static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), rgb,
                        li.voxel_size[u], li.voxel_size[v]);
  }
}

}  // namespace

ThicknessMapResult writeThicknessMap(const Dataset& dataset, const SurfaceMask& surface,
                                     const std::filesystem::path& dir,
                                     const ThicknessMapOptions& options) {
  if (options.max_thickness_mm < 0.0 || options.min_wall_mm < 0.0 || options.max_wall_mm < 0.0) {
    throw std::invalid_argument("Wall thickness limits must not be negative");
  }
  if (options.min_wall_mm > 0.0 && options.max_wall_mm > 0.0 &&
      options.min_wall_mm >= options.max_wall_mm) {
    throw std::invalid_argument("The minimum wall thickness must be below the maximum");
  }
  const DatasetInfo& info = dataset.info();
  if (surface.info().dims != info.dims) {
    throw std::invalid_argument("The surface does not belong to the dataset");
  }
  const std::filesystem::path map_dir = dir / "thickness_map";
  if (std::filesystem::exists(map_dir) && !std::filesystem::is_empty(map_dir)) {
    throw std::invalid_argument("A thickness map exists already in " + dir.string());
  }
  const VoxelSize& pitch = info.voxel_size;
  const double finest = std::min({pitch[0], pitch[1], pitch[2]});
  ThicknessMapResult result;
  result.voxel_size = pitch;
  result.iso_value = surface.info().iso_value;
  result.min_wall_mm = options.min_wall_mm;
  result.max_wall_mm = options.max_wall_mm;
  if (options.max_thickness_mm > 0.0) {
    result.max_thickness_mm = options.max_thickness_mm;
  } else {
    // The extent of the part from its bricks: dims would include air the sieve kept.
    Index3 low{info.dims[0], info.dims[1], info.dims[2]};
    Index3 high{0, 0, 0};
    for (const Index3& brick : info.levels.front().bricks) {
      const Box box = dataset.brickBox(0, brick);
      for (std::size_t a = 0; a < 3; ++a) {
        low[a] = std::min(low[a], box.min[a]);
        high[a] = std::max(high[a], box.max[a]);
      }
    }
    std::array<double, 3> extent{};
    for (std::size_t a = 0; a < 3; ++a) {
      extent[a] = static_cast<double>(std::max<std::int64_t>(high[a] - low[a], 1)) * pitch[a];
    }
    result.max_thickness_mm = std::min({extent[0], extent[1], extent[2], 64.0 * finest});
  }
  result.max_thickness_mm = std::max(result.max_thickness_mm, 2.0 * finest);

  MapBuilder builder(dataset, result.iso_value, result.max_thickness_mm, options.min_wall_mm,
                     options.max_wall_mm);
  DerivedDatasetOptions derived;
  derived.value_type = ValueType::kFloat;
  if (options.progress) {
    derived.progress = [&options](std::string_view stage, double fraction) {
      options.progress(stage == "bricks" ? 0.9 * fraction : 0.9 + 0.1 * fraction);
    };
  }
  {
    const TelemetryPhase phase("thickness map");
    std::filesystem::create_directories(dir);
    (void)writeDerivedDataset(
        dataset, map_dir,
        [&builder](const Index3& /*brick*/, const Box& box, std::vector<float>& values) {
          builder.brick(box, values);
        },
        derived);
  }

  const TelemetryPhase phase("thickness map statistics");
  const Accumulator& total = builder.total();
  result.material_voxels = total.count;
  result.material_volume_mm3 = static_cast<double>(total.count) * pitch[0] * pitch[1] * pitch[2];
  result.histogram.assign(kHistogramBins, 0.0);
  if (total.count > 0) {
    const auto n = static_cast<double>(total.count);
    result.min_mm = total.min;
    result.max_mm = total.max;
    result.mean_mm = total.sum / n;
    result.std_mm = std::sqrt(std::max(0.0, total.sum2 / n - result.mean_mm * result.mean_mm));
    result.below_min = static_cast<double>(total.below) / n;
    result.above_max = static_cast<double>(total.above) / n;
    result.capped = static_cast<double>(total.capped) / n;
    for (std::size_t a = 0; a < 3; ++a) {
      result.thickest_mm[a] = static_cast<double>(total.thickest[a]) * pitch[a];
    }
    // Percentiles from the fine histogram, at the upper edge of their bin.
    const double bin_mm = result.max_thickness_mm / kFineBins;
    std::int64_t cumulative = 0;
    std::size_t next = 0;
    for (std::size_t bin = 0; bin < total.fine.size() && next < kThicknessPercentiles.size();
         ++bin) {
      cumulative += total.fine[bin];
      while (next < kThicknessPercentiles.size() &&
             static_cast<double>(cumulative) >= kThicknessPercentiles[next] / 100.0 * n) {
        result.percentiles_mm[next++] =
            std::clamp(static_cast<double>(bin + 1) * bin_mm, result.min_mm, result.max_mm);
      }
    }
    for (; next < kThicknessPercentiles.size(); ++next) {
      result.percentiles_mm[next] = result.max_mm;
    }
    result.range_mm = detail::niceCeil(
        std::max({result.percentiles_mm[4], 1.25 * options.min_wall_mm, options.max_wall_mm}));
    for (std::size_t bin = 0; bin < total.fine.size(); ++bin) {
      const double t = (static_cast<double>(bin) + 0.5) * bin_mm;
      const int h = std::clamp(static_cast<int>(std::floor(t / result.range_mm * kHistogramBins)),
                               0, kHistogramBins - 1);
      result.histogram[static_cast<std::size_t>(h)] += static_cast<double>(total.fine[bin]) / n;
    }
  }
  result.fine_bin_mm = result.max_thickness_mm / kFineBins;
  result.fine_histogram = total.fine;
  result.block_voxels = builder.blockSize();
  if (options.min_wall_mm > 0.0) {
    result.thin_regions = blockRegions(
        builder.blocks(), builder.blockDims(), builder.blockSize(), pitch,
        [](const BlockStats& b) { return b.below > 0; },
        [](const BlockStats& b) { return static_cast<std::int64_t>(b.below); },
        [](const BlockStats& b) { return static_cast<double>(b.min); }, true);
  }
  if (options.max_wall_mm > 0.0) {
    result.thick_regions = blockRegions(
        builder.blocks(), builder.blockDims(), builder.blockSize(), pitch,
        [](const BlockStats& b) { return b.above > 0; },
        [](const BlockStats& b) { return static_cast<std::int64_t>(b.above); },
        [](const BlockStats& b) { return static_cast<double>(b.max); }, false);
  }
  writeJson(dir / "thickness_map.json", toJson(result));
  if (total.count > 0) {
    const std::array<double, 3> spot = result.thick_regions.empty()
                                           ? result.thickest_mm
                                           : result.thick_regions.front().location_mm;
    Index3 centre{};
    for (std::size_t a = 0; a < 3; ++a) {
      centre[a] = std::clamp<std::int64_t>(std::llround(spot[a] / pitch[a]), 0, info.dims[a] - 1);
    }
    const Dataset map = Dataset::open(map_dir);
    writeMapSlices(dataset, map, result, centre, dir);
  }
  return result;
}

nlohmann::json toJson(const ThicknessMapResult& result) {
  Json percentiles = Json::object();
  for (std::size_t i = 0; i < kThicknessPercentiles.size(); ++i) {
    percentiles[std::to_string(static_cast<int>(kThicknessPercentiles[i]))] =
        result.percentiles_mm[i];
  }
  return {{"voxel_size_mm", result.voxel_size},
          {"iso_value", result.iso_value},
          {"max_thickness_mm", result.max_thickness_mm},
          {"min_wall_mm", result.min_wall_mm},
          {"max_wall_mm", result.max_wall_mm},
          {"dataset", "thickness_map"},
          {"thickness",
           {{"material_voxels", result.material_voxels},
            {"material_volume_mm3", result.material_volume_mm3},
            {"min_mm", result.min_mm},
            {"max_mm", result.max_mm},
            {"mean_mm", result.mean_mm},
            {"std_mm", result.std_mm},
            {"percentiles_mm", percentiles},
            {"below_min", result.below_min},
            {"above_max", result.above_max},
            {"capped", result.capped},
            {"range_mm", result.range_mm},
            {"histogram", result.histogram},
            {"thickest_mm", result.thickest_mm},
            {"fine_bin_mm", result.fine_bin_mm},
            {"fine_histogram", result.fine_histogram}}},
          {"block_voxels", result.block_voxels},
          {"thin_region_count", result.thin_regions.size()},
          {"thin_regions", mapRegionsJson(result.thin_regions)},
          {"thick_region_count", result.thick_regions.size()},
          {"thick_regions", mapRegionsJson(result.thick_regions)}};
}

}  // namespace voxelsieve
