#include "voxelsieve/report.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <span>
#include <sstream>
#include <stdexcept>

#include "detail/png.hpp"
#include "voxelsieve/compare.hpp"
#include "voxelsieve/render.hpp"
#include "voxelsieve/slice.hpp"
#include "voxelsieve/thickness.hpp"

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

/// Numbers the sections of the report that come after the results and depend on what it holds.
void numberSections(Json& data) {
  int next = 7;
  Json section = Json::object();
  for (const char* name : {"materials", "fibres", "comparison", "wall_thickness"}) {
    if (data.contains(name)) {
      section[name] = next++;
    }
  }
  section["assessment"] = next;
  data["section"] = section;
}

/// Angle in degrees between two directions, whatever their sign.
double axialAngleDeg(const std::array<double, 3>& a, const std::array<double, 3>& b) {
  const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const double norm = std::sqrt((a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) *
                                (b[0] * b[0] + b[1] * b[1] + b[2] * b[2]));
  if (norm <= 0.0) {
    throw std::invalid_argument("A fibre direction must not be zero");
  }
  return std::acos(std::clamp(std::abs(dot) / norm, 0.0, 1.0)) * 180.0 / std::numbers::pi;
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

/// Histogram of the wall thickness as SVG: area fraction per bin from 0 to `range`, bars coloured
/// like the views, the ranges below `min_wall` and above `max_wall` shaded (limits of 0 are off).
std::string thicknessHistogramSvg(const std::vector<double>& histogram, double min_wall,
                                  double max_wall, double range) {
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
      << R"(" role="img" aria-label="Distribution of the wall thickness" )"
      << R"(font-family="Helvetica Neue, Arial, sans-serif" font-size="11">)";
  const auto x = [&](double mm) { return kLeft + mm / range * (kRight - kLeft); };
  const auto shade = [&](double from, double to) {
    svg << R"(<rect x=")" << x(from) << R"(" y=")" << kTop << R"(" width=")" << x(to) - x(from)
        << R"(" height=")" << kBottom - kTop << R"(" fill="#c828aa" fill-opacity="0.08"/>)";
  };
  if (range > 0.0 && min_wall > 0.0) {
    shade(0.0, std::min(min_wall, range));
  }
  if (range > 0.0 && max_wall > 0.0 && max_wall < range) {
    shade(max_wall, range);
  }
  double peak = 0.0;
  for (const double f : histogram) {
    peak = std::max(peak, f);
  }
  const auto bins = histogram.size();
  for (std::size_t i = 0; i < bins && peak > 0.0 && range > 0.0; ++i) {
    const double lo = range * static_cast<double>(i) / static_cast<double>(bins);
    const double hi = lo + range / static_cast<double>(bins);
    const double h = histogram[i] / peak * (kBottom - kTop);
    svg << R"(<rect x=")" << x(lo) + 0.5 << R"(" y=")" << kBottom - h << R"(" width=")"
        << std::max(0.0, x(hi) - x(lo) - 1.0) << R"(" height=")" << h << R"(" fill=")"
        << hexColor(thicknessColor(0.5 * (lo + hi), min_wall, max_wall, range)) << R"("/>)";
  }
  svg << R"(<line x1=")" << kLeft << R"(" y1=")" << kBottom << R"(" x2=")" << kRight << R"(" y2=")"
      << kBottom << R"(" stroke="#5f6368"/>)";
  std::vector<double> ticks{0.0, 0.5 * range, range};
  for (const double limit : {min_wall, max_wall}) {
    if (limit > 0.05 * range && limit < 0.95 * range &&
        std::abs(limit - 0.5 * range) > 0.1 * range) {
      ticks.push_back(limit);
    }
  }
  for (const double tick : ticks) {
    const char* anchor = tick <= 0.0 ? "start" : (tick >= range ? "end" : "middle");
    svg << R"(<line x1=")" << x(tick) << R"(" y1=")" << kBottom << R"(" x2=")" << x(tick)
        << R"(" y2=")" << kBottom + 4.0 << R"(" stroke="#5f6368"/>)";
    svg << R"(<text x=")" << x(tick) << R"(" y=")" << kBottom + 17.0 << R"(" text-anchor=")"
        << anchor << R"(" fill="#1d1d1f">)" << formatNumber(tick, 3) << "</text>";
  }
  svg << R"(<text x=")" << 0.5 * (kLeft + kRight) << R"(" y=")" << kBottom + 34.0
      << R"(" text-anchor="middle" fill="#5f6368">Wall thickness in mm (the last bin includes values beyond)</text>)";
  svg << "</svg>";
  return svg.str();
}

struct Field {
  std::string_view path;
  std::string_view label;
};
/// Mandatory fields of the report, from the report contents required by ISO/IEC 17025
/// (7.8.2.1) and the CT-specific settings a test report states under ISO 15708-3.
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

/// Mandatory fields of a first article inspection report: what identifies the part, its drawing
/// and the submission, as the common first article forms ask for it.
constexpr std::array kFirstArticleFields{
    Field{"report.number", "Report number"},
    Field{"report.date", "Date of issue"},
    Field{"supplier.name", "Supplier"},
    Field{"customer.name", "Customer"},
    Field{"part.name", "Part name"},
    Field{"part.number", "Part number"},
    Field{"part.drawing", "Drawing number"},
    Field{"part.revision", "Drawing revision"},
    Field{"part.material", "Material"},
    Field{"part.serial", "Serial number or batch"},
    Field{"submission.reason", "Reason for the submission"},
    Field{"approval.inspector", "Inspector"},
};

