#include "voxelsieve/report.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <numbers>
#include <sstream>
#include <stdexcept>

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxListedPores = 25;
constexpr std::string_view kNotGiven = "nicht angegeben";

/// Number with a decimal comma and thin spaces between thousands, as usual in German reports.
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
      grouped += " ";
    }
    grouped += integer[i];
  }
  std::string out = (value < 0.0 && std::stod(digits) != 0.0 ? "-" : "") + grouped;
  if (!fraction.empty()) {
    out += "," + fraction;
  }
  return out;
}

std::string formatMm(const std::array<double, 3>& p) {
  return formatNumber(p[0], 2) + "; " + formatNumber(p[1], 2) + "; " + formatNumber(p[2], 2);
}

bool insideBox(const std::array<double, 3>& p_mm, const std::array<std::array<double, 3>, 2>& box) {
  for (std::size_t a = 0; a < 3; ++a) {
    if (p_mm[a] < box[0][a] || p_mm[a] >= box[1][a]) {
      return false;
    }
  }
  return true;
}

std::array<double, 3> toMm(const std::array<double, 3>& voxels, double voxel_size) {
  return {voxels[0] * voxel_size, voxels[1] * voxel_size, voxels[2] * voxel_size};
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

/// Mandatory fields of the report, from the report contents required by DIN EN ISO/IEC 17025
/// (7.8.2.1) and the CT-specific settings a test report states under DIN EN ISO 15708-3.
struct Field {
  std::string_view path;
  std::string_view label;
};
constexpr std::array kMandatoryFields{
    Field{"report.number", "Berichtsnummer"},
    Field{"report.date", "Ausstellungsdatum"},
    Field{"laboratory.name", "Prüflaboratorium"},
    Field{"laboratory.address", "Anschrift des Prüflaboratoriums"},
    Field{"customer.name", "Auftraggeber"},
    Field{"order.number", "Auftragsnummer"},
    Field{"order.test_date", "Prüfdatum"},
    Field{"part.name", "Bezeichnung des Prüfgegenstands"},
    Field{"part.drawing", "Zeichnungsnummer"},
    Field{"part.material", "Werkstoff"},
    Field{"part.serial", "Seriennummer oder Charge"},
    Field{"scan.device", "CT-Anlage"},
    Field{"scan.voltage_kv", "Röhrenspannung"},
    Field{"scan.current_ua", "Röhrenstrom"},
    Field{"scan.filter", "Vorfilter"},
    Field{"scan.projections", "Anzahl Projektionen"},
    Field{"scan.exposure_ms", "Belichtungszeit"},
    Field{"scan.reconstruction", "Rekonstruktion"},
    Field{"uncertainty", "Messunsicherheit"},
    Field{"approval.approver", "Freigabe durch"},
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
                                ") fehlt");
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
    return "gesamtes Bauteil";
  }
  const auto& box = *zone.box_mm;
  return "Quader von (" + formatMm(box[0]) + ") bis (" + formatMm(box[1]) + ") mm";
}

std::string criterionLabel(const CriterionResult& criterion, const AcceptanceLimits& limits) {
  if (criterion.name == "max_pore_size_mm") {
    return "Größte Ausdehnung einer Pore";
  }
  if (criterion.name == "max_pore_count") {
    return limits.count_min_size_mm > 0.0
               ? "Anzahl Poren ab " + formatNumber(limits.count_min_size_mm, 2) + " mm"
               : "Anzahl Poren";
  }
  if (criterion.name == "max_porosity_percent") {
    return "Porosität (Poren und Gefügeauflockerung)";
  }
  return "Gefügeauflockerung";
}

std::string criterionValue(const CriterionResult& criterion, bool limit) {
  const double value = limit ? criterion.limit : criterion.measured;
  if (criterion.name == "max_pore_size_mm") {
    return !limit && value == 0.0 ? "keine Pore" : formatNumber(value, 2) + " mm";
  }
  if (criterion.name == "max_porosity_percent") {
    return formatNumber(value, 3) + " %";
  }
  if (criterion.name == "loosening") {
    if (limit) {
      return "nicht zulässig";
    }
    return value > 0.0 ? formatNumber(value, 0) + " Zone(n)" : "keine";
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

double poreSizeMm(const DetectedPore& pore, double voxel_size_mm) {
  std::int64_t longest = 0;
  for (std::size_t a = 0; a < 3; ++a) {
    longest = std::max(longest, pore.bounds.max[a] - pore.bounds.min[a]);
  }
  return static_cast<double>(longest) * voxel_size_mm;
}

Evaluation evaluate(const PorosityResult& result, const std::vector<InspectionZone>& zones) {
  const double v = result.voxel_size_mm;
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
  const double v = result.voxel_size_mm;
  const double voxel_volume = v * v * v;

  data["software"] = {{"name", "VoxelSieve"}, {"version", VOXELSIEVE_VERSION}};
  const double min_pore_volume = static_cast<double>(options.min_pore_voxels) * voxel_volume;
  data["analysis"] = {
      {"voxel_size", formatNumber(v * 1000.0, 1) + " µm"},
      {"min_pore_voxels", options.min_pore_voxels},
      {"min_pore_volume", formatNumber(min_pore_volume, 6) + " mm³"},
      {"min_pore_diameter",
       formatNumber(std::cbrt(6.0 * min_pore_volume / std::numbers::pi), 3) + " mm"},
      {"zone_block", formatNumber(8.0 * v, 2) + " mm"},
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
                            {"verdict", criterion.passed ? "erfüllt" : "nicht erfüllt"}});
      }
      evaluated.push_back({{"name", zone.name},
                           {"region", regionText(spec)},
                           {"part_volume", formatNumber(zone.part_volume_mm3, 1) + " mm³"},
                           {"criteria", criteria},
                           {"passed", zone.passed()},
                           {"verdict", zone.passed() ? "erfüllt" : "nicht erfüllt"}});
    }
    const std::string guideline = order.contains("acceptance")
                                      ? order["acceptance"].value("guideline", "BDG P 202")
                                      : "BDG P 202";
    data["evaluation"] = {
        {"guideline", guideline},
        {"zones", evaluated},
        {"passed", evaluation.passed()},
        {"verdict", evaluation.passed() ? "Die Anforderungen sind erfüllt."
                                        : "Die Anforderungen sind nicht erfüllt."}};
  }

  data["images"] = {{"x", dataUri(image_dir / "projection_x.png")},
                    {"y", dataUri(image_dir / "projection_y.png")},
                    {"z", dataUri(image_dir / "projection_z.png")}};
  return data;
}

}  // namespace voxelsieve
