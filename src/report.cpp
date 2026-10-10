#include "voxelsieve/report.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <numbers>
#include <sstream>
#include <stdexcept>

#include "detail/png.hpp"
#include "voxelsieve/compare.hpp"
#include "voxelsieve/render.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxListedPores = 25;
constexpr std::string_view kNotGiven = "not given";

/// Number with a decimal point and commas between thousands, "12,345.6".
std::string formatNumber(double value, int decimals) {
  std::array<char, 64> buffer{};
  (void)std::snprintf(buffer.data(), buffer.size(), "%.*f", decimals, std::abs(value));
  const std::string digits(buffer.data());
  const auto point = digits.find('.');
  std::string integer = digits.substr(0, point);
  const std::string fraction = point == std::string::npos ? "" : digits.substr(point + 1);
  std::string grouped;
  for (std::size_t i = 0; i < integer.size(); ++i) {
    if (i > 0 && (integer.size() - i) % 3 == 0 && integer.size() > 4) {
      grouped += ",";
    }
    grouped += integer[i];
  }
  std::string out = (value < 0.0 && std::stod(digits) != 0.0 ? "-" : "") + grouped;
  if (!fraction.empty()) {
    out += "." + fraction;
  }
  return out;
}

std::string formatMm(const std::array<double, 3>& p) {
  return formatNumber(p[0], 2) + ", " + formatNumber(p[1], 2) + ", " + formatNumber(p[2], 2);
}

bool insideBox(const std::array<double, 3>& p_mm, const std::array<std::array<double, 3>, 2>& box) {
  for (std::size_t a = 0; a < 3; ++a) {
    if (p_mm[a] < box[0][a] || p_mm[a] >= box[1][a]) {
      return false;
    }
  }
  return true;
}

/// "100.0 µm" or "330.0 × 330.0 × 600.0 µm", with the slice thickness when it leaves gaps.
std::string voxelSizeText(const VoxelSize& v) {
  std::string text = v.isotropic()
                         ? formatNumber(v[0] * 1000.0, 1)
                         : formatNumber(v[0] * 1000.0, 1) + " × " + formatNumber(v[1] * 1000.0, 1) +
                               " × " + formatNumber(v[2] * 1000.0, 1);
  text += " µm";
  if (v.slice_thickness_mm > 0.0) {
    text += " (slice thickness " + formatNumber(v.slice_thickness_mm * 1000.0, 1) + " µm)";
  }
  return text;
}

std::array<double, 3> toMm(const std::array<double, 3>& voxels, const VoxelSize& voxel_size) {
  return voxel_size.toMm(voxels);
}

std::string base64(const std::vector<char>& bytes) {
  static constexpr std::string_view kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    std::uint32_t chunk = static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i])) << 16U;
    if (i + 1 < bytes.size()) {
      chunk |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 1])) << 8U;
    }
    if (i + 2 < bytes.size()) {
      chunk |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 2]));
    }
    out += kAlphabet[(chunk >> 18U) & 63U];
    out += kAlphabet[(chunk >> 12U) & 63U];
    out += i + 1 < bytes.size() ? kAlphabet[(chunk >> 6U) & 63U] : '=';
    out += i + 2 < bytes.size() ? kAlphabet[chunk & 63U] : '=';
  }
  return out;
}

std::string dataUri(const std::filesystem::path& png) {
  std::ifstream in(png, std::ios::binary);
  if (!in) {
    return {};
  }
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
  return "data:image/png;base64," + base64(bytes);
}

std::string dataUri(const RenderImage& image) {
  const auto png =
      detail::encodePng(static_cast<std::uint32_t>(image.width),
                        static_cast<std::uint32_t>(image.height), image.channels, image.pixels);
  return "data:image/png;base64," + base64(std::vector<char>(png.begin(), png.end()));
}

