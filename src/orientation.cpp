#include "voxelsieve/orientation.hpp"

#include <openvdb/openvdb.h>
#include <tbb/blocked_range.h>
#include <tbb/combinable.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>

#include "detail/png.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/telemetry.hpp"
#include "voxelsieve/vdb.hpp"

namespace voxelsieve {
namespace {

using Index3 = std::array<std::int64_t, 3>;
using Vec = std::array<double, 3>;
using Mat = std::array<double, 9>;  // row major

/// Eigen decomposition of a symmetric 3x3 matrix by Jacobi rotations: eigenvalues descending and
/// the eigenvectors as rows.
std::pair<Vec, std::array<Vec, 3>> symmetricEigen(Mat a) {
  Mat v{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};  // columns are the eigenvectors
  for (int sweep = 0; sweep < 50; ++sweep) {
    const double off = a[1] * a[1] + a[2] * a[2] + a[5] * a[5];
    if (off < 1e-30 * (a[0] * a[0] + a[4] * a[4] + a[8] * a[8]) + 1e-300) {
      break;
    }
    for (const auto& [p, q] :
         {std::pair<std::size_t, std::size_t>{0, 1}, std::pair<std::size_t, std::size_t>{0, 2},
          std::pair<std::size_t, std::size_t>{1, 2}}) {
      const double apq = a[3 * p + q];
      if (std::abs(apq) < 1e-300) {
        continue;
      }
      const double theta = (a[3 * q + q] - a[3 * p + p]) / (2.0 * apq);
      const double t =
          (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
      const double c = 1.0 / std::sqrt(t * t + 1.0);
      const double s = t * c;
      // a = J^T a J and v = v J with the rotation J in the plane (p, q).
      for (std::size_t k = 0; k < 3; ++k) {
        const double akp = a[3 * k + p];
        const double akq = a[3 * k + q];
        a[3 * k + p] = c * akp - s * akq;
        a[3 * k + q] = s * akp + c * akq;
      }
      for (std::size_t k = 0; k < 3; ++k) {
        const double apk = a[3 * p + k];
        const double aqk = a[3 * q + k];
        a[3 * p + k] = c * apk - s * aqk;
        a[3 * q + k] = s * apk + c * aqk;
      }
      for (std::size_t k = 0; k < 3; ++k) {
        const double vkp = v[3 * k + p];
        const double vkq = v[3 * k + q];
        v[3 * k + p] = c * vkp - s * vkq;
        v[3 * k + q] = s * vkp + c * vkq;
      }
    }
  }
  std::array<std::size_t, 3> order{0, 1, 2};
  std::sort(order.begin(), order.end(),
            [&a](std::size_t l, std::size_t r) { return a[4 * l] > a[4 * r]; });
  Vec values{};
  std::array<Vec, 3> vectors{};
  for (std::size_t i = 0; i < 3; ++i) {
    values[i] = a[4 * order[i]];
    for (std::size_t r = 0; r < 3; ++r) {
      vectors[i][r] = v[3 * r + order[i]];
    }
  }
  return {values, vectors};
}

double dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/// Normalised Gaussian kernel of `sigma` voxels, radius 3 sigma rounded up.
std::vector<float> gaussianKernel(double sigma) {
  if (sigma <= 0.0) {
    return {1.0F};
  }
  const auto radius = static_cast<int>(std::ceil(3.0 * sigma));
  std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
  double sum = 0.0;
  for (std::size_t k = 0; k < kernel.size(); ++k) {
    const double i = static_cast<double>(k) - radius;
    const double w = std::exp(-0.5 * i * i / (sigma * sigma));
    kernel[k] = static_cast<float>(w);
    sum += w;
  }
  for (float& w : kernel) {
    w = static_cast<float>(w / sum);
  }
  return kernel;
}

/// Convolves `data` (dims `n`, x fastest) along `axis` with `kernel`, clamping at the borders.
void convolve(std::vector<float>& data, const Index3& n, int axis, std::span<const float> kernel) {
  const auto radius = static_cast<std::int64_t>(kernel.size() / 2);
  if (radius == 0) {
    return;
  }
  const std::int64_t stride = axis == 0 ? 1 : axis == 1 ? n[0] : n[0] * n[1];
  const std::int64_t length = n[static_cast<std::size_t>(axis)];
  std::vector<float> line(static_cast<std::size_t>(length));
  const std::int64_t lines = n[0] * n[1] * n[2] / length;
  for (std::int64_t l = 0; l < lines; ++l) {
    // Start of line l: all voxels with coordinate 0 along the axis.
    std::int64_t start = 0;
    if (axis == 0) {
      start = l * n[0];
    } else if (axis == 1) {
      start = (l % n[0]) + (l / n[0]) * n[0] * n[1];
    } else {
      start = l;
    }
    for (std::int64_t i = 0; i < length; ++i) {
      line[static_cast<std::size_t>(i)] = data[static_cast<std::size_t>(start + i * stride)];
    }
    for (std::int64_t i = 0; i < length; ++i) {
      double sum = 0.0;
      for (std::int64_t k = -radius; k <= radius; ++k) {
        const std::int64_t j = std::clamp<std::int64_t>(i + k, 0, length - 1);
        sum += kernel[static_cast<std::size_t>(k + radius)] * line[static_cast<std::size_t>(j)];
      }
      data[static_cast<std::size_t>(start + i * stride)] = static_cast<float>(sum);
    }
  }
}

/// The material analysed: `options.material`, or the one with the highest grey value.
int chosenMaterial(const OrientationOptions& options) {
  if (options.materials == nullptr) {
    return 0;
  }
  const auto& materials = options.materials->info().materials;
  if (materials.empty()) {
    throw std::invalid_argument("The material segmentation has no materials");
  }
  if (options.material != 0) {
    if (std::none_of(materials.begin(), materials.end(),
                     [&](const Material& m) { return m.id == options.material; })) {
      throw std::invalid_argument("No material " + std::to_string(options.material) +
                                  " in the segmentation");
    }
    return options.material;
  }
  return std::max_element(materials.begin(), materials.end(),
                          [](const Material& a, const Material& b) { return a.lower < b.lower; })
      ->id;
}

/// Groups unit directions (sign arbitrary) into `k` families: k-means with the squared sine of
/// the angle as distance, so that p and -p are the same. Starts from the main axes of the
/// orientation tensor and from the directions farthest from the families found so far.
std::vector<int> groupDirections(const std::vector<OrientationBlock>& blocks,
                                 const std::vector<std::size_t>& members, int k,
                                 std::vector<Vec>& centres) {
  centres.clear();
  const auto weight = [&](std::size_t i) { return static_cast<double>(blocks[i].voxels); };
  Mat tensor{};
  for (const std::size_t i : members) {
    const Vec& p = blocks[i].direction;
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 3; ++c) {
        tensor[3 * r + c] += weight(i) * p[r] * p[c];
      }
    }
  }
  centres.push_back(symmetricEigen(tensor).second[0]);
  while (static_cast<int>(centres.size()) < k) {
    // The direction least like every centre so far.
    double worst = -1.0;
    Vec next = centres.front();
    for (const std::size_t i : members) {
      double best = 0.0;
      for (const Vec& c : centres) {
        best = std::max(best, std::abs(dot(blocks[i].direction, c)));
      }
      if (1.0 - best > worst) {
        worst = 1.0 - best;
        next = blocks[i].direction;
      }
    }
    centres.push_back(next);
  }
  std::vector<int> family(members.size(), 0);
  for (int iteration = 0; iteration < 50; ++iteration) {
    bool changed = false;
    for (std::size_t m = 0; m < members.size(); ++m) {
      int best = 0;
      double best_cos = -1.0;
      for (std::size_t c = 0; c < centres.size(); ++c) {
        const double cos = std::abs(dot(blocks[members[m]].direction, centres[c]));
        if (cos > best_cos) {
          best_cos = cos;
          best = static_cast<int>(c);
        }
      }
      changed = changed || family[m] != best;
      family[m] = best;
    }
    // Each centre: main axis of the orientation tensor of its members.
    for (std::size_t c = 0; c < centres.size(); ++c) {
      Mat t{};
      bool any = false;
      for (std::size_t m = 0; m < members.size(); ++m) {
        if (family[m] != static_cast<int>(c)) {
          continue;
        }
        const Vec& p = blocks[members[m]].direction;
        for (std::size_t r = 0; r < 3; ++r) {
          for (std::size_t q = 0; q < 3; ++q) {
            t[3 * r + q] += weight(members[m]) * p[r] * p[q];
          }
        }
        any = true;
      }
      if (any) {
        centres[c] = symmetricEigen(t).second[0];
      }
    }
    if (!changed && iteration > 0) {
      break;
    }
  }
  return family;
}

/// Canonical sign of a direction: its largest component positive.
Vec canonical(Vec p) {
  std::size_t largest = 0;
  for (std::size_t a = 1; a < 3; ++a) {
    if (std::abs(p[a]) > std::abs(p[largest])) {
      largest = a;
    }
  }
  if (p[largest] < 0.0) {
    for (double& v : p) {
      v = -v;
    }
  }
  return p;
}

/// Directions of the blocks of row (by, bz) along x.
std::vector<OrientationBlock> analyzeRow(const Dataset& dataset, const OrientationOptions& options,
                                         int material, std::span<const float> kernel,
                                         std::int64_t by, std::int64_t bz) {
  const DatasetInfo& info = dataset.info();
  const std::int64_t b = options.block_size;
  const auto halo = static_cast<std::int64_t>(kernel.size() / 2) + 1;
  const std::int64_t blocks_x = (info.dims[0] + b - 1) / b;
  const auto min_voxels =
      static_cast<std::int64_t>(std::ceil(options.min_fill * static_cast<double>(b * b * b)));
  std::vector<OrientationBlock> found;
  // The whole row of blocks along x, with a halo for smoothing and gradients.
  Box box;
  box.min = {0, by * b - halo, bz * b - halo};
  box.max = {info.dims[0], (by + 1) * b + halo, (bz + 1) * b + halo};
  Box inner = box;
  for (std::size_t a = 0; a < 3; ++a) {
    inner.min[a] = std::max<std::int64_t>(0, box.min[a]);
    inner.max[a] = std::min(info.dims[a], box.max[a]);
  }
  const Index3 n{inner.size(0), inner.size(1), inner.size(2)};
  std::vector<float> grey(static_cast<std::size_t>(inner.voxelCount()));
  dataset.readRegion(0, inner, grey, info.air_level);
  // Analysed voxels: above the air threshold, or of the material.
  std::vector<std::uint8_t> mask(grey.size(), 0);
  if (options.materials != nullptr) {
    options.materials->readRegion(inner, mask);
    for (std::uint8_t& m : mask) {
      m = m == material ? 1 : 0;
    }
  } else {
    for (std::size_t i = 0; i < grey.size(); ++i) {
      mask[i] = grey[i] >= info.threshold ? 1 : 0;
    }
  }
  bool any = false;
  for (const std::uint8_t m : mask) {
    any = any || m != 0;
  }
  if (!any) {
    return found;
  }
  for (int axis = 0; axis < 3; ++axis) {
    convolve(grey, n, axis, kernel);
  }
  const auto at = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
    return static_cast<std::size_t>(x + n[0] * (y + n[1] * z));
  };
  for (std::int64_t bx = 0; bx < blocks_x; ++bx) {
    Mat j{};
    std::int64_t count = 0;
    for (std::int64_t z = bz * b; z < std::min(info.dims[2], (bz + 1) * b); ++z) {
      for (std::int64_t y = by * b; y < std::min(info.dims[1], (by + 1) * b); ++y) {
        for (std::int64_t x = bx * b; x < std::min(info.dims[0], (bx + 1) * b); ++x) {
          const std::int64_t lx = x - inner.min[0];
          const std::int64_t ly = y - inner.min[1];
          const std::int64_t lz = z - inner.min[2];
          if (lx < 1 || ly < 1 || lz < 1 || lx + 1 >= n[0] || ly + 1 >= n[1] || lz + 1 >= n[2]) {
            continue;
          }
          // The voxel and its face neighbours analysed: no gradients across the material's
          // boundary, which runs along its surface and not along the fibres.
          if (mask[at(lx, ly, lz)] == 0 || mask[at(lx - 1, ly, lz)] == 0 ||
              mask[at(lx + 1, ly, lz)] == 0 || mask[at(lx, ly - 1, lz)] == 0 ||
              mask[at(lx, ly + 1, lz)] == 0 || mask[at(lx, ly, lz - 1)] == 0 ||
              mask[at(lx, ly, lz + 1)] == 0) {
            continue;
          }
          // Gradient in grey value per mm, so that non-cubic voxels give true directions.
          const Vec g{
              (grey[at(lx + 1, ly, lz)] - grey[at(lx - 1, ly, lz)]) / (2.0 * info.voxel_size[0]),
              (grey[at(lx, ly + 1, lz)] - grey[at(lx, ly - 1, lz)]) / (2.0 * info.voxel_size[1]),
              (grey[at(lx, ly, lz + 1)] - grey[at(lx, ly, lz - 1)]) / (2.0 * info.voxel_size[2])};
          for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t c = r; c < 3; ++c) {
              j[3 * r + c] += g[r] * g[c];
            }
          }
          ++count;
        }
      }
    }
    if (count < min_voxels) {
      continue;
    }
    j[3] = j[1];
    j[6] = j[2];
    j[7] = j[5];
    const auto [values, vectors] = symmetricEigen(j);
    OrientationBlock block;
    block.block = {bx, by, bz};
    block.direction = canonical(vectors[2]);
    block.coherence = values[1] > 0.0 ? std::clamp(1.0 - values[2] / values[1], 0.0, 1.0) : 0.0;
    block.voxels = count;
    found.push_back(block);
  }
  return found;
}

}  // namespace