void completeMandatory(Json& data, std::span<const Field> fields,
                       std::vector<std::string>* warnings) {
  for (const Field& field : fields) {
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
  completeMandatory(data, kMandatoryFields, warnings);
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
  if (!result.material_levels.empty()) {
    Json levels = Json::object();
    for (const auto& [id, level] : result.material_levels) {
      levels[std::to_string(id)] = formatNumber(level, 0);
    }
    data["analysis"]["material_levels"] = levels;
  }
  numberSections(data);
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
  numberSections(data);
}

namespace {

/// Slice through the middle of `dataset` normal to `axis` with the material classes blended
/// over the grey values, from the coarsest level that still has 900 pixels along its longer side.
RenderImage materialSlice(const Dataset& dataset, const MaterialVolume& materials, int axis) {
  const DatasetInfo& info = dataset.info();
  const auto axes = sliceAxes(axis);
  const auto u = static_cast<std::size_t>(axes[0]);
  const auto v = static_cast<std::size_t>(axes[1]);
  int level = 0;
  while (level + 1 < static_cast<int>(info.levels.size()) &&
         (std::max(info.dims[u], info.dims[v]) >> (level + 1)) >= 900) {
    ++level;
  }
  SliceRequest request;
  request.axis = axis;
  request.index = info.dims[static_cast<std::size_t>(axis)] / 2;
  request.level = level;
  const std::int64_t step = std::int64_t{1} << level;
  request.size = {(info.dims[u] + step - 1) / step, (info.dims[v] + step - 1) / step};
  const SliceImage slice = readSlice(dataset, request, nullptr, &materials);

  std::vector<float> material_values;
  for (const float g : slice.grey) {
    if (g > info.threshold) {
      material_values.push_back(g);
    }
  }
  const float low = info.air_level;
  float high = low + 1.0F;
  if (!material_values.empty()) {
    const auto top =
        material_values.begin() +
        static_cast<std::ptrdiff_t>(0.995 * static_cast<double>(material_values.size()));
    std::nth_element(material_values.begin(), top, material_values.end());
    high = std::max(high, *top);
  }
  std::map<int, std::array<std::uint8_t, 3>> colors;
  for (const Material& material : materials.info().materials) {
    colors[material.id] = material.color;
  }
  RenderImage image;
  image.width = static_cast<int>(slice.width);
  image.height = static_cast<int>(slice.height);
  image.pixels.resize(static_cast<std::size_t>(slice.width * slice.height) * 3);
  for (std::size_t i = 0; i < slice.grey.size(); ++i) {
    const double grey = 255.0 * std::clamp((slice.grey[i] - low) / (high - low), 0.0F, 1.0F);
    const int overlay = slice.overlay[i];
    const int first_material = static_cast<int>(SliceOverlay::kMaterial);
    const auto color =
        overlay > first_material ? colors.find(overlay - first_material) : colors.end();
    for (std::size_t c = 0; c < 3; ++c) {
      const double value = color == colors.end()
                               ? grey
                               : 0.55 * grey + 0.45 * color->second[c] * (grey / 255.0 + 0.5);
      image.pixels[3 * i + c] = static_cast<std::uint8_t>(std::clamp(value, 0.0, 255.0));
    }
  }
  return image;
}

Json readJsonFile(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot read " + file.string());
  }
  return Json::parse(in);
}

std::string percentText(double fraction) { return formatNumber(100.0 * fraction, 1) + " %"; }

}  // namespace

void addMaterials(Json& data, const std::filesystem::path& dir, const Dataset* dataset) {
  const MaterialVolume materials = MaterialVolume::open(dir);
  const MaterialVolumeInfo& info = materials.info();
  double total = 0.0;
  for (const Material& material : info.materials) {
    total += material.volume_mm3;
  }
  const Json levels = data.contains("analysis")
                          ? data["analysis"].value("material_levels", Json::object())
                          : Json::object();
  Json classes = Json::array();
  for (std::size_t i = 0; i < info.materials.size(); ++i) {
    const Material& material = info.materials[i];
    std::string range = "from " + formatNumber(material.lower, 1);
    if (i + 1 < info.materials.size()) {
      range =
          formatNumber(material.lower, 1) + " to " + formatNumber(info.materials[i + 1].lower, 1);
    }
    Json row = {{"id", material.id},
                {"name", material.name},
                {"color", hexColor(material.color)},
                {"grey_values", info.model.empty() ? range : "learned model"},
                {"volume", formatNumber(material.volume_mm3, 2)},
                {"share", percentText(total > 0.0 ? material.volume_mm3 / total : 0.0)}};
    if (const auto level = levels.find(std::to_string(material.id)); level != levels.end()) {
      row["porosity_level"] = *level;
    }
    classes.push_back(row);
  }
  data["materials"] = {
      {"method", info.model.empty()
                     ? "grey value thresholds on the locally averaged grey value, air below " +
                           formatNumber(info.air_threshold, 1)
                     : "learned model " + info.model},
      {"classes", classes},
      {"volume", formatNumber(total, 1) + " mm³"},
      {"per_material_porosity", !levels.empty()}};
  if (dataset != nullptr) {
    if (dataset->info().dims != info.dims) {
      throw std::invalid_argument("The material volume does not match the dataset");
    }
    for (int axis = 0; axis < 3; ++axis) {
      data["images"][std::string("materials_") + "xyz"[axis]] =
          dataUri(materialSlice(*dataset, materials, axis));
    }
  }
  numberSections(data);
}

void addFibreOrientation(Json& data, const std::filesystem::path& dir, const Json& order) {
  const Json o = readJsonFile(dir / "orientation.json");
  const auto direction_text = [](const std::array<double, 3>& d) {
    return formatSigned(d[0], 3) + ", " + formatSigned(d[1], 3) + ", " + formatSigned(d[2], 3);
  };
  std::vector<std::array<double, 3>> directions;
  std::vector<double> fractions;
  Json families = Json::array();
  for (const Json& family : o.at("families")) {
    const auto d = family.at("direction").get<std::array<double, 3>>();
    directions.push_back(d);
    fractions.push_back(family.at("fraction").get<double>());
    std::string axes;
    for (int a = 0; a < 3; ++a) {
      std::array<double, 3> axis{};
      axis[static_cast<std::size_t>(a)] = 1.0;
      axes += std::string(a > 0 ? ", " : "") + "xyz"[a] + " " +
              formatNumber(axialAngleDeg(d, axis), 1) + "°";
    }
    families.push_back({{"id", family.at("id")},
                        {"direction", direction_text(d)},
                        {"angles", axes},
                        {"share", percentText(family.at("fraction").get<double>())},
                        {"spread", formatNumber(family.at("spread_deg").get<double>(), 1) + "°"},
                        {"blocks", family.at("blocks")}});
  }
  const auto voxel_size = o.at("voxel_size_mm").get<VoxelSize>();
  const int block = o.at("block_size").get<int>();
  const int material = o.value("material", 0);
  std::string material_name = material == 0 ? "all material above the air threshold"
                                            : "material " + std::to_string(material);
  if (data.contains("materials")) {
    for (const Json& c : data["materials"]["classes"]) {
      if (c.value("id", 0) == material) {
        material_name = c.at("name").get<std::string>();
      }
    }
  }
  std::string block_text = formatNumber(block * voxel_size[0], 2);
  if (!voxel_size.isotropic()) {
    block_text += " × " + formatNumber(block * voxel_size[1], 2) + " × " +
                  formatNumber(block * voxel_size[2], 2);
  }
  std::string eigenvalues;
  for (const double value : o.at("tensor_eigenvalues")) {
    eigenvalues += (eigenvalues.empty() ? "" : " / ") + formatNumber(value, 3);
  }
  data["fibres"] = {{"block", std::to_string(block) + " voxels (" + block_text + " mm)"},
                    {"material", material_name},
                    {"blocks", o.value("analysed_blocks", 0)},
                    {"coherence", formatNumber(o.value("mean_coherence", 0.0), 2)},
                    {"eigenvalues", eigenvalues},
                    {"families", families}};
  for (const char axis : {'x', 'y', 'z'}) {
    data["images"][std::string("orientation_") + axis] =
        dataUri(dir / (std::string("orientation_") + axis + ".png"));
  }

  const Json required = order.is_object() && order.contains("fibres")
                            ? order["fibres"].value("families", Json::array())
                            : Json::array();
  if (!required.empty()) {
    if (directions.empty()) {
      throw std::invalid_argument("The fibre orientation found no fibre families to evaluate");
    }
    Json criteria = Json::array();
    bool passed = true;
    const auto add = [&](const std::string& label, const std::string& measured,
                         const std::string& limit, bool ok) {
      criteria.push_back({{"label", label},
                          {"measured", measured},
                          {"limit", limit},
                          {"passed", ok},
                          {"verdict", ok ? "passed" : "failed"}});
      passed = passed && ok;
    };
    for (const Json& family : required) {
      const auto nominal = family.at("direction").get<std::array<double, 3>>();
      const std::string name = family.value("name", "Fibre family " + direction_text(nominal));
      std::size_t best = 0;
      for (std::size_t i = 1; i < directions.size(); ++i) {
        if (axialAngleDeg(directions[i], nominal) < axialAngleDeg(directions[best], nominal)) {
          best = i;
        }
      }
      const double angle = axialAngleDeg(directions[best], nominal);
      if (family.contains("tolerance_deg")) {
        const double tolerance = family.at("tolerance_deg").get<double>();
        add(name + ": deviation of the direction", formatNumber(angle, 1) + "°",
            "≤ " + formatNumber(tolerance, 1) + "°", angle <= tolerance);
      }
      const double share = 100.0 * fractions[best];
      if (family.contains("min_share_percent")) {
        const double limit = family.at("min_share_percent").get<double>();
        add(name + ": share", formatNumber(share, 1) + " %", "≥ " + formatNumber(limit, 1) + " %",
            share >= limit);
      }
      if (family.contains("max_share_percent")) {
        const double limit = family.at("max_share_percent").get<double>();
        add(name + ": share", formatNumber(share, 1) + " %", "≤ " + formatNumber(limit, 1) + " %",
            share <= limit);
      }
    }
    const Json block_json = {{"name", "Fibre orientation"},
                             {"region", "fibre families of the " + material_name},
                             {"criteria", criteria},
                             {"passed", passed},
                             {"verdict", passed ? "passed" : "failed"}};
    if (!data.contains("evaluation")) {
      data["evaluation"] = {
          {"guideline", order["fibres"].value("guideline", std::string("the inspection order"))},
          {"zones", Json::array()},
          {"passed", true}};
    }
    Json& evaluation = data["evaluation"];
    evaluation["zones"].push_back(block_json);
    evaluation["passed"] = evaluation.value("passed", true) && passed;
    evaluation["verdict"] = evaluation["passed"].get<bool>() ? "The requirements are met."
                                                             : "The requirements are not met.";
  }
  numberSections(data);
}