/// Signed number with a decimal point, "+0.123" or "−0.123".
std::string formatSigned(double value, int decimals) {
  std::string text = formatNumber(std::abs(value), decimals);
  const bool zero = text.find_first_not_of("0.") == std::string::npos;
  if (zero) {
    return text;
  }
  return (value < 0.0 ? "−" : "+") + text;
}

std::string hexColor(const std::array<std::uint8_t, 3>& c) {
  std::array<char, 8> text{};
  std::snprintf(text.data(), text.size(), "#%02x%02x%02x", c[0], c[1], c[2]);
  return text.data();
}

/// Histogram of the deviation as SVG: area fraction per bin, bars coloured like the 3D views,
/// with the tolerance band and an axis in mm.
std::string deviationHistogramSvg(const std::vector<double>& histogram, double tolerance,
                                  double range) {
  constexpr double kWidth = 640.0;
  constexpr double kHeight = 200.0;
  constexpr double kLeft = 10.0;
  constexpr double kRight = 630.0;
  constexpr double kTop = 12.0;
  constexpr double kBottom = 160.0;
  std::ostringstream svg;
  svg.setf(std::ios::fixed);
  svg.precision(2);
  svg << R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 )" << kWidth << ' ' << kHeight
      << R"(" role="img" aria-label="Distribution of the deviation" )"
      << R"(font-family="Helvetica Neue, Arial, sans-serif" font-size="11">)";
  const auto x = [&](double mm) { return kLeft + (mm + range) / (2.0 * range) * (kRight - kLeft); };
  if (range > 0.0 && tolerance < range) {
    svg << R"(<rect x=")" << x(-tolerance) << R"(" y=")" << kTop << R"(" width=")"
        << x(tolerance) - x(-tolerance) << R"(" height=")" << kBottom - kTop
        << R"(" fill="#3cbe5a" fill-opacity="0.08"/>)";
  }
  double peak = 0.0;
  for (const double f : histogram) {
    peak = std::max(peak, f);
  }
  const auto bins = histogram.size();
  for (std::size_t i = 0; i < bins && peak > 0.0 && range > 0.0; ++i) {
    const double lo = -range + 2.0 * range * static_cast<double>(i) / static_cast<double>(bins);
    const double hi = lo + 2.0 * range / static_cast<double>(bins);
    const double h = histogram[i] / peak * (kBottom - kTop);
    svg << R"(<rect x=")" << x(lo) + 0.5 << R"(" y=")" << kBottom - h << R"(" width=")"
        << std::max(0.0, x(hi) - x(lo) - 1.0) << R"(" height=")" << h << R"(" fill=")"
        << hexColor(deviationColor(0.5 * (lo + hi), tolerance, range)) << R"("/>)";
  }
  svg << R"(<line x1=")" << kLeft << R"(" y1=")" << kBottom << R"(" x2=")" << kRight << R"(" y2=")"
      << kBottom << R"(" stroke="#5f6368"/>)";
  std::vector<double> ticks{-range, 0.0, range};
  if (tolerance > 0.0 && tolerance < 0.8 * range) {
    ticks.insert(ticks.begin() + 1, -tolerance);
    ticks.insert(ticks.end() - 1, tolerance);
  }
  for (const double tick : ticks) {
    const char* anchor = tick <= -range ? "start" : (tick >= range ? "end" : "middle");
    svg << R"(<line x1=")" << x(tick) << R"(" y1=")" << kBottom << R"(" x2=")" << x(tick)
        << R"(" y2=")" << kBottom + 4.0 << R"(" stroke="#5f6368"/>)";
    svg << R"(<text x=")" << x(tick) << R"(" y=")" << kBottom + 17.0 << R"(" text-anchor=")"
        << anchor << R"(" fill="#1d1d1f">)" << formatSigned(tick, 3) << "</text>";
  }
  svg << R"(<text x=")" << 0.5 * (kLeft + kRight) << R"(" y=")" << kBottom + 34.0
      << R"(" text-anchor="middle" fill="#5f6368">Deviation in mm (edge bins include values beyond)</text>)";
  svg << "</svg>";
  return svg.str();
}