OrientationResult analyzeOrientation(const Dataset& dataset, const OrientationOptions& options) {
  if (options.block_size < 4 || options.block_size > 128) {
    throw std::invalid_argument("block_size must be between 4 and 128");
  }
  if (options.families < 1 || options.families > 6) {
    throw std::invalid_argument("families must be between 1 and 6");
  }
  if (options.smoothing_sigma < 0.0 || options.smoothing_sigma > 4.0) {
    throw std::invalid_argument("smoothing_sigma must be between 0 and 4");
  }
  const DatasetInfo& info = dataset.info();
  if (options.materials != nullptr && options.materials->info().dims != info.dims) {
    throw std::invalid_argument("The material volume does not have the dataset's dimensions");
  }
  OrientationResult result;
  result.voxel_size = info.voxel_size;
  result.block_size = options.block_size;
  result.material = chosenMaterial(options);

  const std::int64_t b = options.block_size;
  const std::vector<float> kernel = gaussianKernel(options.smoothing_sigma);
  const Index3 blocks{(info.dims[0] + b - 1) / b, (info.dims[1] + b - 1) / b,
                      (info.dims[2] + b - 1) / b};
  // Blocks are analysed in slabs of one block row, in parallel.
  const std::int64_t rows = blocks[1] * blocks[2];

  std::optional<TelemetryPhase> phase(std::in_place, "structure tensor");
  tbb::combinable<std::vector<OrientationBlock>> found;
  tbb::parallel_for(tbb::blocked_range<std::int64_t>(0, rows), [&](const auto& range) {
    auto& local = found.local();
    for (std::int64_t row = range.begin(); row != range.end(); ++row) {
      const std::vector<OrientationBlock> row_blocks =
          analyzeRow(dataset, options, result.material, kernel, row % blocks[1], row / blocks[1]);
      local.insert(local.end(), row_blocks.begin(), row_blocks.end());
    }
  });
  found.combine_each([&](std::vector<OrientationBlock>& local) {
    result.blocks.insert(result.blocks.end(), local.begin(), local.end());
  });
  std::sort(result.blocks.begin(), result.blocks.end(),
            [](const OrientationBlock& l, const OrientationBlock& r) {
              return std::tie(l.block[2], l.block[1], l.block[0]) <
                     std::tie(r.block[2], r.block[1], r.block[0]);
            });
  phase.reset();

  const TelemetryPhase families_phase("fibre families");
  std::vector<std::size_t> coherent;
  double weight_sum = 0.0;
  double coherence_sum = 0.0;
  for (std::size_t i = 0; i < result.blocks.size(); ++i) {
    const OrientationBlock& block = result.blocks[i];
    result.analysed_voxels += block.voxels;
    coherence_sum += block.coherence * static_cast<double>(block.voxels);
    if (block.coherence < options.min_coherence) {
      continue;
    }
    coherent.push_back(i);
    const auto w = static_cast<double>(block.voxels);
    weight_sum += w;
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 3; ++c) {
        result.tensor[3 * r + c] += w * block.direction[r] * block.direction[c];
      }
    }
  }
  result.mean_coherence = result.analysed_voxels > 0
                              ? coherence_sum / static_cast<double>(result.analysed_voxels)
                              : 0.0;
  if (weight_sum <= 0.0) {
    return result;
  }
  for (double& v : result.tensor) {
    v /= weight_sum;
  }
  const auto [values, axes] = symmetricEigen(result.tensor);
  result.tensor_values = values;
  for (std::size_t i = 0; i < 3; ++i) {
    result.tensor_axes[i] = canonical(axes[i]);
  }

  std::vector<Vec> centres;
  const std::vector<int> family =
      groupDirections(result.blocks, coherent,
                      std::min<int>(options.families, static_cast<int>(coherent.size())), centres);
  result.families.resize(centres.size());
  std::vector<double> family_weight(centres.size(), 0.0);
  std::vector<double> family_angle(centres.size(), 0.0);
  for (std::size_t m = 0; m < coherent.size(); ++m) {
    OrientationBlock& block = result.blocks[coherent[m]];
    const auto f = static_cast<std::size_t>(family[m]);
    block.family = family[m] + 1;
    const auto w = static_cast<double>(block.voxels);
    family_weight[f] += w;
    family_angle[f] +=
        w * std::acos(std::clamp(std::abs(dot(block.direction, centres[f])), 0.0, 1.0));
    ++result.families[f].blocks;
  }
  for (std::size_t f = 0; f < centres.size(); ++f) {
    FibreFamily& out = result.families[f];
    out.direction = canonical(centres[f]);
    out.fraction = family_weight[f] / weight_sum;
    out.spread_deg = family_weight[f] > 0.0
                         ? family_angle[f] / family_weight[f] * 180.0 / std::numbers::pi
                         : 0.0;
  }
  // Largest family first, ids in that order.
  std::vector<std::size_t> order(centres.size());
  for (std::size_t f = 0; f < order.size(); ++f) {
    order[f] = f;
  }
  std::sort(order.begin(), order.end(), [&](std::size_t l, std::size_t r) {
    return result.families[l].fraction > result.families[r].fraction;
  });
  std::vector<FibreFamily> sorted;
  std::vector<int> new_id(centres.size(), 0);
  for (std::size_t i = 0; i < order.size(); ++i) {
    sorted.push_back(result.families[order[i]]);
    sorted.back().id = static_cast<int>(i) + 1;
    new_id[order[i]] = static_cast<int>(i) + 1;
  }
  result.families = std::move(sorted);
  for (OrientationBlock& block : result.blocks) {
    if (block.family > 0) {
      block.family = new_id[static_cast<std::size_t>(block.family - 1)];
    }
  }
  return result;
}