void addWallThickness(Json& data, const std::filesystem::path& dir, const Json& order) {
  const Json w = readJsonFile(dir / "wall_thickness.json");
  const ThicknessMesh measured = readThicknessPly(dir / "thickness.ply");
  const Json required = order.is_object() && order.contains("wall_thickness")
                            ? order["wall_thickness"]
                            : Json::object();
  // The limits of the order, else those the analysis ran with.
  const double min_wall = required.value("min_mm", w.value("min_wall_mm", 0.0));
  const double max_wall = required.value("max_mm", w.value("max_wall_mm", 0.0));
  const ThicknessStats s = thicknessStats(measured.mesh, measured.thickness_mm, min_wall, max_wall);
  const auto mm = [](double value) { return formatNumber(value, 3) + " mm"; };
  const auto region_rows = [&](const std::vector<ThicknessRegion>& regions) {
    Json rows = Json::array();
    for (std::size_t i = 0; i < std::min<std::size_t>(regions.size(), 10); ++i) {
      const ThicknessRegion& r = regions[i];
      rows.push_back({{"id", r.id},
                      {"thickness", mm(r.extreme_mm)},
                      {"area", formatNumber(r.area_mm2, 2) + " mm²"},
                      {"location", formatNumber(r.location_mm[0], 2) + ", " +
                                       formatNumber(r.location_mm[1], 2) + ", " +
                                       formatNumber(r.location_mm[2], 2)}});
    }
    return rows;
  };
  const bool ray = w.value("method", std::string("ray")) == "ray";
  const auto percentiles = [&](std::size_t i) { return mm(s.percentiles_mm[i]); };
  Json section = {
      {"method", ray ? "Ray along the inward surface normal to the opposite face of the wall, "
                       "opposite face within " +
                           formatNumber(w.value("search_angle_deg", 0.0), 0) + "° of the ray"
                     : "Diameter of the largest sphere inside the material that touches the "
                       "surface"},
      {"max_thickness", mm(w.value("max_thickness_mm", 0.0))},
      {"area", formatNumber(s.area_mm2, 0) + " mm²"},
      {"measured", percentText(s.area_mm2 > 0.0 ? s.measured_area_mm2 / s.area_mm2 : 0.0)},
      {"dropped", w.value("dropped_components", 0)},
      {"min", mm(s.min_mm)},
      {"p5", percentiles(1)},
      {"median", percentiles(2)},
      {"mean", mm(s.mean_mm)},
      {"p95", percentiles(3)},
      {"max", mm(s.max_mm)},
      {"range", mm(s.range_mm)}};
  if (min_wall > 0.0) {
    const std::vector<ThicknessRegion> thin =
        thinRegions(measured.mesh, measured.thickness_mm, min_wall);
    section["min_wall"] = {{"limit", mm(min_wall)},
                           {"share", percentText(s.below_min)},
                           {"area", formatNumber(s.below_min * s.measured_area_mm2, 2) + " mm²"},
                           {"count", thin.size()},
                           {"has_regions", !thin.empty()},
                           {"regions", region_rows(thin)}};
  }
  if (max_wall > 0.0) {
    const std::vector<ThicknessRegion> thick =
        thickRegions(measured.mesh, measured.thickness_mm, max_wall);
    section["max_wall"] = {{"limit", mm(max_wall)},
                           {"share", percentText(s.above_max)},
                           {"area", formatNumber(s.above_max * s.measured_area_mm2, 2) + " mm²"},
                           {"count", thick.size()},
                           {"has_regions", !thick.empty()},
                           {"regions", region_rows(thick)}};
  }
  data["wall_thickness"] = section;
  // The views in the colours of these limits.
  RenderScene scene;
  scene.mesh = &measured.mesh;
  for (const float t : measured.thickness_mm) {
    scene.vertex_colors.push_back(thicknessColor(t, min_wall, max_wall, s.range_mm));
  }
  for (std::size_t k = 0; k < kDeviationViews.size() && !measured.mesh.points.empty(); ++k) {
    RenderView view;
    view.azimuth_degrees = kDeviationViews[k][0];
    view.elevation_degrees = kDeviationViews[k][1];
    data["images"]["thickness_" + std::to_string(k + 1)] = dataUri(render(scene, view));
  }
  data["images"]["thickness_histogram"] =
      thicknessHistogramSvg(s.histogram, min_wall, max_wall, s.range_mm);

  // The thickness map of the whole part, when the analysis wrote one.
  std::optional<Json> map;
  bool map_capped = false;
  double map_max = 0.0;
  if (std::filesystem::exists(dir / "thickness_map.json")) {
    map = readJsonFile(dir / "thickness_map.json");
    const Json& t = map->at("thickness");
    const double cap = map->at("max_thickness_mm").get<double>();
    map_max = t.at("max_mm").get<double>();
    map_capped = t.value("capped", 0.0) > 0.0;
    // Shares beyond the limits of the order from the fine histogram.
    const auto fine = t.at("fine_histogram").get<std::vector<std::int64_t>>();
    const double bin = t.at("fine_bin_mm").get<double>();
    const double voxels = std::max(1.0, t.at("material_voxels").get<double>());
    const auto share = [&](double limit, bool below) {
      double count = 0.0;
      for (std::size_t i = 0; i < fine.size(); ++i) {
        const double centre = (static_cast<double>(i) + 0.5) * bin;
        if (below ? centre < limit : centre > limit) {
          count += static_cast<double>(fine[i]);
        }
      }
      return count / voxels;
    };
    const auto percentile = [&](const char* key) {
      return mm(t.at("percentiles_mm").at(key).get<double>());
    };
    Json m = {{"max_thickness", mm(cap)},
              {"volume", formatNumber(t.at("material_volume_mm3").get<double>(), 0) + " mm³"},
              {"p5", percentile("5")},
              {"median", percentile("50")},
              {"p95", percentile("95")},
              {"max", (map_capped ? "≥ " : "") + mm(map_max)}};
    // Regions were found with the limits of the analysis; they are listed when the order's are
    // the same.
    const auto region_table = [&](const char* key, double limit, double analysed) {
      Json rows = Json::array();
      if (limit != analysed) {
        return rows;
      }
      for (const Json& r : map->at(key)) {
        if (rows.size() >= 10) {
          break;
        }
        const auto location = r.at("location_mm").get<std::array<double, 3>>();
        rows.push_back(
            {{"id", r.at("id")},
             {"thickness", mm(r.at("thickness_mm").get<double>())},
             {"volume", formatNumber(r.at("volume_mm3").get<double>(), 2) + " mm³"},
             {"location", formatNumber(location[0], 2) + ", " + formatNumber(location[1], 2) +
                              ", " + formatNumber(location[2], 2)}});
      }
      return rows;
    };
    if (min_wall > 0.0) {
      const Json rows = region_table("thin_regions", min_wall, map->value("min_wall_mm", 0.0));
      m["thin"] = {{"limit", mm(min_wall)},
                   {"share", percentText(share(min_wall, true))},
                   {"has_regions", !rows.empty()},
                   {"regions", rows}};
    }
    if (max_wall > 0.0) {
      const Json rows = region_table("thick_regions", max_wall, map->value("max_wall_mm", 0.0));
      m["thick"] = {{"limit", mm(max_wall)},
                    {"share", percentText(share(max_wall, false))},
                    {"has_regions", !rows.empty()},
                    {"regions", rows}};
    }
    data["wall_thickness"]["map"] = m;
    for (const char* axis : {"x", "y", "z"}) {
      const auto file = dir / (std::string("thickness_map_") + axis + ".png");
      if (std::filesystem::exists(file)) {
        data["images"][std::string("thickness_map_") + axis] = dataUri(file);
      }
    }
  }

  if (required.contains("min_mm") || required.contains("max_mm")) {
    Json criteria = Json::array();
    bool passed = true;
    const auto add = [&](const std::string& label, const std::string& measured_text,
                         const std::string& limit, bool ok) {
      criteria.push_back({{"label", label},
                          {"measured", measured_text},
                          {"limit", limit},
                          {"passed", ok},
                          {"verdict", ok ? "passed" : "failed"}});
      passed = passed && ok;
    };
    if (s.measured_area_mm2 <= 0.0) {
      throw std::invalid_argument("The wall thickness analysis measured no wall to evaluate");
    }
    if (required.contains("min_mm")) {
      if (required.contains("max_area_below_min_mm2")) {
        // Small thin spots may be tolerated, such as at sharp edges.
        const double limit = required.at("max_area_below_min_mm2").get<double>();
        const double area = s.below_min * s.measured_area_mm2;
        add("Area thinner than " + mm(min_wall), formatNumber(area, 2) + " mm²",
            "≤ " + formatNumber(limit, 2) + " mm²", area <= limit);
      } else {
        add("Thinnest wall", mm(s.min_mm), "≥ " + mm(min_wall), s.min_mm >= min_wall);
      }
    }
    if (required.contains("max_mm")) {
      add("Thickest wall", mm(s.max_mm), "≤ " + mm(max_wall), s.max_mm <= max_wall);
      if (map) {
        // Material accumulations inside the part. A map that reached its maximum only tells
        // that the material is at least that thick, which does not show it within the limit.
        add("Thickest spot inside the part", (map_capped ? "≥ " : "") + mm(map_max),
            "≤ " + mm(max_wall), !map_capped && map_max <= max_wall);
      }
    }
    const Json block_json = {{"name", "Wall thickness"},
                             {"region", "measured surface of the part"},
                             {"criteria", criteria},
                             {"passed", passed},
                             {"verdict", passed ? "passed" : "failed"}};
    if (!data.contains("evaluation")) {
      data["evaluation"] = {
          {"guideline", required.value("guideline", std::string("the inspection order"))},
          {"zones", Json::array()},
          {"passed", true}};
    }
    Json& evaluation = data["evaluation"];
    evaluation["zones"].push_back(block_json);
    evaluation["passed"] = evaluation.value("passed", true) && passed;
    evaluation["verdict"] = evaluation["passed"].get<bool>() ? "The requirements are met."
                                                             : "The requirements are not met.";
  }
  numberSections(data);
}