/// Mandatory fields of the report, from the report contents required by ISO/IEC 17025
/// (7.8.2.1) and the CT-specific settings a test report states under ISO 15708-3.
struct Field {
  std::string_view path;
  std::string_view label;
};
constexpr std::array kMandatoryFields{
    Field{"report.number", "Report number"},
    Field{"report.date", "Date of issue"},
    Field{"laboratory.name", "Testing laboratory"},
    Field{"laboratory.address", "Address of the testing laboratory"},
    Field{"customer.name", "Customer"},
    Field{"order.number", "Order number"},
    Field{"order.test_date", "Date of test"},
    Field{"part.name", "Designation of the test item"},
    Field{"part.drawing", "Drawing number"},
    Field{"part.material", "Material"},
    Field{"part.serial", "Serial number or batch"},
    Field{"scan.device", "CT system"},
    Field{"scan.voltage_kv", "Tube voltage"},
    Field{"scan.current_ua", "Tube current"},
    Field{"scan.filter", "Pre-filter"},
    Field{"scan.projections", "Number of projections"},
    Field{"scan.exposure_ms", "Exposure time"},
    Field{"scan.reconstruction", "Reconstruction"},
    Field{"uncertainty", "Measurement uncertainty"},
    Field{"approval.approver", "Approved by"},
};

void completeMandatory(Json& data, std::vector<std::string>* warnings) {
  for (const Field& field : kMandatoryFields) {
    Json* node = &data;
    std::string_view rest = field.path;
    while (true) {
      const auto dot = rest.find('.');
      const std::string key(rest.substr(0, dot));
      if (!node->is_object()) {
        *node = Json::object();
      }
      if (dot == std::string_view::npos) {
        Json& value = (*node)[key];
        if (value.is_null() || (value.is_string() && value.get<std::string>().empty())) {
          value = kNotGiven;
          if (warnings != nullptr) {
            warnings->push_back(std::string(field.label) + " (" + std::string(field.path) +
                                ") missing");
          }
        }
        break;
      }
      node = &(*node)[key];
      rest = rest.substr(dot + 1);
    }
  }
}

std::string regionText(const InspectionZone& zone) {
  if (!zone.box_mm) {
    return "whole part";
  }
  const auto& box = *zone.box_mm;
  return "box from (" + formatMm(box[0]) + ") to (" + formatMm(box[1]) + ") mm";
}

std::string criterionLabel(const CriterionResult& criterion, const AcceptanceLimits& limits) {
  if (criterion.name == "max_pore_size_mm") {
    return "Largest extent of a pore";
  }
  if (criterion.name == "max_pore_count") {
    return limits.count_min_size_mm > 0.0
               ? "Number of pores from " + formatNumber(limits.count_min_size_mm, 2) + " mm"
               : "Number of pores";
  }
  if (criterion.name == "max_porosity_percent") {
    return "Porosity (pores and loosened microstructure)";
  }
  return "Loosened microstructure";
}

std::string criterionValue(const CriterionResult& criterion, bool limit) {
  const double value = limit ? criterion.limit : criterion.measured;
  if (criterion.name == "max_pore_size_mm") {
    return !limit && value == 0.0 ? "no pore" : formatNumber(value, 2) + " mm";
  }
  if (criterion.name == "max_porosity_percent") {
    return formatNumber(value, 3) + " %";
  }
  if (criterion.name == "loosening") {
    if (limit) {
      return "not permitted";
    }
    return value > 0.0 ? formatNumber(value, 0) + " zone(s)" : "none";
  }
  return formatNumber(value, 0);
}

}  // namespace

bool ZoneEvaluation::passed() const {
  return std::all_of(criteria.begin(), criteria.end(), [](const auto& c) { return c.passed; });
}

bool Evaluation::passed() const {
  return std::all_of(zones.begin(), zones.end(), [](const auto& z) { return z.passed(); });
}

