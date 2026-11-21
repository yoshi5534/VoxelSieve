#include "voxelsieve/battery.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include "detail/battery.hpp"
#include "detail/png.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {

using namespace detail::battery;

namespace {

/// Bounding box, cell type and axis from a coarse level.
Overview overview(const Dataset& dataset, const BatteryOptions& options,
                  std::vector<std::string>& warnings) {
  const DatasetInfo& info = dataset.info();
  // A level of at least 4³ voxels per voxel, so that noise in the air does not reach the
  // threshold, and at most 256 voxels along any axis.
  int level = 0;
  for (const LevelInfo& l : info.levels) {
    level = l.level;
    if (l.level >= 2 && *std::max_element(l.dims.begin(), l.dims.end()) <= 256) {
      break;
    }
  }
  const Index3 dims = dataset.level(level).dims;
  std::vector<float> values(at(dims[0] * dims[1] * dims[2]));
  dataset.readRegion(level, Box{{0, 0, 0}, dims}, values, info.air_level);
  const auto idx = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
    return at(x + dims[0] * (y + dims[1] * z));
  };
  // Bounding box from the material per slice along each axis, ignoring slices with a few
  // stray voxels.
  std::array<std::vector<std::int64_t>, 3> counts;
  for (std::size_t a = 0; a < 3; ++a) {
    counts[a].assign(at(dims[a]), 0);
  }
  for (std::int64_t z = 0; z < dims[2]; ++z) {
    for (std::int64_t y = 0; y < dims[1]; ++y) {
      for (std::int64_t x = 0; x < dims[0]; ++x) {
        if (values[idx(x, y, z)] >= info.threshold) {
          ++counts[0][at(x)];
          ++counts[1][at(y)];
          ++counts[2][at(z)];
        }
      }
    }
  }
  Index3 lo{dims};
  Index3 hi{0, 0, 0};
  for (std::size_t a = 0; a < 3; ++a) {
    const std::int64_t most = *std::max_element(counts[a].begin(), counts[a].end());
    const std::int64_t enough = std::max<std::int64_t>(2, most / 50);
    for (std::int64_t i = 0; i < dims[a]; ++i) {
      if (counts[a][at(i)] >= enough) {
        lo[a] = std::min(lo[a], i);
        hi[a] = std::max(hi[a], i + 1);
      }
    }
  }
  if (hi[0] <= lo[0]) {
    throw std::invalid_argument("The dataset holds no material above the air threshold");
  }
  const std::int64_t scale = std::int64_t{1} << static_cast<unsigned>(level);
  Overview o;
  for (std::size_t a = 0; a < 3; ++a) {
    o.lo[a] = std::max<std::int64_t>(0, lo[a] * scale - scale);
    o.hi[a] = std::min(info.dims[a], hi[a] * scale + scale);
  }
  // Round cross-section: the filled share of the bounding box of the middle slice is pi / 4.
  int round_axis = -1;
  double round_error = 1.0;
  for (int c = 0; c < 3; ++c) {
    const auto [u, v] = planeAxes(c);
    const std::int64_t mid = (lo[at(c)] + hi[at(c)]) / 2;
    std::int64_t area = 0;
    for (std::int64_t j = lo[at(v)]; j < hi[at(v)]; ++j) {
      std::int64_t first = -1;
      std::int64_t last = -1;
      for (std::int64_t i = lo[at(u)]; i < hi[at(u)]; ++i) {
        Index3 p{};
        p[at(c)] = mid;
        p[at(u)] = i;
        p[at(v)] = j;
        Index3 q = p;
        q[at(u)] = std::min(i + 1, hi[at(u)] - 1);
        if (values[idx(p[0], p[1], p[2])] >= info.threshold &&
            values[idx(q[0], q[1], q[2])] >= info.threshold) {
          first = first < 0 ? i : first;
          last = i + 1;
        }
      }
      area += first < 0 ? 0 : last - first + 1;
    }
    const auto wu = static_cast<double>(hi[at(u)] - lo[at(u)]) * info.voxel_size[at(u)];
    const auto wv = static_cast<double>(hi[at(v)] - lo[at(v)]) * info.voxel_size[at(v)];
    const auto fill = static_cast<double>(area) /
                      static_cast<double>((hi[at(u)] - lo[at(u)]) * (hi[at(v)] - lo[at(v)]));
    const double aspect = wu / wv;
    const double error = std::abs(fill - kPi / 4.0);
    if (aspect > 0.8 && aspect < 1.25 && error < 0.09 && error < round_error) {
      round_error = error;
      round_axis = c;
    }
  }
  o.type =
      options.cell_type.value_or(round_axis >= 0 ? CellType::kCylindrical : CellType::kStacked);
  if (options.axis) {
    o.axis = *options.axis;
  } else if (o.type == CellType::kCylindrical) {
    if (round_axis < 0) {
      // Given as cylindrical but not round: the longest extent.
      warnings.emplace_back("No round cross-section found; the cell axis is the longest extent");
    }
    o.axis = round_axis;
  } else {
    o.axis = -1;
  }
  if (o.axis < 0) {
    // Cylindrical without a round section: the longest extent; stacked: the thinnest.
    double best = o.type == CellType::kStacked ? std::numeric_limits<double>::infinity() : 0.0;
    for (int a = 0; a < 3; ++a) {
      const auto extent = static_cast<double>(o.hi[at(a)] - o.lo[at(a)]) * info.voxel_size[at(a)];
      if (o.type == CellType::kStacked ? extent < best : extent > best) {
        best = extent;
        o.axis = a;
      }
    }
  }
  if (o.axis < 0 || o.axis > 2) {
    throw std::invalid_argument("The cell axis must be 0, 1 or 2");
  }
  return o;
}