nlohmann::json toJson(const OrientationResult& result) {
  const auto angles = [](const Vec& p) {
    // Angle of the direction to each axis, in degrees.
    nlohmann::json a = nlohmann::json::array();
    for (const double c : p) {
      a.push_back(std::acos(std::clamp(std::abs(c), 0.0, 1.0)) * 180.0 / std::numbers::pi);
    }
    return a;
  };
  nlohmann::json families = nlohmann::json::array();
  for (const FibreFamily& f : result.families) {
    families.push_back({{"id", f.id},
                        {"direction", f.direction},
                        {"angles_to_axes_deg", angles(f.direction)},
                        {"fraction", f.fraction},
                        {"spread_deg", f.spread_deg},
                        {"blocks", f.blocks}});
  }
  return {{"voxel_size_mm", result.voxel_size},
          {"block_size", result.block_size},
          {"material", result.material},
          {"analysed_voxels", result.analysed_voxels},
          {"analysed_blocks", result.blocks.size()},
          {"mean_coherence", result.mean_coherence},
          {"orientation_tensor", result.tensor},
          {"tensor_eigenvalues", result.tensor_values},
          {"tensor_axes", result.tensor_axes},
          {"families", families}};
}

void writeOrientationResult(const Dataset& dataset, const OrientationResult& result,
                            const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
  writeJson(dir / "orientation.json", toJson(result));
  const DatasetInfo& info = dataset.info();
  const std::int64_t b = result.block_size;

  // Grids at block resolution: block i covers level-0 voxels [i * b, (i + 1) * b).
  initializeVdb();
  auto direction = openvdb::Vec3SGrid::create(openvdb::Vec3s(0.0F));
  auto coherence = openvdb::FloatGrid::create(0.0F);
  auto family = openvdb::Int32Grid::create(0);
  auto direction_acc = direction->getAccessor();
  auto coherence_acc = coherence->getAccessor();
  auto family_acc = family->getAccessor();
  for (const OrientationBlock& block : result.blocks) {
    const openvdb::Coord c(static_cast<int>(block.block[0]), static_cast<int>(block.block[1]),
                           static_cast<int>(block.block[2]));
    direction_acc.setValue(c, openvdb::Vec3s(static_cast<float>(block.direction[0]),
                                             static_cast<float>(block.direction[1]),
                                             static_cast<float>(block.direction[2])));
    coherence_acc.setValue(c, static_cast<float>(block.coherence));
    family_acc.setValue(c, block.family);
  }
  const openvdb::Vec3d pitch(info.voxel_size[0] * static_cast<double>(b),
                             info.voxel_size[1] * static_cast<double>(b),
                             info.voxel_size[2] * static_cast<double>(b));
  auto transform =
      std::make_shared<openvdb::math::Transform>(std::make_shared<openvdb::math::ScaleMap>(pitch));
  transform->postTranslate(
      openvdb::Vec3d(info.voxel_size[0], info.voxel_size[1], info.voxel_size[2]) *
      ((static_cast<double>(b) - 1.0) / 2.0));
  direction->setName("direction");
  coherence->setName("coherence");
  family->setName("family");
  for (const openvdb::GridBase::Ptr& grid :
       std::initializer_list<openvdb::GridBase::Ptr>{direction, coherence, family}) {
    grid->setTransform(transform);
  }
  writeVdb(dir / "orientation.vdb", {direction, coherence, family});

  // Slices through the middle along each axis: grey values with the blocks in the colour of
  // their direction, faint where the structure is barely directional.
  std::map<Index3, const OrientationBlock*> by_index;
  for (const OrientationBlock& block : result.blocks) {
    by_index[block.block] = &block;
  }
  constexpr std::array<char, 3> kNames{'x', 'y', 'z'};
  for (int axis = 0; axis < 3; ++axis) {
    const int u = axis == 0 ? 1 : 0;
    const int v = axis == 2 ? 1 : 2;
    const auto ua = static_cast<std::size_t>(u);
    const auto va = static_cast<std::size_t>(v);
    const auto aa = static_cast<std::size_t>(axis);
    Box slice;
    slice.min = {0, 0, 0};
    slice.max = info.dims;
    slice.min[aa] = info.dims[aa] / 2;
    slice.max[aa] = slice.min[aa] + 1;
    std::vector<float> grey(static_cast<std::size_t>(slice.voxelCount()));
    dataset.readRegion(0, slice, grey, info.air_level);
    const float lo = info.air_level;
    float hi = lo + 1.0F;
    for (const float g : grey) {
      hi = std::max(hi, g);
    }
    const std::int64_t width = info.dims[ua];
    const std::int64_t height = info.dims[va];
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width * height * 3));
    for (std::int64_t y = 0; y < height; ++y) {
      for (std::int64_t x = 0; x < width; ++x) {
        Index3 p{};
        p[ua] = x;
        p[va] = y;
        p[aa] = slice.min[aa];
        const Index3 local{p[0] - slice.min[0], p[1] - slice.min[1], p[2] - slice.min[2]};
        const float g = grey[static_cast<std::size_t>(
            local[0] + slice.size(0) * (local[1] + slice.size(1) * local[2]))];
        const double level = std::clamp((g - lo) / (hi - lo), 0.0F, 1.0F) * 255.0;
        std::array<double, 3> colour{level, level, level};
        if (const auto it = by_index.find({p[0] / b, p[1] / b, p[2] / b}); it != by_index.end()) {
          const OrientationBlock& block = *it->second;
          const double alpha = 0.65 * std::clamp(block.coherence * 2.0, 0.2, 1.0);
          for (std::size_t c = 0; c < 3; ++c) {
            const double tint = 255.0 * std::abs(block.direction[c]);
            colour[c] = (1.0 - alpha) * colour[c] + alpha * tint * level / 255.0 * 1.4;
          }
        }
        const auto out = static_cast<std::size_t>((x + width * y) * 3);
        for (std::size_t c = 0; c < 3; ++c) {
          rgb[out + c] = static_cast<std::uint8_t>(std::clamp(colour[c], 0.0, 255.0));
        }
      }
    }
    detail::writeRgbPng(dir / (std::string("orientation_") + kNames[aa] + ".png"),
                        static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), rgb,
                        info.voxel_size[ua], info.voxel_size[va]);
  }
}

}  // namespace voxelsieve