double poreSizeMm(const DetectedPore& pore, const VoxelSize& voxel_size) {
  double longest = 0.0;
  for (std::size_t a = 0; a < 3; ++a) {
    longest = std::max(
        longest, static_cast<double>(pore.bounds.max[a] - pore.bounds.min[a]) * voxel_size[a]);
  }
  return longest;
}

Evaluation evaluate(const PorosityResult& result, const std::vector<InspectionZone>& zones) {
  const VoxelSize& v = result.voxel_size;
  Evaluation evaluation;
  for (const InspectionZone& zone : zones) {
    const auto inside = [&](const std::array<double, 3>& center_voxels) {
      return !zone.box_mm || insideBox(toMm(center_voxels, v), *zone.box_mm);
    };
    ZoneEvaluation out;
    out.name = zone.name;
    out.part_volume_mm3 = zone.box_mm ? result.partVolumeMm3(*zone.box_mm) : result.part_volume_mm3;
    for (const DetectedPore& pore : result.pores) {
      if (!inside(pore.center_voxels)) {
        continue;
      }
      const double size = poreSizeMm(pore, v);
      ++out.pore_count;
      if (size >= zone.limits.count_min_size_mm) {
        ++out.counted_pores;
      }
      if (size > out.largest_pore_size_mm) {
        out.largest_pore_size_mm = size;
        out.largest_pore_id = pore.id;
      }
      out.void_volume_mm3 += pore.volume_mm3;
    }
    for (const PorosityZone& loosened : result.zones) {
      if (inside(loosened.center_voxels)) {
        ++out.loosening_zones;
        out.void_volume_mm3 += loosened.void_volume_mm3;
      }
    }
    out.porosity_percent =
        out.part_volume_mm3 > 0.0 ? 100.0 * out.void_volume_mm3 / out.part_volume_mm3 : 0.0;

    const AcceptanceLimits& limits = zone.limits;
    if (limits.max_pore_size_mm) {
      out.criteria.push_back({"max_pore_size_mm", out.largest_pore_size_mm,
                              *limits.max_pore_size_mm,
                              out.largest_pore_size_mm <= *limits.max_pore_size_mm});
    }
    if (limits.max_pore_count) {
      out.criteria.push_back({"max_pore_count", static_cast<double>(out.counted_pores),
                              static_cast<double>(*limits.max_pore_count),
                              out.counted_pores <= *limits.max_pore_count});
    }
    if (limits.max_porosity_percent) {
      out.criteria.push_back({"max_porosity_percent", out.porosity_percent,
                              *limits.max_porosity_percent,
                              out.porosity_percent <= *limits.max_porosity_percent});
    }
    if (!limits.allow_loosening) {
      out.criteria.push_back(
          {"loosening", static_cast<double>(out.loosening_zones), 0.0, out.loosening_zones == 0});
    }
    evaluation.zones.push_back(std::move(out));
  }
  return evaluation;
}

std::vector<InspectionZone> inspectionZonesFromJson(const nlohmann::json& order) {
  std::vector<InspectionZone> zones;
  const auto acceptance = order.find("acceptance");
  if (acceptance == order.end() || !acceptance->contains("zones")) {
    return zones;
  }
  for (const Json& item : acceptance->at("zones")) {
    InspectionZone zone;
    zone.name = item.value("name", "Zone " + std::to_string(zones.size() + 1));
    if (item.contains("box_mm")) {
      const Json& box = item.at("box_mm");
      const auto is_point = [](const Json& p) {
        return p.is_array() && p.size() == 3 &&
               std::all_of(p.begin(), p.end(), [](const Json& c) { return c.is_number(); });
      };
      if (!box.is_array() || box.size() != 2 || !is_point(box[0]) || !is_point(box[1])) {
        throw std::invalid_argument("Inspection zone '" + zone.name +
                                    "': box_mm must be [[x0, y0, z0], [x1, y1, z1]]");
      }
      zone.box_mm = box.get<std::array<std::array<double, 3>, 2>>();
    }
    AcceptanceLimits& limits = zone.limits;
    if (item.contains("max_pore_size_mm")) {
      limits.max_pore_size_mm = item.at("max_pore_size_mm").get<double>();
    }
    if (item.contains("max_pore_count")) {
      limits.max_pore_count = item.at("max_pore_count").get<std::int64_t>();
    }
    limits.count_min_size_mm = item.value("count_min_size_mm", 0.0);
    if (item.contains("max_porosity_percent")) {
      limits.max_porosity_percent = item.at("max_porosity_percent").get<double>();
    }
    limits.allow_loosening = item.value("allow_loosening", true);
    zones.push_back(std::move(zone));
  }
  return zones;
}