// ---------------------------------------------------------------------------------------------
// Analysis of a dataset

BatteryResult analyze(const Dataset& dataset, const BatteryOptions& options) {
  const DatasetInfo& info = dataset.info();
  const VolumeView volume = viewOf(dataset);
  BatteryResult result;
  result.dims = info.dims;
  result.voxel_size = info.voxel_size;
  result.levels.air = info.air_level;
  result.min_overhang_mm = kNaN;
  const VoxelSize& pitch = info.voxel_size;
  Extras extras;
  Overview box;
  {
    const TelemetryPhase phase("overview");
    options.progress.check();
    box = overview(dataset, options, result.warnings);
  }
  extras.box = box;
  result.type = box.type;
  result.axis = box.axis;
  for (std::size_t a = 0; a < 3; ++a) {
    result.bounds_min_mm[a] = static_cast<double>(box.lo[a]) * pitch[a];
    result.bounds_max_mm[a] = static_cast<double>(box.hi[a] - 1) * pitch[a];
  }
  // Until measured: the length of the bounding box.
  result.length_mm =
      static_cast<double>(box.hi[at(box.axis)] - box.lo[at(box.axis)]) * pitch[at(box.axis)];
  InRegion in_region;
  if (box.type == CellType::kCylindrical) {
    CylinderData data;
    data.axis = box.axis;
    std::tie(data.u, data.v) = planeAxes(box.axis);
    data.box = box;
    cylinderSections(volume, options, data, result);
    cylinderOuterSize(volume, volume, data, result);
    cylinderOverhang(volume, options, data, result, extras.worst);
    summarizeEdges(result);
    cylinderEnds(data, result);
    extras.points = data.points;
    extras.cross_axis = box.axis;
    extras.cross_index = static_cast<std::int64_t>(std::lround(
        result.sections[result.sections.size() / 2].position_mm[0] / pitch[at(box.axis)]));
    // Longitudinal: normal to the first in-plane axis, through the centre.
    extras.long_axis = data.u;
    const double centre_pos =
        0.5 * (result.bounds_min_mm[at(box.axis)] + result.bounds_max_mm[at(box.axis)]);
    extras.long_index =
        static_cast<std::int64_t>(std::lround(data.centre(centre_pos)[0] / pitch[at(data.u)]));
    in_region = cylinderRegion(data, pitch);
  } else {
    StackData data;
    data.n = box.axis;
    const auto [p, q] = planeAxes(box.axis);
    const auto ep = static_cast<double>(box.hi[at(p)] - box.lo[at(p)]) * pitch[at(p)];
    const auto eq = static_cast<double>(box.hi[at(q)] - box.lo[at(q)]) * pitch[at(q)];
    data.u = ep >= eq ? p : q;
    data.v = ep >= eq ? q : p;
    data.box = box;
    stackSections(volume, options, data, result);
    {
      const Index3 through{(box.lo[0] + box.hi[0]) / 2, (box.lo[1] + box.hi[1]) / 2,
                           (box.lo[2] + box.hi[2]) / 2};
      for (int a = 0; a < 3; ++a) {
        result.outer_size_mm[at(a)] = outerExtentMm(volume, box, a, through, 1);
      }
      result.length_mm = result.outer_size_mm[at(box.axis)];
    }
    stackOverhang(volume, options, data, result, extras.worst);
    summarizeEdges(result);
    // The electrode stack: between the outermost layers, inside the anode edges.
    std::array<double, 6> limits{};
    for (std::size_t a = 0; a < 3; ++a) {
      const double inset = 0.05 * (result.bounds_max_mm[a] - result.bounds_min_mm[a]);
      limits[2 * a] = result.bounds_min_mm[a] + inset;
      limits[2 * a + 1] = result.bounds_max_mm[a] - inset;
    }
    std::array<double, 6> void_limits = limits;
    for (const BatteryEdge& edge : result.edges) {
      if (edge.samples > 0) {
        const double inwards = (edge.side == 0 ? 2.0 : -2.0) * pitch[at(edge.axis)];
        limits[at(2 * edge.axis + edge.side)] = edge.anode_edge_mm + inwards;
        void_limits[at(2 * edge.axis + edge.side)] = edge.cathode_edge_mm + inwards;
      }
    }
    if (!std::isnan(data.first_layer)) {
      limits[2 * at(data.n)] = data.first_layer - data.half_pitch;
      limits[at(2 * data.n + 1)] = data.last_layer + data.half_pitch;
      void_limits[2 * at(data.n)] = data.first_layer;
      void_limits[at(2 * data.n + 1)] = data.last_layer;
    }
    in_region = [limits, void_limits, pitch](const Index3& v, bool bright) {
      const std::array<double, 6>& l = bright ? limits : void_limits;
      for (std::size_t a = 0; a < 3; ++a) {
        const auto mm = static_cast<double>(v[a]) * pitch[a];
        if (mm < l[2 * a] || mm > l[2 * a + 1]) {
          return false;
        }
      }
      return true;
    };
    extras.cross_axis = data.v;
    extras.cross_index = (box.lo[at(data.v)] + box.hi[at(data.v)]) / 2;
    extras.long_axis = data.u;
    extras.long_index = (box.lo[at(data.u)] + box.hi[at(data.u)]) / 2;
  }
  summarizeSections(result);
  setThresholds(result, options);
  findParticlesAndVoids(dataset, in_region, result, options);
  placeFindings(result);
  renderImages(volume, volume, extras, result);
  return result;
}