namespace {

/// GD&T-like symbol of a characteristic type for the table.
std::string_view characteristicSymbol(const std::string& type) {
  static const std::map<std::string, std::string_view, std::less<>> symbols = {
      {"diameter", "⌀"},   {"radius", "R"},   {"distance", "↔"},    {"angle", "∠"},
      {"coordinate", "⌗"}, {"position", "⌖"}, {"flatness", "⏥"},    {"cylindricity", "⌭"},
      {"sphericity", "◯"}, {"form", "◯"},     {"parallelism", "∥"}, {"perpendicularity", "⟂"},
      {"profile", "⌓"},    {"deviation", "±"}};
  const auto found = symbols.find(type);
  return found == symbols.end() ? std::string_view() : found->second;
}

/// A value of a characteristic: angles and lengths with three decimals.
std::string characteristicNumber(const Json& value, bool sign) {
  if (!value.is_number()) {
    return {};
  }
  return sign ? formatSigned(value.get<double>(), 3) : formatNumber(value.get<double>(), 3);
}

}  // namespace

nlohmann::json firstArticleData(const nlohmann::json& order,
                                const std::filesystem::path& measurement_dir,
                                const nlohmann::json& porosity,
                                std::vector<std::string>* warnings) {
  std::ifstream in(measurement_dir / "measurement.json", std::ios::binary);
  if (!in) {
    throw std::runtime_error("No measurement.json in " + measurement_dir.string());
  }
  const Json measurement = Json::parse(in);
  Json data = order.is_object() ? order : Json::object();
  // The inspection plan may name the part and its drawing; the order wins.
  if (const auto part = measurement.find("part"); part != measurement.end() && part->is_object()) {
    Json merged = *part;
    if (data.contains("part") && data["part"].is_object()) {
      merged.merge_patch(data["part"]);
    }
    data["part"] = merged;
  }
  completeMandatory(data, kFirstArticleFields, warnings);
  data["software"] = {{"name", "VoxelSieve"}, {"version", VOXELSIEVE_VERSION}};

  Json characteristics = Json::array();
  for (const Json& c : measurement.value("characteristics", Json::array())) {
    const std::string type = c.value("type", "");
    const Json passed = c.value("passed", Json());
    std::string result_class = "open";
    if (passed.is_boolean()) {
      result_class = passed.get<bool>() ? "ok" : "nok";
    }
    std::string relation;
    for (const Json& id : c.value("elements", Json::array())) {
      relation += (relation.empty() ? "" : ", ") + id.get<std::string>();
    }
    if (c.contains("datum")) {
      relation += " | " + c.at("datum").get<std::string>();
    }
    if (c.contains("along")) {
      relation += " (" + c.at("along").get<std::string>() + ")";
    }
    if (c.contains("axis")) {
      relation += " (" + c.at("axis").get<std::string>() + ")";
    }
    Json row = {{"number", c.value("number", "")},
                {"label", c.value("label", "")},
                {"type", type},
                {"symbol", std::string(characteristicSymbol(type))},
                {"relation", relation},
                {"nominal", characteristicNumber(c.value("nominal", Json()), false)},
                {"upper", characteristicNumber(c.value("upper", Json()), true)},
                {"lower", characteristicNumber(c.value("lower", Json()), true)},
                {"actual", characteristicNumber(c.value("actual", Json()), false)},
                {"deviation", characteristicNumber(c.value("deviation", Json()), true)},
                {"unit", c.value("unit", "mm") == "deg" ? "°" : "mm"},
                {"result", c.value("result", "")},
                {"result_class", result_class}};
    for (const char* key : {"drawing_zone", "method", "note", "message"}) {
      if (c.contains(key)) {
        row[key] = c.at(key);
      }
    }
    characteristics.push_back(row);
  }
  data["characteristics"] = characteristics;

  Json elements = Json::array();
  for (const Json& e : measurement.value("elements", Json::array())) {
    Json row = {{"id", e.value("id", "")},
                {"type", e.value("type", "")},
                {"source", e.value("source", "") == "cad" ? "CAD model" : "given"},
                {"measured", e.value("measured", false)}};
    if (e.value("measured", false)) {
      row["points"] = e.value("points", 0);
      row["rms"] = formatNumber(e.value("rms_mm", 0.0), 4);
      row["form"] = formatNumber(e.value("form_mm", 0.0), 4);
    } else {
      row["message"] = e.value("message", "");
    }
    elements.push_back(row);
  }
  data["elements"] = elements;

  const Json& alignment = measurement.at("alignment");
  std::string alignment_text = "best fit of the surface to the CAD model";
  if (alignment.value("method", "") == "datums") {
    std::string datums;
    for (const Json& d : alignment.value("datums", Json::array())) {
      datums += (datums.empty() ? "" : ", ") + d.value("element", "");
    }
    alignment_text = "on the datums " + datums + " in this order (" +
                     std::to_string(alignment.value("fixed_degrees_of_freedom", 0)) +
                     " degrees of freedom fixed, the others from the best fit)";
  }
  const Json& summary = measurement.at("summary");
  const auto count = [&](const char* key) { return summary.value(key, std::size_t{0}); };
  const std::size_t failed = count("failed");
  const std::size_t not_measured = count("not_measured");
  data["measurement"] = {
      {"alignment", alignment_text},
      {"voxel_size", voxelSizeText(readVoxelSize(measurement))},
      {"search", formatNumber(measurement.value("search_mm", 0.0), 3) + " mm"},
      {"edge_margin", formatNumber(measurement.value("edge_margin_mm", 0.0), 3) + " mm"},
      {"cad",
       std::filesystem::path(measurement.value("cad_path", std::string())).filename().string()},
      {"characteristics", count("characteristics")},
      {"passed", count("passed")},
      {"failed", failed},
      {"not_measured", not_measured},
      {"unrated", count("unrated")}};
  // The decision on the dimensional result; porosity limits, when evaluated, count as well.
  const bool conforming = failed == 0;
  bool porosity_failed = false;
  if (porosity.is_object()) {
    data["porosity"] = {{"results", porosity.at("results")}, {"analysis", porosity.at("analysis")}};
    if (const auto evaluation = porosity.find("evaluation"); evaluation != porosity.end()) {
      data["porosity"]["evaluation"] = *evaluation;
      porosity_failed = !evaluation->value("passed", true);
    }
    if (const auto images = porosity.find("images"); images != porosity.end()) {
      for (const char* key : {"part", "pores"}) {
        if (images->contains(key)) {
          data["images"][key] = images->at(key);
        }
      }
    }
  }
  std::string decision = "conforming";
  std::string decision_text = "All measured characteristics are within their tolerances.";
  if (!conforming || porosity_failed) {
    decision = "not conforming";
    decision_text.clear();
    if (failed > 0) {
      decision_text = std::to_string(failed) + " characteristic" + (failed == 1 ? " is" : "s are") +
                      " out of tolerance.";
    }
    if (porosity_failed) {
      decision_text += std::string(decision_text.empty() ? "" : " ") +
                       "The porosity exceeds the acceptance limits.";
    }
  } else if (not_measured > 0) {
    decision = "incomplete";
    decision_text = std::to_string(not_measured) + " characteristic" +
                    (not_measured == 1 ? " was" : "s were") + " not measured.";
  }
  data["decision"] = {{"result", decision},
                      {"text", decision_text},
                      {"class", decision == "conforming"       ? "ok"
                                : decision == "not conforming" ? "nok"
                                                               : "open"}};
  data["images"]["measurement_1"] = dataUri(measurement_dir / "measurement_view_1.png");
  data["images"]["measurement_2"] = dataUri(measurement_dir / "measurement_view_2.png");
  return data;
}