nlohmann::json reportData(const nlohmann::json& order, const PorosityResult& result,
                          const PorosityOptions& options, const Evaluation& evaluation,
                          const std::filesystem::path& image_dir,
                          std::vector<std::string>* warnings) {
  Json data = order.is_object() ? order : Json::object();
  completeMandatory(data, warnings);
  const VoxelSize& v = result.voxel_size;
  const double voxel_volume = v.volumeMm3();

  data["software"] = {{"name", "VoxelSieve"}, {"version", VOXELSIEVE_VERSION}};
  const double min_pore_volume = static_cast<double>(options.min_pore_voxels) * voxel_volume;
  data["analysis"] = {
      {"voxel_size", voxelSizeText(v)},
      {"min_pore_voxels", options.min_pore_voxels},
      {"min_pore_volume", formatNumber(min_pore_volume, 6) + " mm³"},
      {"min_pore_diameter",
       formatNumber(std::cbrt(6.0 * min_pore_volume / std::numbers::pi), 3) + " mm"},
      {"zone_block", v.isotropic()
                         ? formatNumber(8.0 * v[0], 2) + " mm"
                         : formatNumber(8.0 * v[0], 2) + " × " + formatNumber(8.0 * v[1], 2) +
                               " × " + formatNumber(8.0 * v[2], 2) + " mm"},
      {"zone_min_porosity", formatNumber(100.0 * options.min_zone_void_fraction, 1) + " %"},
      {"zone_sigma", formatNumber(options.zone_sigma, 1)},
      {"material_level", formatNumber(result.material_level, 0)},
      {"air_level", formatNumber(result.air_level, 0)},
      {"noise_sigma", formatNumber(result.noise_sigma, 0)},
  };
  data["results"] = {
      {"part_volume", formatNumber(result.part_volume_mm3, 1) + " mm³"},
      {"pore_count", result.pores.size()},
      {"pore_volume", formatNumber(result.poreVolumeMm3(), 3) + " mm³"},
      {"zone_count", result.zones.size()},
      {"zone_void_volume", formatNumber(result.zoneVoidVolumeMm3(), 3) + " mm³"},
      {"porosity", formatNumber(100.0 * result.porosity(), 3) + " %"},
  };

  std::vector<const DetectedPore*> by_size;
  by_size.reserve(result.pores.size());
  for (const DetectedPore& pore : result.pores) {
    by_size.push_back(&pore);
  }
  std::stable_sort(by_size.begin(), by_size.end(), [v](const auto* a, const auto* b) {
    return poreSizeMm(*a, v) > poreSizeMm(*b, v);
  });
  Json pores = Json::array();
  for (std::size_t i = 0; i < std::min(by_size.size(), kMaxListedPores); ++i) {
    const DetectedPore& pore = *by_size[i];
    pores.push_back({{"id", pore.id},
                     {"position", formatMm(toMm(pore.center_voxels, v))},
                     {"size", formatNumber(poreSizeMm(pore, v), 2)},
                     {"diameter", formatNumber(pore.equivalent_diameter_mm, 2)},
                     {"volume", formatNumber(pore.volume_mm3, 4)}});
  }
  data["has_pores"] = !pores.empty();
  data["pores"] = pores;
  data["pores_listed"] = pores.size();
  data["pores_more"] = by_size.size() > kMaxListedPores;

  Json zones = Json::array();
  for (const PorosityZone& zone : result.zones) {
    zones.push_back({{"id", zone.id},
                     {"position", formatMm(toMm(zone.center_voxels, v))},
                     {"volume", formatNumber(zone.volume_mm3, 2)},
                     {"void_volume", formatNumber(zone.void_volume_mm3, 3)},
                     {"porosity", formatNumber(100.0 * zone.porosity(), 2)}});
  }
  data["has_zones"] = !zones.empty();
  data["zones"] = zones;

  if (!evaluation.zones.empty()) {
    const auto inspection_zones = inspectionZonesFromJson(order);
    Json evaluated = Json::array();
    for (std::size_t i = 0; i < evaluation.zones.size(); ++i) {
      const ZoneEvaluation& zone = evaluation.zones[i];
      const InspectionZone& spec = inspection_zones.at(i);
      Json criteria = Json::array();
      for (const CriterionResult& criterion : zone.criteria) {
        criteria.push_back({{"label", criterionLabel(criterion, spec.limits)},
                            {"measured", criterionValue(criterion, false)},
                            {"limit", criterionValue(criterion, true)},
                            {"passed", criterion.passed},
                            {"verdict", criterion.passed ? "passed" : "failed"}});
      }
      evaluated.push_back({{"name", zone.name},
                           {"region", regionText(spec)},
                           {"part_volume", formatNumber(zone.part_volume_mm3, 1) + " mm³"},
                           {"criteria", criteria},
                           {"passed", zone.passed()},
                           {"verdict", zone.passed() ? "passed" : "failed"}});
    }
    const std::string guideline = order.contains("acceptance")
                                      ? order["acceptance"].value("guideline", "BDG P 202")
                                      : "BDG P 202";
    data["evaluation"] = {{"guideline", guideline},
                          {"zones", evaluated},
                          {"passed", evaluation.passed()},
                          {"verdict", evaluation.passed() ? "The requirements are met."
                                                          : "The requirements are not met."}};
  }

  data["images"] = {{"x", dataUri(image_dir / "projection_x.png")},
                    {"y", dataUri(image_dir / "projection_y.png")},
                    {"z", dataUri(image_dir / "projection_z.png")}};
  return data;
}

