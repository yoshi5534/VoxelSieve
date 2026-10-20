#include "voxelsieve/cells.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

#include "detail/noise.hpp"

namespace voxelsieve {
namespace {

using Vec = std::array<double, 3>;

bool inside(const CellSphere& sphere, const Vec& p) {
  double d2 = 0.0;
  for (std::size_t a = 0; a < 3; ++a) {
    const double d = p[a] - sphere.center_mm[a];
    d2 += d * d;
  }
  return d2 <= sphere.radius_mm * sphere.radius_mm;
}

/// Angle of (x, y) around z in degrees, 0 to 360.
double angleDeg(double x, double y) {
  double deg = std::atan2(y, x) * 180.0 / std::numbers::pi;
  if (deg < 0.0) {
    deg += 360.0;
  }
  return deg;
}

bool inRange(double deg, double from, double to) {
  if (to - from >= 360.0) {
    return true;
  }
  const double span = std::fmod(std::fmod(to - from, 360.0) + 360.0, 360.0);
  const double offset = std::fmod(std::fmod(deg - from, 360.0) + 360.0, 360.0);
  return offset <= span;
}

/// How far the cathode edge of `layer` at `edge` reaches beyond its nominal position.
double extension(const CellSpec& spec, int layer, int edge, double deg) {
  double length = 0.0;
  for (const CathodeExtension& e : spec.extensions) {
    if (e.layer == layer && e.edge == edge &&
        (spec.type == CellType::kStacked || inRange(deg, e.from_deg, e.to_deg))) {
      length += e.length_mm;
    }
  }
  return length;
}

double stackHeightMm(const CellSpec& spec) {
  return spec.cathode_layers * cellPitchMm(spec) + spec.anode_thickness_mm + spec.gap_mm;
}

/// Low and high end of the anode sheets of a stacked cell along x and y.
std::array<double, 4> anodeFootprint(const CellSpec& spec) {
  return {-0.5 * spec.cathode_size_mm[0] - spec.overhang_mm[0],
          0.5 * spec.cathode_size_mm[0] + spec.overhang_mm[1],
          -0.5 * spec.cathode_size_mm[1] - spec.overhang_mm[2],
          0.5 * spec.cathode_size_mm[1] + spec.overhang_mm[3]};
}

double cylinderHalfLengthMm(const CellSpec& spec) {
  return 0.5 * spec.cathode_height_mm + std::max(spec.overhang_mm[0], spec.overhang_mm[1]) +
         spec.housing_gap_mm + spec.housing_wall_mm;
}

std::uint16_t cylindricalValue(const CellSpec& spec, const Vec& p) {
  const double x = p[0] / (1.0 + spec.ovality);
  const double y = p[1];
  const double z = p[2];
  const double r = std::hypot(x, y);
  const double outer = cellOuterRadiusMm(spec);
  const double half = cylinderHalfLengthMm(spec);
  if (r > outer || std::abs(z) > half) {
    return spec.air_value;
  }
  if (r > outer - spec.housing_wall_mm || std::abs(z) > half - spec.housing_wall_mm) {
    return spec.housing_value;
  }
  for (const CellSphere& s : spec.particles) {
    if (inside(s, p)) {
      return spec.particle_value;
    }
  }
  for (const CellSphere& s : spec.voids) {
    if (inside(s, p)) {
      return spec.air_value;
    }
  }
  if (r < spec.core_radius_mm) {
    return spec.air_value;
  }
  const double pitch = cellPitchMm(spec);
  const double deg = angleDeg(x, y);
  const double s = (r - spec.core_radius_mm) / pitch - deg / 360.0;
  const double turn = std::floor(s);
  const double unrolled = deg / 360.0 + turn;  // turns of the strip up to this point
  if (unrolled < 0.0 || unrolled > spec.turns) {
    return spec.electrolyte_value;
  }
  const double f = (s - turn) * pitch;
  const auto layer = static_cast<int>(turn);
  const double h = 0.5 * spec.cathode_height_mm;
  if (f < spec.cathode_thickness_mm) {
    const bool on =
        z >= -h - extension(spec, layer, 0, deg) && z <= h + extension(spec, layer, 1, deg);
    return on ? spec.cathode_value : spec.electrolyte_value;
  }
  const double anode_from = spec.cathode_thickness_mm + spec.separator_thickness_mm;
  if (f >= anode_from && f < anode_from + spec.anode_thickness_mm) {
    const bool on = z >= -h - spec.overhang_mm[0] && z <= h + spec.overhang_mm[1];
    return on ? spec.anode_value : spec.electrolyte_value;
  }
  return std::abs(z) <= h ? spec.separator_value.value_or(spec.electrolyte_value)
                          : spec.electrolyte_value;
}

std::uint16_t stackedValue(const CellSpec& spec, const Vec& p) {
  const std::array<double, 4> anode = anodeFootprint(spec);
  const double margin = spec.housing_gap_mm + spec.housing_wall_mm;
  const double height = stackHeightMm(spec);
  const std::array<double, 6> outer{anode[0] - margin,      anode[1] + margin,
                                    anode[2] - margin,      anode[3] + margin,
                                    -0.5 * height - margin, 0.5 * height + margin};
  for (std::size_t a = 0; a < 3; ++a) {
    if (p[a] < outer[2 * a] || p[a] > outer[2 * a + 1]) {
      return spec.air_value;
    }
  }
  for (std::size_t a = 0; a < 3; ++a) {
    if (p[a] < outer[2 * a] + spec.housing_wall_mm ||
        p[a] > outer[2 * a + 1] - spec.housing_wall_mm) {
      return spec.housing_value;
    }
  }
  for (const CellSphere& s : spec.particles) {
    if (inside(s, p)) {
      return spec.particle_value;
    }
  }
  for (const CellSphere& s : spec.voids) {
    if (inside(s, p)) {
      return spec.air_value;
    }
  }
  double f = p[2] + 0.5 * height;
  if (f < 0.0 || f > height) {
    return spec.electrolyte_value;
  }
  const double pitch = cellPitchMm(spec);
  if (spec.gap_mm > 0.0) {
    const double gap = (spec.gap_after + 1) * pitch;
    if (f >= gap && f < gap + spec.gap_mm) {
      return spec.electrolyte_value;
    }
    f -= f >= gap ? spec.gap_mm : 0.0;
  }
  const double period = std::floor(f / pitch);
  const double g = f - period * pitch;
  const auto layer = static_cast<int>(period);
  if (g < spec.anode_thickness_mm) {
    const bool on = p[0] >= anode[0] && p[0] <= anode[1] && p[1] >= anode[2] && p[1] <= anode[3];
    return on ? spec.anode_value : spec.electrolyte_value;
  }
  const double cathode_from = spec.anode_thickness_mm + spec.separator_thickness_mm;
  if (layer < spec.cathode_layers && g >= cathode_from &&
      g < cathode_from + spec.cathode_thickness_mm) {
    const double hx = 0.5 * spec.cathode_size_mm[0];
    const double hy = 0.5 * spec.cathode_size_mm[1];
    const bool on = p[0] >= -hx - extension(spec, layer, 0, 0.0) &&
                    p[0] <= hx + extension(spec, layer, 1, 0.0) &&
                    p[1] >= -hy - extension(spec, layer, 2, 0.0) &&
                    p[1] <= hy + extension(spec, layer, 3, 0.0);
    return on ? spec.cathode_value : spec.electrolyte_value;
  }
  const bool facing = std::abs(p[0]) <= 0.5 * spec.cathode_size_mm[0] &&
                      std::abs(p[1]) <= 0.5 * spec.cathode_size_mm[1];
  return facing ? spec.separator_value.value_or(spec.electrolyte_value) : spec.electrolyte_value;
}

nlohmann::json spheresJson(const std::vector<CellSphere>& spheres) {
  nlohmann::json list = nlohmann::json::array();
  for (const CellSphere& s : spheres) {
    list.push_back({{"center_mm", s.center_mm},
                    {"radius_mm", s.radius_mm},
                    {"volume_mm3", 4.0 / 3.0 * std::numbers::pi * std::pow(s.radius_mm, 3)}});
  }
  return list;
}

}  // namespace

CellSpec defaultCylindricalCell() { return {}; }

CellSpec defaultStackedCell() {
  CellSpec spec;
  spec.type = CellType::kStacked;
  spec.dims = {200, 176, 104};
  return spec;
}

double cellPitchMm(const CellSpec& spec) {
  return spec.cathode_thickness_mm + spec.anode_thickness_mm + 2.0 * spec.separator_thickness_mm;
}

double cellOuterRadiusMm(const CellSpec& spec) {
  // The last turn reaches one pitch further out where the strip ends.
  return spec.core_radius_mm + (spec.turns + 1) * cellPitchMm(spec) + spec.housing_gap_mm +
         spec.housing_wall_mm;
}

std::array<double, 3> cellOuterSizeMm(const CellSpec& spec) {
  const double margin = spec.housing_gap_mm + spec.housing_wall_mm;
  if (spec.type == CellType::kCylindrical) {
    const double d = 2.0 * cellOuterRadiusMm(spec);
    return {d * (1.0 + spec.ovality), d, 2.0 * cylinderHalfLengthMm(spec)};
  }
  const std::array<double, 4> anode = anodeFootprint(spec);
  return {anode[1] - anode[0] + 2.0 * margin, anode[3] - anode[2] + 2.0 * margin,
          stackHeightMm(spec) + 2.0 * margin};
}

std::uint16_t cellValueAt(const CellSpec& spec, const std::array<double, 3>& point_mm) {
  return spec.type == CellType::kCylindrical ? cylindricalValue(spec, point_mm)
                                             : stackedValue(spec, point_mm);
}

double cellOverhangMm(const CellSpec& spec, int layer, int edge, double deg) {
  if (edge < 0 || edge > 3 || (spec.type == CellType::kCylindrical && edge > 1)) {
    throw std::invalid_argument("No edge " + std::to_string(edge) + " of this cell");
  }
  return spec.overhang_mm[static_cast<std::size_t>(edge)] - extension(spec, layer, edge, deg);
}

nlohmann::json cellGroundTruth(const CellSpec& spec) {
  const bool cylindrical = spec.type == CellType::kCylindrical;
  const int layers = cylindrical ? spec.turns : spec.cathode_layers;
  const int edges = cylindrical ? 2 : 4;
  nlohmann::json overhang = nlohmann::json::array();
  for (int edge = 0; edge < edges; ++edge) {
    double smallest = std::numeric_limits<double>::infinity();
    for (int layer = 0; layer < layers; ++layer) {
      smallest = std::min(smallest, cellOverhangMm(spec, layer, edge, 0.0));
      for (const CathodeExtension& e : spec.extensions) {
        if (e.layer == layer && e.edge == edge) {
          smallest = std::min(smallest, cellOverhangMm(spec, layer, edge, e.from_deg));
        }
      }
    }
    overhang.push_back({{"edge", edge},
                        {"nominal_mm", spec.overhang_mm[static_cast<std::size_t>(edge)]},
                        {"min_mm", smallest}});
  }
  nlohmann::json truth = {{"type", cylindrical ? "cylindrical" : "stacked"},
                          {"dims", spec.dims},
                          {"voxel_size_mm", spec.voxel_size},
                          {"pitch_mm", cellPitchMm(spec)},
                          {"cathode_layers", layers},
                          {"anode_layers", cylindrical ? spec.turns : spec.cathode_layers + 1},
                          {"outer_size_mm", cellOuterSizeMm(spec)},
                          {"housing_wall_mm", spec.housing_wall_mm},
                          {"overhang", overhang},
                          {"particles", spheresJson(spec.particles)},
                          {"voids", spheresJson(spec.voids)}};
  if (cylindrical) {
    truth["outer_radius_mm"] = cellOuterRadiusMm(spec);
    truth["core_radius_mm"] = spec.core_radius_mm;
    truth["cathode_height_mm"] = spec.cathode_height_mm;
    truth["ovality"] = spec.ovality;
  } else {
    truth["cathode_size_mm"] = spec.cathode_size_mm;
    truth["stack_height_mm"] = stackHeightMm(spec);
    if (spec.gap_mm > 0.0) {
      truth["gap"] = {{"after_layer", spec.gap_after}, {"thickness_mm", spec.gap_mm}};
    }
  }
  return truth;
}

CellSource::CellSource(CellSpec spec) : spec_(std::move(spec)) {
  if (spec_.supersampling < 1 || spec_.supersampling > 8) {
    throw std::invalid_argument("Supersampling must be 1 to 8");
  }
  if (spec_.turns < 1 || spec_.cathode_layers < 1) {
    throw std::invalid_argument("A cell needs at least one turn or cathode layer");
  }
}

void CellSource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  if (static_cast<std::int64_t>(out.size()) != box.voxelCount()) {
    throw std::invalid_argument("Region and buffer size differ");
  }
  const int n = spec_.supersampling;
  const auto samples = static_cast<double>(n * n * n);
  std::size_t offset = 0;
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
      for (std::int64_t x = box.min[0]; x < box.max[0]; ++x) {
        const std::array<std::int64_t, 3> index{x, y, z};
        double sum = 0.0;
        for (int k = 0; k < n * n * n; ++k) {
          const std::array<int, 3> sub{k % n, (k / n) % n, k / (n * n)};
          Vec p{};
          for (std::size_t a = 0; a < 3; ++a) {
            const double frac = (sub[a] + 0.5) / n;
            p[a] =
                (static_cast<double>(index[a]) + frac - 0.5 * static_cast<double>(spec_.dims[a])) *
                spec_.voxel_size[a];
          }
          sum += cellValueAt(spec_, p);
        }
        double value = sum / samples;
        if (spec_.noise_sigma > 0.0) {
          const auto linear =
              static_cast<std::uint64_t>(x + spec_.dims[0] * (y + spec_.dims[1] * z));
          value += spec_.noise_sigma * detail::gaussianNoise(spec_.seed, linear);
        }
        out[offset++] = static_cast<std::uint16_t>(std::clamp(std::round(value), 0.0, 65535.0));
      }
    }
  }
}

}  // namespace voxelsieve