namespace {

/// A length in mm with three decimals, or "–" where nothing was measured.
std::string mmText(const Json& value) {
  return value.is_number() ? formatNumber(value.get<double>(), 3) + " mm" : "–";
}

/// The same without the unit, for table columns that name it in the header.
std::string numberText(const Json& value) {
  return value.is_number() ? formatNumber(value.get<double>(), 3) : "–";
}

std::string signedText(const Json& value) {
  return value.is_number() ? formatSigned(value.get<double>(), 3) : "–";
}

/// Colours of the edges in the overhang chart, in a fixed order.
constexpr std::array<std::string_view, 4> kEdgeColors = {"#2a78d6", "#eb6834", "#1baf7a",
                                                         "#eda100"};

/// Smallest overhang per section of every edge, as SVG: one series per edge over the angle
/// (cylindrical) or the position along the edge (stacked), with the required minimum dashed.
std::string overhangChartSvg(const Json& battery, const Json& limit) {
  constexpr double kWidth = 640.0;
  constexpr double kHeight = 230.0;
  constexpr double kLeft = 52.0;
  constexpr double kRight = 520.0;
  constexpr double kTop = 12.0;
  constexpr double kBottom = 190.0;
  const bool cylindrical = battery.at("type") == "cylindrical";
  const std::string key = cylindrical ? "angle_deg" : "position_mm";
  // Per edge: section -> smallest overhang.
  std::vector<std::map<double, double>> series(battery.at("edges").size());
  double xmin = std::numeric_limits<double>::infinity();
  double xmax = -xmin;
  double ymin = limit.is_number() ? limit.get<double>() : xmin;
  double ymax = limit.is_number() ? limit.get<double>() : -xmin;
  for (const Json& sample : battery.at("overhang")) {
    const auto edge = sample.at("edge").get<std::size_t>();
    const double x = sample.at(key).get<double>();
    const double y = sample.at("overhang_mm").get<double>();
    auto& points = series.at(edge);
    const auto it = points.find(x);
    points[x] = it == points.end() ? y : std::min(it->second, y);
    xmin = std::min(xmin, x);
    xmax = std::max(xmax, x);
    ymin = std::min(ymin, y);
    ymax = std::max(ymax, y);
  }
  if (!std::isfinite(xmin) || !std::isfinite(ymin)) {
    return {};
  }
  if (cylindrical) {
    xmin = 0.0;
    xmax = 360.0;
  }
  if (xmax <= xmin) {
    xmax = xmin + 1.0;
  }
  // Round ticks: steps of 1, 2 or 5 times a power of ten, zero included.
  ymin = std::min(ymin, 0.0);
  ymax = std::max(ymax, ymin + 0.05);
  const double raw_step = (ymax - ymin) / 4.0;
  const double magnitude = std::pow(10.0, std::floor(std::log10(raw_step)));
  double step = 10.0 * magnitude;
  for (const double f : {1.0, 2.0, 5.0}) {
    if (f * magnitude >= raw_step) {
      step = f * magnitude;
      break;
    }
  }
  ymin = std::floor(ymin / step) * step;
  ymax = std::ceil(ymax / step) * step;
  const int ticks = static_cast<int>(std::lround((ymax - ymin) / step));
  const auto px = [&](double x) { return kLeft + (x - xmin) / (xmax - xmin) * (kRight - kLeft); };
  const auto py = [&](double y) { return kBottom - (y - ymin) / (ymax - ymin) * (kBottom - kTop); };
  std::ostringstream svg;
  svg.setf(std::ios::fixed);
  svg.precision(2);
  svg << R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 )" << kWidth << ' ' << kHeight
      << R"(" role="img" aria-label="Smallest anode overhang per section and edge" )"
      << R"(font-family="Helvetica Neue, Arial, sans-serif" font-size="10" fill="#52514e">)";
  // Recessive grid with five values on the overhang axis; zero darker.
  for (int i = 0; i <= ticks; ++i) {
    const double y = ymin + step * i;
    svg << R"(<line x1=")" << kLeft << R"(" x2=")" << kRight << R"(" y1=")" << py(y) << R"(" y2=")"
        << py(y) << R"(" stroke="#e4e3df"/>)" << R"(<text x=")" << kLeft - 6 << R"(" y=")"
        << py(y) + 3 << R"(" text-anchor="end">)"
        << (std::abs(y) < 0.5 * step ? std::string("0") : formatSigned(y, step < 0.1 ? 2 : 1))
        << "</text>";
  }
  svg << R"(<line x1=")" << kLeft << R"(" x2=")" << kRight << R"(" y1=")" << py(0.0) << R"(" y2=")"
      << py(0.0) << R"(" stroke="#8a8984"/>)";
  for (int i = 0; i <= 4; ++i) {
    const double x = xmin + (xmax - xmin) * i / 4.0;
    svg << R"(<text x=")" << px(x) << R"(" y=")" << kBottom + 14 << R"(" text-anchor="middle">)"
        << formatNumber(x, cylindrical ? 0 : 1) << (cylindrical ? "°" : "") << "</text>";
  }
  svg << R"(<text x=")" << 0.5 * (kLeft + kRight) << R"(" y=")" << kBottom + 32
      << R"(" text-anchor="middle">)"
      << (cylindrical ? "Angle around the cell axis" : "Position along the edge in mm") << "</text>"
      << R"(<text x="12" y=")" << 0.5 * (kTop + kBottom)
      << R"(" text-anchor="middle" transform="rotate(-90 12 )" << 0.5 * (kTop + kBottom)
      << R"x()">Smallest overhang in mm</text>)x";
  if (limit.is_number()) {
    const double y = py(limit.get<double>());
    svg << R"(<line x1=")" << kLeft << R"(" x2=")" << kRight << R"(" y1=")" << y << R"(" y2=")" << y
        << R"(" stroke="#b3261e" stroke-width="1.5" stroke-dasharray="5 4"/>)" << R"(<text x=")"
        << kRight + 8 << R"(" y=")" << y + 3 << R"(" fill="#b3261e">minimum )"
        << formatNumber(limit.get<double>(), 2) << " mm</text>";
  }
  const Json& edges = battery.at("edges");
  for (std::size_t e = 0; e < series.size(); ++e) {
    const std::string color(kEdgeColors[e % kEdgeColors.size()]);
    const std::string name = edges[e].at("name").get<std::string>();
    std::string path;
    for (const auto& [x, y] : series[e]) {
      path += (path.empty() ? "M" : " L") + formatNumber(px(x), 2) + " " + formatNumber(py(y), 2);
    }
    if (series[e].size() > 1) {
      svg << R"(<path d=")" << path << R"(" fill="none" stroke=")" << color
          << R"(" stroke-width="2" stroke-linejoin="round"/>)";
    }
    for (const auto& [x, y] : series[e]) {
      svg << R"(<circle cx=")" << px(x) << R"(" cy=")" << py(y) << R"(" r="3" fill=")" << color
          << R"(" stroke="#fcfcfb" stroke-width="1"><title>)" << name << ": " << formatSigned(y, 3)
          << " mm at " << formatNumber(x, cylindrical ? 0 : 2) << (cylindrical ? "°" : " mm")
          << "</title></circle>";
    }
    // Legend on the right, text in ink, the swatch carries the colour.
    const double ly = kTop + 8.0 + 16.0 * static_cast<double>(e);
    svg << R"(<rect x=")" << kRight + 8 << R"(" y=")" << ly - 7
        << R"(" width="10" height="10" rx="2" fill=")" << color << R"("/><text x=")" << kRight + 24
        << R"(" y=")" << ly + 2 << R"(" fill="#0b0b0b">)" << name << "</text>";
  }
  svg << "</svg>";
  return svg.str();
}