void addPartImages(Json& data, const SurfaceMask& surface, const PorosityResult& result) {
  IndexedMesh mesh = surfaceDisplayMesh(surface, 1500000);
  if (mesh.triangles.empty()) {
    return;
  }
  // Rendered in mm, so that parts with voxels of different edge lengths keep their shape.
  const VoxelSize& v = result.voxel_size;
  for (auto& p : mesh.points) {
    for (std::size_t k = 0; k < 3; ++k) {
      p[k] = static_cast<float>(p[k] * v[k]);
    }
  }
  std::array<float, 3> lo = mesh.points.front();
  std::array<float, 3> hi = lo;
  for (const auto& p : mesh.points) {
    for (std::size_t k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], p[k]);
      hi[k] = std::max(hi[k], p[k]);
    }
  }
  const double size = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
  // Very small pores get a minimum radius so that they stay visible.
  const double min_radius = 0.004 * size;
  RenderView view;
  view.width = 900;
  view.height = 680;
  RenderScene part;
  part.mesh = &mesh;
  data["images"]["part"] = dataUri(render(part, view));

  RenderScene glass;
  glass.mesh = &mesh;
  glass.surface_opacity = 0.2;
  glass.surface_color = {120, 138, 162};
  for (const PorosityZone& zone : result.zones) {
    const double radius = std::cbrt(3.0 * zone.volume_mm3 / (4.0 * std::numbers::pi));
    glass.spheres.push_back(
        {v.toMm(zone.center_voxels), std::max(radius, min_radius), {240, 168, 32}});
  }
  for (const DetectedPore& pore : result.pores) {
    glass.spheres.push_back({v.toMm(pore.center_voxels),
                             std::max(0.5 * pore.equivalent_diameter_mm, min_radius),
                             {212, 44, 36}});
  }
  data["images"]["pores"] = dataUri(render(glass, view));
}