// ---------------------------------------------------------------------------------------------
// Results as JSON

nlohmann::json findingJson(const CellFinding& f, CellType type) {
  nlohmann::json j = {{"id", f.id},
                      {"center_mm", f.center_mm},
                      {"voxels", f.voxels},
                      {"volume_mm3", f.volume_mm3},
                      {"size_mm", f.size_mm},
                      {"equivalent_diameter_mm", f.equivalent_diameter_mm},
                      {"grey_value", f.extreme_value}};
  if (type == CellType::kCylindrical) {
    j["radius_mm"] = f.radius_mm;
    j["angle_deg"] = f.angle_deg;
  } else {
    j["normal_mm"] = f.radius_mm;
  }
  return j;
}

/// NaN as null, so that the JSON stays valid.
nlohmann::json number(double value) {
  return std::isfinite(value) ? nlohmann::json(value) : nlohmann::json();
}

}  // namespace

BatteryResult analyzeBattery(const Dataset& dataset, const BatteryOptions& options) {
  return analyze(dataset, options);
}

BatteryResult analyzeBatteryImages(const CellImages& images, const BatteryOptions& options) {
  return analyzeImages(images, options);
}

nlohmann::json toJson(const BatteryResult& result) {
  const bool cylindrical = result.type == CellType::kCylindrical;
  nlohmann::json sections = nlohmann::json::array();
  for (const BatterySection& s : result.sections) {
    nlohmann::json j = {
        {"outer_mm", number(s.outer_mm)},         {"min_wall_mm", number(s.min_wall_mm)},
        {"mean_wall_mm", number(s.mean_wall_mm)}, {"stack_mm", number(s.stack_mm)},
        {"cathode_layers", s.cathode_layers},     {"anode_layers", s.anode_layers},
        {"pitch_mm", number(s.pitch_mm)}};
    if (cylindrical) {
      j["position_mm"] = s.position_mm[0];
      j["center_mm"] = s.center_mm;
      j["out_of_roundness_mm"] = number(s.out_of_roundness_mm);
      j["dent_mm"] = number(s.dent_mm);
      j["core_diameter_mm"] = number(s.core_diameter_mm);
      j["core_out_of_roundness_mm"] = number(s.core_out_of_roundness_mm);
      j["core_offset_mm"] = number(s.core_offset_mm);
      nlohmann::json outer = nlohmann::json::array();
      nlohmann::json core = nlohmann::json::array();
      for (std::size_t k = 0; k < s.outer_radius_mm.size(); ++k) {
        outer.push_back(number(s.outer_radius_mm[k]));
        core.push_back(number(s.core_radius_mm[k]));
      }
      j["outer_radius_mm"] = outer;
      j["core_radius_mm"] = core;
    } else {
      j["position_mm"] = s.position_mm;
    }
    sections.push_back(j);
  }
  nlohmann::json edges = nlohmann::json::array();
  for (const BatteryEdge& e : result.edges) {
    edges.push_back({{"axis", e.axis},
                     {"side", e.side == 0 ? "min" : "max"},
                     {"name", e.name},
                     {"samples", e.samples},
                     {"min_mm", number(e.min_mm)},
                     {"mean_mm", number(e.mean_mm)},
                     {"max_mm", number(e.max_mm)},
                     {"std_mm", number(e.std_mm)},
                     {"worst", e.worst},
                     {"cathode_edge_spread_mm", number(e.cathode_edge_spread_mm)},
                     {"anode_edge_spread_mm", number(e.anode_edge_spread_mm)},
                     {"anode_edge_mm", number(e.anode_edge_mm)},
                     {"cathode_edge_mm", number(e.cathode_edge_mm)}});
  }
  nlohmann::json overhang = nlohmann::json::array();
  for (const OverhangSample& s : result.overhang) {
    overhang.push_back({{"edge", s.edge},
                        {cylindrical ? "angle_deg" : "position_mm", s.section},
                        {"layer", s.layer},
                        {"across_mm", s.across_mm},
                        {"cathode_end_mm", s.cathode_end_mm},
                        {"anode_end_mm", s.anode_end_mm},
                        {"overhang_mm", s.overhang_mm}});
  }
  nlohmann::json gaps = nlohmann::json::array();
  for (const LayerGap& g : result.gaps) {
    gaps.push_back({{"position_mm", g.position_mm},
                    {"size_mm", g.size_mm},
                    {"expected_mm", g.expected_mm},
                    {"section", g.section}});
  }
  nlohmann::json particles = nlohmann::json::array();
  for (const CellFinding& f : result.particles) {
    particles.push_back(findingJson(f, result.type));
  }
  nlohmann::json voids = nlohmann::json::array();
  for (const CellFinding& f : result.voids) {
    voids.push_back(findingJson(f, result.type));
  }
  const BatteryLevels& l = result.levels;
  return {{"type", cylindrical ? "cylindrical" : "stacked"},
          {"axis", result.axis},
          {"dims", result.dims},
          {"voxel_size_mm", result.voxel_size},
          {"levels",
           {{"air", l.air},
            {"gap", number(l.gap)},
            {"anode", number(l.anode)},
            {"cathode", number(l.cathode)},
            {"housing", number(l.housing)},
            {"electrodes_distinct", l.electrodes_distinct}}},
          {"bounds_min_mm", result.bounds_min_mm},
          {"bounds_max_mm", result.bounds_max_mm},
          {"outer_size_mm", result.outer_size_mm},
          {"length_mm", result.length_mm},
          {"cathode_layers", result.cathode_layers},
          {"anode_layers", result.anode_layers},
          {"pitch_mm", number(result.pitch_mm)},
          {"outer_mean_mm", number(result.outer_mean_mm)},
          {"outer_min_size_mm", number(result.outer_min_size_mm)},
          {"outer_max_size_mm", number(result.outer_max_size_mm)},
          {"max_out_of_roundness_mm", number(result.max_out_of_roundness_mm)},
          {"max_dent_mm", number(result.max_dent_mm)},
          {"min_wall_mm", number(result.min_wall_mm)},
          {"max_core_out_of_roundness_mm", number(result.max_core_out_of_roundness_mm)},
          {"min_overhang_mm", number(result.min_overhang_mm)},
          {"particle_threshold", result.particle_threshold},
          {"void_threshold", result.void_threshold},
          {"sections", sections},
          {"edges", edges},
          {"overhang", overhang},
          {"gaps", gaps},
          {"particles", particles},
          {"voids", voids},
          {"bright_structures", result.bright_structures},
          {"from_images", result.from_images},
          {"warnings", result.warnings}};
}

void writeBatteryResult(const BatteryResult& result, const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
  writeJson(dir / "battery.json", toJson(result));
  for (const BatteryImage& image : result.images) {
    // Small sections are enlarged, pixel by pixel, so that the layers can be seen.
    const std::int64_t f = std::max<std::int64_t>(1, 640 / std::max(image.width, image.height));
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(3 * image.width * image.height * f * f));
    for (std::int64_t y = 0; y < image.height * f; ++y) {
      for (std::int64_t x = 0; x < image.width * f; ++x) {
        for (std::int64_t c = 0; c < 3; ++c) {
          rgb[static_cast<std::size_t>(3 * (x + image.width * f * y) + c)] =
              image.rgb[static_cast<std::size_t>(3 * (x / f + image.width * (y / f)) + c)];
        }
      }
    }
    detail::writeRgbPng(dir / (image.name + ".png"), static_cast<std::uint32_t>(image.width * f),
                        static_cast<std::uint32_t>(image.height * f), rgb, image.pixel_width_mm,
                        image.pixel_height_mm);
  }
}

}  // namespace voxelsieve