/// One finding (particle or void) for the tables.
Json findingRow(const Json& f, bool cylindrical) {
  const auto c = f.at("center_mm").get<std::array<double, 3>>();
  Json row = {
      {"id", f.at("id")},
      {"position", formatMm(c)},
      {"size", formatNumber(f.at("size_mm").get<double>(), 3)},
      {"diameter", formatNumber(f.at("equivalent_diameter_mm").get<double>(), 3)},
      {"volume", f.at("volume_mm3").is_number() ? formatNumber(f.at("volume_mm3").get<double>(), 6)
                                                : std::string("–")},
      {"grey_value", formatNumber(f.at("grey_value").get<double>(), 0)}};
  row["in_cell"] = cylindrical ? "r " + formatNumber(f.value("radius_mm", 0.0), 2) + " mm, " +
                                     formatNumber(f.value("angle_deg", 0.0), 0) + "°"
                               : formatNumber(f.value("normal_mm", 0.0), 2) + " mm";
  return row;
}

}  // namespace

nlohmann::json batteryReportData(const nlohmann::json& order,
                                 const std::filesystem::path& battery_dir,
                                 std::vector<std::string>* warnings) {
  const Json b = readJsonFile(battery_dir / "battery.json");
  Json data = order.is_object() ? order : Json::object();
  completeMandatory(data, kMandatoryFields, warnings);
  data["software"] = {{"name", "VoxelSieve"}, {"version", VOXELSIEVE_VERSION}};
  const bool cylindrical = b.at("type") == "cylindrical";
  const auto voxel_size = b.at("voxel_size_mm").get<VoxelSize>();
  const std::string axis(1, "xyz"[b.at("axis").get<int>()]);
  const Json& levels = b.at("levels");
  const auto level = [&](const char* name) {
    return levels.at(name).is_number() ? formatNumber(levels.at(name).get<double>(), 0) : "–";
  };
  data["analysis"] = {
      {"voxel_size", voxelSizeText(voxel_size)},
      {"levels", "air " + level("air") + ", gaps " + level("gap") + ", anode " + level("anode") +
                     ", cathode " + level("cathode") + ", housing " + level("housing")},
      {"particle_threshold", formatNumber(b.at("particle_threshold").get<double>(), 0)},
      {"void_threshold", formatNumber(b.at("void_threshold").get<double>(), 0)},
      {"sections", b.at("sections").size()},
      {"overhang_samples", b.at("overhang").size()}};

  const auto size = b.at("outer_size_mm").get<std::array<double, 3>>();
  Json cell = {{"type", cylindrical ? "cylindrical, wound electrodes"
                                    : "stacked electrodes (pouch or prismatic)"},
               {"axis", axis},
               {"outer_size", formatNumber(size[0], 3) + " × " + formatNumber(size[1], 3) + " × " +
                                  formatNumber(size[2], 3) + " mm (x × y × z)"},
               {"length", mmText(b.at("length_mm"))},
               {"cathode_layers", b.at("cathode_layers")},
               {"anode_layers", b.at("anode_layers")},
               {"pitch", mmText(b.at("pitch_mm"))},
               {"min_wall", mmText(b.at("min_wall_mm"))},
               {"gaps", b.at("gaps").size()},
               {"distinct", levels.value("electrodes_distinct", false)},
               {"cylindrical", cylindrical},
               {"from_images", b.value("from_images", false)}};
  if (cylindrical) {
    cell["diameter"] = mmText(b.at("outer_mean_mm"));
    cell["out_of_roundness"] = mmText(b.at("max_out_of_roundness_mm"));
    cell["dent"] = mmText(b.at("max_dent_mm"));
    cell["core_out_of_roundness"] = mmText(b.at("max_core_out_of_roundness_mm"));
  } else {
    cell["thickness"] = mmText(b.at("outer_min_size_mm")) + " to " +
                        mmText(b.at("outer_max_size_mm")) + ", mean " +
                        mmText(b.at("outer_mean_mm"));
  }
  Json sections = Json::array();
  for (const Json& s : b.at("sections")) {
    Json row = {{"outer", numberText(s.at("outer_mm"))},
                {"wall", numberText(s.at("min_wall_mm"))},
                {"stack", numberText(s.at("stack_mm"))},
                {"layers", std::to_string(s.at("cathode_layers").get<int>()) + " / " +
                               std::to_string(s.at("anode_layers").get<int>())},
                {"pitch", numberText(s.at("pitch_mm"))}};
    if (cylindrical) {
      row["position"] = axis + " " + formatNumber(s.at("position_mm").get<double>(), 2);
      row["out_of_roundness"] = numberText(s.at("out_of_roundness_mm"));
      row["core"] = numberText(s.at("core_diameter_mm"));
    } else {
      const auto pos = s.at("position_mm").get<std::array<double, 2>>();
      row["position"] = formatNumber(pos[0], 2) + ", " + formatNumber(pos[1], 2);
    }
    sections.push_back(row);
  }
  cell["sections"] = sections;
  Json edges = Json::array();
  const Json& samples = b.at("overhang");
  for (const Json& e : b.at("edges")) {
    Json row = {{"name", e.at("name")},
                {"samples", e.at("samples")},
                {"min", signedText(e.at("min_mm"))},
                {"mean", signedText(e.at("mean_mm"))},
                {"max", signedText(e.at("max_mm"))},
                {"std", numberText(e.at("std_mm"))},
                {"cathode_spread", numberText(e.at("cathode_edge_spread_mm"))},
                {"anode_spread", numberText(e.at("anode_edge_spread_mm"))}};
    if (const auto worst = e.at("worst").get<std::int64_t>(); worst >= 0) {
      const Json& w = samples.at(static_cast<std::size_t>(worst));
      row["worst"] = "layer " + std::to_string(w.at("layer").get<int>() + 1) + ", " +
                     (cylindrical ? formatNumber(w.at("angle_deg").get<double>(), 0) + "°"
                                  : formatNumber(w.at("position_mm").get<double>(), 2) + " mm");
    }
    const std::string name = e.at("name").get<std::string>();  // "z max"
    const std::string file = "overhang_" + name.substr(0, 1) + "_" + name.substr(2) + ".png";
    row["image"] = dataUri(battery_dir / file);
    edges.push_back(row);
  }
  cell["edges"] = edges;
  data["battery"] = cell;
  const Json limit = order.is_object() && order.contains("battery")
                         ? order["battery"].value("min_overhang_mm", Json())
                         : Json();
  data["images"]["cross_section"] = dataUri(battery_dir / "cross_section.png");
  data["images"]["longitudinal"] = dataUri(battery_dir / "longitudinal.png");
  data["images"]["overhang_chart"] = overhangChartSvg(b, limit);

  Json particles = Json::array();
  for (const Json& f : b.at("particles")) {
    if (particles.size() < kMaxListedPores) {
      particles.push_back(findingRow(f, cylindrical));
    }
  }
  Json voids = Json::array();
  for (const Json& f : b.at("voids")) {
    if (voids.size() < kMaxListedPores) {
      voids.push_back(findingRow(f, cylindrical));
    }
  }
  data["particles"] = {{"count", b.at("particles").size()},
                       {"listed", particles},
                       {"any", !particles.empty()},
                       {"more", b.at("particles").size() > particles.size()}};
  data["voids"] = {{"count", b.at("voids").size()},
                   {"listed", voids},
                   {"any", !voids.empty()},
                   {"more", b.at("voids").size() > voids.size()}};
  data["warnings"] = b.value("warnings", Json::array());
  data["has_warnings"] = !data["warnings"].empty();

  // Requirements of the order.
  if (order.is_object() && order.contains("battery")) {
    const Json& r = order["battery"];
    Json criteria = Json::array();
    bool passed = true;
    const auto add = [&](const std::string& label, const Json& measured,
                         const std::string& limit_text, const std::function<bool(double)>& ok,
                         int decimals, const char* unit) {
      const bool known = measured.is_number();
      const bool good = known && ok(measured.get<double>());
      criteria.push_back({{"label", label},
                          {"measured", known ? formatNumber(measured.get<double>(), decimals) + unit
                                             : "not measured"},
                          {"limit", limit_text},
                          {"passed", good},
                          {"verdict", good ? "passed" : (known ? "failed" : "not measured")}});
      passed = passed && good;
    };
    const auto at_least = [&](const char* key, const std::string& label, const Json& measured,
                              int decimals, const char* unit) {
      if (r.contains(key)) {
        const double l = r.at(key).get<double>();
        add(
            label, measured, "≥ " + formatNumber(l, decimals) + unit,
            [l](double v) { return v >= l; }, decimals, unit);
      }
    };
    const auto at_most = [&](const char* key, const std::string& label, const Json& measured,
                             int decimals, const char* unit) {
      if (r.contains(key)) {
        const double l = r.at(key).get<double>();
        add(
            label, measured, "≤ " + formatNumber(l, decimals) + unit,
            [l](double v) { return v <= l; }, decimals, unit);
      }
    };
    const auto nominal = [&](const char* key, const std::string& label, const Json& measured) {
      if (!r.contains(key)) {
        return;
      }
      const Json& spec = r.at(key);
      if (spec.contains("nominal")) {
        const double n = spec.at("nominal").get<double>();
        const double t = spec.value("tolerance", 0.0);
        add(
            label, measured, formatNumber(n, 3) + " ± " + formatNumber(t, 3) + " mm",
            [n, t](double v) { return std::abs(v - n) <= t; }, 3, " mm");
      }
      if (spec.contains("min")) {
        const double l = spec.at("min").get<double>();
        add(
            label, measured, "≥ " + formatNumber(l, 3) + " mm", [l](double v) { return v >= l; }, 3,
            " mm");
      }
      if (spec.contains("max")) {
        const double l = spec.at("max").get<double>();
        add(
            label, measured, "≤ " + formatNumber(l, 3) + " mm", [l](double v) { return v <= l; }, 3,
            " mm");
      }
    };
    at_least("min_overhang_mm", "Smallest anode overhang, all edges", b.at("min_overhang_mm"), 3,
             " mm");
    if (r.contains("max_overhang_mm")) {
      double largest = -std::numeric_limits<double>::infinity();
      for (const Json& e : b.at("edges")) {
        if (e.at("max_mm").is_number()) {
          largest = std::max(largest, e.at("max_mm").get<double>());
        }
      }
      at_most("max_overhang_mm", "Largest anode overhang, all edges",
              std::isfinite(largest) ? Json(largest) : Json(), 3, " mm");
    }
    if (r.contains("max_edge_spread_mm")) {
      double spread = -std::numeric_limits<double>::infinity();
      for (const Json& e : b.at("edges")) {
        for (const char* k : {"cathode_edge_spread_mm", "anode_edge_spread_mm"}) {
          if (e.at(k).is_number()) {
            spread = std::max(spread, e.at(k).get<double>());
          }
        }
      }
      at_most("max_edge_spread_mm", "Unevenness of the electrode edges",
              std::isfinite(spread) ? Json(spread) : Json(), 3, " mm");
    }
    if (r.contains("cathode_layers")) {
      const Json& spec = r.at("cathode_layers");
      const int lo = spec.is_object() ? spec.value("min", 0) : spec.get<int>();
      const int hi = spec.is_object() ? spec.value("max", 1 << 20) : spec.get<int>();
      const std::string text = lo == hi ? std::to_string(lo)
                               : spec.contains("max")
                                   ? std::to_string(lo) + " to " + std::to_string(hi)
                                   : "≥ " + std::to_string(lo);
      add(
          cylindrical ? "Cathode layers (turns) per radius" : "Cathode layers",
          b.at("cathode_layers"), text, [lo, hi](double v) { return v >= lo && v <= hi; }, 0, "");
    }
    nominal("diameter_mm", "Outer diameter", b.at("outer_mean_mm"));
    nominal("length_mm", cylindrical ? "Length" : "Thickness at the middle", b.at("length_mm"));
    if (r.contains("thickness_mm")) {
      const Json& spec = r.at("thickness_mm");
      if (spec.contains("max")) {
        const double l = spec.at("max").get<double>();
        add(
            "Largest thickness", b.at("outer_max_size_mm"), "≤ " + formatNumber(l, 3) + " mm",
            [l](double v) { return v <= l; }, 3, " mm");
      }
      if (spec.contains("min")) {
        const double l = spec.at("min").get<double>();
        add(
            "Smallest thickness", b.at("outer_min_size_mm"), "≥ " + formatNumber(l, 3) + " mm",
            [l](double v) { return v >= l; }, 3, " mm");
      }
    }
    if (r.contains("max_swelling_mm")) {
      const Json& lo = b.at("outer_min_size_mm");
      const Json& hi = b.at("outer_max_size_mm");
      at_most("max_swelling_mm", "Thickness variation (swelling)",
              lo.is_number() && hi.is_number() ? Json(hi.get<double>() - lo.get<double>()) : Json(),
              3, " mm");
    }
    at_most("max_out_of_roundness_mm", "Out-of-roundness of the housing",
            b.at("max_out_of_roundness_mm"), 3, " mm");
    at_most("max_dent_mm", "Largest dent of the housing", b.at("max_dent_mm"), 3, " mm");
    at_least("min_wall_mm", "Thinnest housing wall", b.at("min_wall_mm"), 3, " mm");
    at_most("max_core_out_of_roundness_mm", "Out-of-roundness of the core",
            b.at("max_core_out_of_roundness_mm"), 3, " mm");
    at_most("max_gaps", "Gaps between the layers", Json(b.at("gaps").size()), 0, "");
    const auto largest = [&](const char* list) {
      double extent = 0.0;
      for (const Json& f : b.at(list)) {
        extent = std::max(extent, f.at("size_mm").get<double>());
      }
      return Json(extent);
    };
    at_most("max_particles", "Foreign particles", Json(b.at("particles").size()), 0, "");
    at_most("max_particle_size_mm", "Largest foreign particle", largest("particles"), 3, " mm");
    at_most("max_voids", "Gas voids", Json(b.at("voids").size()), 0, "");
    at_most("max_void_size_mm", "Largest gas void", largest("voids"), 3, " mm");
    if (!criteria.empty()) {
      data["evaluation"] = {
          {"guideline", r.value("guideline", std::string("the inspection order"))},
          {"criteria", criteria},
          {"passed", passed},
          {"verdict", passed ? "The requirements are met." : "The requirements are not met."}};
    }
  }
  return data;
}

}  // namespace voxelsieve