void addComparison(Json& data, const std::filesystem::path& dir, const std::string& cad_name) {
  std::ifstream in(dir / "compare.json", std::ios::binary);
  if (!in) {
    throw std::runtime_error("No compare.json in " + dir.string());
  }
  const Json compare = Json::parse(in);
  const Json& d = compare.at("deviation");
  const double tolerance = compare.at("tolerance_mm").get<double>();
  const double range = d.at("range_mm").get<double>();
  std::string cad = cad_name;
  if (cad.empty()) {
    cad = std::filesystem::path(compare.value("cad_path", std::string())).filename().string();
  }
  const auto percent = [&](const char* key) {
    return formatNumber(100.0 * d.at(key).get<double>(), 1) + " %";
  };
  const auto mm = [&](double value) { return formatSigned(value, 3) + " mm"; };
  const Json& p = d.at("percentiles_mm");
  data["comparison"] = {
      {"cad", cad.empty() ? std::string(kNotGiven) : cad},
      {"alignment", compare.value("alignment", "auto") == "none" ? "given position"
                                                                 : "best fit of the outer surface"},
      {"fit_rms", formatNumber(compare.at("fit").at("rms_mm").get<double>(), 3) + " mm"},
      {"tolerance", "± " + formatNumber(tolerance, 3) + " mm"},
      {"mean", mm(d.at("mean_mm").get<double>())},
      {"std", formatNumber(d.at("std_mm").get<double>(), 3) + " mm"},
      {"min", mm(d.at("min_mm").get<double>())},
      {"max", mm(d.at("max_mm").get<double>())},
      {"p5", mm(p.value("5", 0.0))},
      {"p95", mm(p.value("95", 0.0))},
      {"within", percent("within_tolerance")},
      {"above", percent("above_tolerance")},
      {"below", percent("below_tolerance")},
      {"area", formatNumber(d.at("area_mm2").get<double>(), 0) + " mm²"},
      {"dropped", compare.value("dropped_components", 0)},
      {"range", "± " + formatNumber(range, 3) + " mm"}};
  if (const auto deformation = compare.find("deformation"); deformation != compare.end()) {
    // The deviation was measured on the surface bent onto the CAD model (ADR 0022).
    const Json& moved = deformation->at("displacement");
    const Json& rigid = deformation->at("rigid_deviation");
    data["comparison"]["alignment"] =
        data["comparison"]["alignment"].get<std::string>() + ", then non-rigid registration";
    data["comparison"]["non_rigid"] = {
        {"max", formatNumber(moved.at("max_mm").get<double>(), 3) + " mm"},
        {"mean", formatNumber(moved.at("mean_mm").get<double>(), 3) + " mm"},
        {"moved", formatNumber(100.0 * moved.at("above_tolerance").get<double>(), 1) + " %"},
        {"spacing", formatNumber(deformation->at("spacing_mm").get<double>(), 3) + " mm"},
        {"stiffness", formatNumber(deformation->at("stiffness").get<double>(), 3)},
        {"rigid_within",
         formatNumber(100.0 * rigid.at("within_tolerance").get<double>(), 1) + " %"},
        {"rigid_mean", mm(rigid.at("mean_mm").get<double>())},
        {"rigid_std", formatNumber(rigid.at("std_mm").get<double>(), 3) + " mm"}};
    data["images"]["displacement_1"] = dataUri(dir / "displacement_view_1.png");
    data["images"]["displacement_2"] = dataUri(dir / "displacement_view_2.png");
  }
  data["images"]["deviation_1"] = dataUri(dir / "deviation_view_1.png");
  data["images"]["deviation_2"] = dataUri(dir / "deviation_view_2.png");
  data["images"]["deviation_histogram"] =
      deviationHistogramSvg(d.at("histogram").get<std::vector<double>>(), tolerance, range);
}

}  // namespace voxelsieve
