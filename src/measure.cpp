#include "voxelsieve/measure.hpp"

#include <openvdb/tools/VolumeToMesh.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "voxelsieve/telemetry.hpp"
#include "voxelsieve/vdb.hpp"

namespace voxelsieve {
namespace {

using Mat3 = std::array<std::array<double, 3>, 3>;

std::int64_t ceilDiv(std::int64_t a, std::int64_t b) { return (a + b - 1) / b; }

/// Eigenvalues (descending) and unit eigenvectors (rows) of a symmetric 3x3 matrix, by Jacobi
/// rotations.
std::pair<std::array<double, 3>, Mat3> eigenSymmetric(Mat3 a) {
  Mat3 v{{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};
  for (int sweep = 0; sweep < 50; ++sweep) {
    const double off = std::abs(a[0][1]) + std::abs(a[0][2]) + std::abs(a[1][2]);
    if (off < 1e-15 * (std::abs(a[0][0]) + std::abs(a[1][1]) + std::abs(a[2][2]) + 1e-300)) {
      break;
    }
    for (std::size_t p = 0; p < 2; ++p) {
      for (std::size_t q = p + 1; q < 3; ++q) {
        if (a[p][q] == 0.0) {
          continue;
        }
        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double t =
            (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;
        for (std::size_t k = 0; k < 3; ++k) {
          const double akp = a[k][p];
          const double akq = a[k][q];
          a[k][p] = c * akp - s * akq;
          a[k][q] = s * akp + c * akq;
        }
        for (std::size_t k = 0; k < 3; ++k) {
          const double apk = a[p][k];
          const double aqk = a[q][k];
          a[p][k] = c * apk - s * aqk;
          a[q][k] = s * apk + c * aqk;
        }
        for (std::size_t k = 0; k < 3; ++k) {
          const double vkp = v[k][p];
          const double vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq;
          v[k][q] = s * vkp + c * vkq;
        }
      }
    }
  }
  std::array<std::size_t, 3> order{0, 1, 2};
  std::ranges::sort(order, [&](std::size_t i, std::size_t j) { return a[i][i] > a[j][j]; });
  std::array<double, 3> values{};
  Mat3 vectors{};
  for (std::size_t i = 0; i < 3; ++i) {
    values[i] = a[order[i]][order[i]];
    for (std::size_t k = 0; k < 3; ++k) {
      vectors[i][k] = v[k][order[i]];
    }
  }
  return {values, vectors};
}

/// Running sums of one object while it is flood-filled, in sample indices.
struct Accumulator {
  int material = 0;
  std::int64_t samples = 0;
  std::array<double, 3> sum{};
  Mat3 products{};  // sums of i_a * i_b
  std::array<std::int64_t, 3> low{std::numeric_limits<std::int64_t>::max(),
                                  std::numeric_limits<std::int64_t>::max(),
                                  std::numeric_limits<std::int64_t>::max()};
  std::array<std::int64_t, 3> high{-1, -1, -1};
  bool touches_border = false;
};

/// Measures the objects in `ids`, the labels sampled at every `stride`th voxel of a volume of
/// `dims` voxels.
MeasureInfo measureSamples(std::span<const std::uint8_t> ids,
                           const std::array<std::int64_t, 3>& dims, std::int64_t stride,
                           const VoxelSize& voxel_size, const std::vector<Material>& materials,
                           const MeasureOptions& options);

}  // namespace

MeasureInfo measureObjects(const MaterialVolume& volume, const MeasureOptions& options) {
  const MaterialVolumeInfo& info = volume.info();
  const std::int64_t longest = *std::ranges::max_element(info.dims);
  const auto samples = [&](std::int64_t s) {
    return ceilDiv(info.dims[0], s) * ceilDiv(info.dims[1], s) * ceilDiv(info.dims[2], s);
  };
  std::int64_t stride = 1;
  while (samples(stride) > options.max_samples && stride < longest) {
    stride *= 2;
  }
  std::vector<std::uint8_t> ids(static_cast<std::size_t>(samples(stride)));
  volume.readRegion({{0, 0, 0}, info.dims}, ids, stride);
  return measureSamples(ids, info.dims, stride, info.voxel_size, info.materials, options);
}

MeasureInfo measureObjects(std::span<const std::uint8_t> labels,
                           const std::array<std::int64_t, 3>& dims, const VoxelSize& voxel_size,
                           const std::vector<Material>& materials, const MeasureOptions& options) {
  if (static_cast<std::int64_t>(labels.size()) != dims[0] * dims[1] * dims[2]) {
    throw std::invalid_argument("The labels do not fill the volume");
  }
  return measureSamples(labels, dims, 1, voxel_size, materials, options);
}

namespace {

MeasureInfo measureSamples(std::span<const std::uint8_t> ids,
                           const std::array<std::int64_t, 3>& dims, std::int64_t stride,
                           const VoxelSize& voxel_size, const std::vector<Material>& materials,
                           const MeasureOptions& options) {
  if (options.max_objects < 1) {
    throw std::invalid_argument("max_objects must be at least 1");
  }
  if (options.min_voxels < 1) {
    throw std::invalid_argument("min_voxels must be at least 1");
  }
  std::array<bool, 256> wanted{};
  if (options.material_ids.empty()) {
    for (const Material& material : materials) {
      wanted[static_cast<std::size_t>(material.id)] = true;
    }
  }
  for (const int id : options.material_ids) {
    if (id != 0 &&
        std::ranges::none_of(materials, [id](const Material& m) { return m.id == id; })) {
      throw std::invalid_argument("The segmentation has no material " + std::to_string(id));
    }
    wanted[static_cast<std::size_t>(id)] = true;
  }
  std::array<double, 3> axis{};
  if (options.axis) {
    // A direction in voxel axes, measured in mm: scaled by the pitch so it stays the same line.
    const auto& d = *options.axis;
    const double length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(length > 0.0)) {
      throw std::invalid_argument("The measuring axis must not be zero");
    }
    axis = {d[0] / length, d[1] / length, d[2] / length};
  }

  MeasureInfo result;
  result.stride = stride;
  const std::array<std::int64_t, 3> n{ceilDiv(dims[0], stride), ceilDiv(dims[1], stride),
                                      ceilDiv(dims[2], stride)};
  const auto count = static_cast<std::size_t>(n[0] * n[1] * n[2]);

  // Flood fill of every wanted material, one object at a time.
  std::vector<std::int32_t> labels(count, 0);
  std::vector<Accumulator> found;
  {
    const TelemetryPhase phase("objects");
    std::vector<std::size_t> stack;
    for (std::size_t start = 0; start < count; ++start) {
      if (labels[start] != 0 || !wanted[ids[start]]) {
        continue;
      }
      if (found.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max() - 1)) {
        throw std::runtime_error("Too many objects to measure");
      }
      const auto label = static_cast<std::int32_t>(found.size() + 1);
      Accumulator object;
      object.material = ids[start];
      labels[start] = label;
      stack.push_back(start);
      while (!stack.empty()) {
        const std::size_t i = stack.back();
        stack.pop_back();
        const auto index = static_cast<std::int64_t>(i);
        const std::array<std::int64_t, 3> p{index % n[0], (index / n[0]) % n[1],
                                            index / (n[0] * n[1])};
        ++object.samples;
        for (std::size_t a = 0; a < 3; ++a) {
          const auto pa = static_cast<double>(p[a]);
          object.sum[a] += pa;
          for (std::size_t b = 0; b < 3; ++b) {
            object.products[a][b] += pa * static_cast<double>(p[b]);
          }
          object.low[a] = std::min(object.low[a], p[a]);
          object.high[a] = std::max(object.high[a], p[a]);
        }
        const std::array<std::int64_t, 3> step{1, n[0], n[0] * n[1]};
        for (std::size_t a = 0; a < 3; ++a) {
          for (const int direction : {-1, 1}) {
            const std::int64_t q = p[a] + direction;
            if (q < 0 || q >= n[a]) {
              object.touches_border = true;
              continue;
            }
            const auto j = static_cast<std::size_t>(index + direction * step[a]);
            if (labels[j] == 0 && ids[j] == object.material) {
              labels[j] = label;
              stack.push_back(j);
            }
          }
        }
      }
      found.push_back(object);
    }
  }
  result.objects_found = static_cast<std::int64_t>(found.size());

  // The objects kept: not outside air, large enough, the largest first.
  const std::int64_t per_sample = stride * stride * stride;
  std::vector<std::size_t> kept;
  for (std::size_t k = 0; k < found.size(); ++k) {
    const Accumulator& object = found[k];
    if (object.material == 0 && object.touches_border) {
      continue;  // the air around the part
    }
    if (object.samples * per_sample >= options.min_voxels) {
      kept.push_back(k);
    }
  }
  std::ranges::stable_sort(
      kept, [&](std::size_t a, std::size_t b) { return found[a].samples > found[b].samples; });
  if (kept.size() > static_cast<std::size_t>(options.max_objects)) {
    kept.resize(static_cast<std::size_t>(options.max_objects));
  }
  std::vector<std::int32_t> slot(found.size() + 1, -1);  // label -> index in kept
  for (std::size_t k = 0; k < kept.size(); ++k) {
    slot[kept[k] + 1] = static_cast<std::int32_t>(k);
  }

  // Sample index -> mm: samples stand for the middle of their stride^3 block of voxels.
  const auto mm = [&](std::size_t a, double sample) {
    return (sample * static_cast<double>(stride) + 0.5 * static_cast<double>(stride - 1)) *
           voxel_size[a];
  };
  const double voxel_mm3 = voxel_size[0] * voxel_size[1] * voxel_size[2];
  result.objects.resize(kept.size());
  for (std::size_t k = 0; k < kept.size(); ++k) {
    const Accumulator& object = found[kept[k]];
    MeasuredObject& out = result.objects[k];
    out.material = object.material;
    out.material_name = "Enclosed air";
    for (const Material& material : materials) {
      if (material.id == object.material) {
        out.material_name = material.name;
      }
    }
    out.voxels = object.samples * per_sample;
    out.volume_mm3 = static_cast<double>(out.voxels) * voxel_mm3;
    out.touches_border = object.touches_border;
    const auto m = static_cast<double>(object.samples);
    std::array<double, 3> mean{};
    for (std::size_t a = 0; a < 3; ++a) {
      mean[a] = object.sum[a] / m;
      out.centroid_mm[a] = mm(a, mean[a]);
      // The faces of the outermost voxels, not their centres.
      const double half = 0.5 * static_cast<double>(stride) * voxel_size[a];
      out.min_mm[a] = mm(a, static_cast<double>(object.low[a])) - half;
      out.max_mm[a] = mm(a, static_cast<double>(object.high[a])) + half;
    }
    // Covariance in mm^2, then its principal axes.
    Mat3 covariance{};
    for (std::size_t a = 0; a < 3; ++a) {
      for (std::size_t b = 0; b < 3; ++b) {
        const double samples_cov = object.products[a][b] / m - mean[a] * mean[b];
        covariance[a][b] =
            samples_cov * static_cast<double>(stride * stride) * voxel_size[a] * voxel_size[b];
      }
    }
    out.principal_axes = eigenSymmetric(covariance).second;
  }

  // Lengths: the spread of the voxels along the principal axes and the given axis.
  {
    const TelemetryPhase phase("lengths");
    constexpr double kInf = std::numeric_limits<double>::infinity();
    std::vector<std::array<double, 8>> spans(kept.size(),
                                             {kInf, -kInf, kInf, -kInf, kInf, -kInf, kInf, -kInf});
    for (std::size_t i = 0; i < count; ++i) {
      if (labels[i] == 0) {
        continue;
      }
      const std::int32_t k = slot[static_cast<std::size_t>(labels[i])];
      if (k < 0) {
        continue;
      }
      const auto index = static_cast<std::int64_t>(i);
      const std::int64_t x = index % n[0];
      const std::int64_t y = (index / n[0]) % n[1];
      const std::int64_t z = index / (n[0] * n[1]);
      const std::array<double, 3> p{mm(0, static_cast<double>(x)), mm(1, static_cast<double>(y)),
                                    mm(2, static_cast<double>(z))};
      auto& span = spans[static_cast<std::size_t>(k)];
      const MeasuredObject& object = result.objects[static_cast<std::size_t>(k)];
      for (std::size_t e = 0; e < 4; ++e) {
        const std::array<double, 3>& direction = e < 3 ? object.principal_axes[e] : axis;
        const double t = direction[0] * p[0] + direction[1] * p[1] + direction[2] * p[2];
        span[2 * e] = std::min(span[2 * e], t);
        span[2 * e + 1] = std::max(span[2 * e + 1], t);
      }
    }
    for (std::size_t k = 0; k < kept.size(); ++k) {
      MeasuredObject& object = result.objects[k];
      const auto& span = spans[k];
      // Plus the extent of one sample along the direction, so a single voxel has its size.
      const auto thickness = [&](const std::array<double, 3>& direction) {
        double sum = 0.0;
        for (std::size_t a = 0; a < 3; ++a) {
          sum += std::abs(direction[a]) * static_cast<double>(stride) * voxel_size[a];
        }
        return sum;
      };
      for (std::size_t e = 0; e < 3; ++e) {
        object.principal_lengths_mm[e] =
            span[2 * e + 1] - span[2 * e] + thickness(object.principal_axes[e]);
      }
      if (options.axis) {
        object.axis_length_mm = span[7] - span[6] + thickness(axis);
      }
    }
  }

  // Surfaces: marching cubes on each object, in mm.
  {
    const TelemetryPhase phase("surfaces");
    for (std::size_t k = 0; k < kept.size(); ++k) {
      const Accumulator& object = found[kept[k]];
      const auto label = static_cast<std::int32_t>(kept[k] + 1);
      auto grid = openvdb::FloatGrid::create(1.0F);
      auto accessor = grid->getAccessor();
      for (std::int64_t z = object.low[2]; z <= object.high[2]; ++z) {
        for (std::int64_t y = object.low[1]; y <= object.high[1]; ++y) {
          for (std::int64_t x = object.low[0]; x <= object.high[0]; ++x) {
            if (labels[static_cast<std::size_t>((z * n[1] + y) * n[0] + x)] == label) {
              accessor.setValue(
                  openvdb::Coord(static_cast<openvdb::Int32>(x), static_cast<openvdb::Int32>(y),
                                 static_cast<openvdb::Int32>(z)),
                  0.0F);
            }
          }
        }
      }
      std::vector<openvdb::Vec3s> points;
      std::vector<openvdb::Vec3I> triangles;
      std::vector<openvdb::Vec4I> quads;
      openvdb::tools::volumeToMesh(*grid, points, triangles, quads, 0.5);
      const auto scaled = [&](const openvdb::Vec3s& q) {
        return std::array<double, 3>{
            static_cast<double>(q[0]) * static_cast<double>(stride) * voxel_size[0],
            static_cast<double>(q[1]) * static_cast<double>(stride) * voxel_size[1],
            static_cast<double>(q[2]) * static_cast<double>(stride) * voxel_size[2]};
      };
      const auto area = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        const auto pa = scaled(points[a]);
        const auto pb = scaled(points[b]);
        const auto pc = scaled(points[c]);
        const std::array<double, 3> u{pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        const std::array<double, 3> v{pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        const std::array<double, 3> w{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                                      u[0] * v[1] - u[1] * v[0]};
        return 0.5 * std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
      };
      double sum = 0.0;
      for (const auto& t : triangles) {
        sum += area(t[0], t[1], t[2]);
      }
      for (const auto& q : quads) {
        sum += area(q[0], q[1], q[2]) + area(q[0], q[2], q[3]);
      }
      result.objects[k].surface_mm2 = sum;
    }
  }
  return result;
}

}  // namespace

void writeObjectsCsv(const std::filesystem::path& file, const MeasureInfo& info) {
  std::ofstream out(file);
  if (!out) {
    throw std::runtime_error("Cannot write " + file.string());
  }
  out << std::setprecision(8);
  out << "object,material,material_name,voxels,volume_mm3,surface_mm2,centroid_x_mm,centroid_y_mm,"
         "centroid_z_mm,min_x_mm,min_y_mm,min_z_mm,max_x_mm,max_y_mm,max_z_mm,length_1_mm,"
         "length_2_mm,length_3_mm,axis_1_x,axis_1_y,axis_1_z,axis_length_mm,touches_border\n";
  for (std::size_t k = 0; k < info.objects.size(); ++k) {
    const MeasuredObject& object = info.objects[k];
    std::string name = object.material_name;
    std::ranges::replace(name, '"', '\'');
    out << k + 1 << ',' << object.material << ",\"" << name << "\"," << object.voxels << ','
        << object.volume_mm3 << ',' << object.surface_mm2;
    for (const auto* values : {&object.centroid_mm, &object.min_mm, &object.max_mm,
                               &object.principal_lengths_mm, &object.principal_axes[0]}) {
      for (const double value : *values) {
        out << ',' << value;
      }
    }
    out << ',';
    if (object.axis_length_mm) {
      out << *object.axis_length_mm;
    }
    out << ',' << (object.touches_border ? "true" : "false") << '\n';
  }
}

}  // namespace voxelsieve
