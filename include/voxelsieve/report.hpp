#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "voxelsieve/porosity.hpp"

namespace voxelsieve {

/// Acceptance limits for one inspection zone, following the scheme of BDG P 202 (volume deficits
/// of castings): size of the largest pore, number of pores, porosity. Every limit is optional; the
/// values come from the drawing or the customer, not from VoxelSieve.
struct AcceptanceLimits {
  /// Largest extent of a single pore (longest edge of its bounding box).
  std::optional<double> max_pore_size_mm;
  /// Number of pores whose extent is at least `count_min_size_mm`.
  std::optional<std::int64_t> max_pore_count;
  double count_min_size_mm = 0.0;
  /// Void volume of pores and loosened zones relative to the part volume in the zone, in percent.
  std::optional<double> max_porosity_percent;
  /// Whether zones of loosened microstructure are permitted at all.
  bool allow_loosening = true;
};

/// Region of the part with its own limits, for example a sealing face with tighter limits than
/// the rest. Without a box the zone is the whole part.
struct InspectionZone {
  std::string name;
  /// Axis-aligned box in mm in the dataset's coordinates (voxel index times voxel size).
  std::optional<std::array<std::array<double, 3>, 2>> box_mm;
  AcceptanceLimits limits;
};

struct CriterionResult {
  std::string name;  // "max_pore_size_mm", "max_pore_count", "max_porosity_percent", "loosening"
  double measured = 0.0;
  double limit = 0.0;
  bool passed = true;
};

struct ZoneEvaluation {
  std::string name;
  double part_volume_mm3 = 0.0;
  std::int64_t pore_count = 0;     // all pores in the zone
  std::int64_t counted_pores = 0;  // pores at least `count_min_size_mm`
  double largest_pore_size_mm = 0.0;
  int largest_pore_id = 0;
  double void_volume_mm3 = 0.0;
  double porosity_percent = 0.0;
  std::int64_t loosening_zones = 0;
  std::vector<CriterionResult> criteria;
  [[nodiscard]] bool passed() const;
};

struct Evaluation {
  std::vector<ZoneEvaluation> zones;
  [[nodiscard]] bool passed() const;
};

/// Largest extent of a pore in mm: the longest edge of its bounding box.
[[nodiscard]] double poreSizeMm(const DetectedPore& pore, double voxel_size_mm);

/// Assigns pores and loosened zones to the inspection zones by their centre and checks the limits.
/// A pore or zone can belong to several overlapping inspection zones.
[[nodiscard]] Evaluation evaluate(const PorosityResult& result,
                                  const std::vector<InspectionZone>& zones);

/// Reads the inspection zones from the "acceptance" section of an inspection order (see
/// examples/inspection_order.json).
[[nodiscard]] std::vector<InspectionZone> inspectionZonesFromJson(const nlohmann::json& order);

/// Everything a report template can use: the inspection order as given, completed with
/// "nicht angegeben" for missing mandatory fields, the analysis settings and results formatted
/// for print, the evaluation and the projection images as data URIs. `warnings` receives one
/// line per missing mandatory field.
[[nodiscard]] nlohmann::json reportData(const nlohmann::json& order, const PorosityResult& result,
                                        const PorosityOptions& options,
                                        const Evaluation& evaluation,
                                        const std::filesystem::path& image_dir,
                                        std::vector<std::string>* warnings = nullptr);

/// Renders a template with a subset of Mustache: {{name}} (HTML-escaped), {{{name}}} (raw),
/// dotted names, sections {{#name}}...{{/name}} over lists, objects and true values, inverted
/// sections {{^name}}...{{/name}}, {{.}} for the current element and {{! comments }}.
[[nodiscard]] std::string renderTemplate(std::string_view text, const nlohmann::json& data);

/// The built-in report template (German, A4, print to PDF from the browser).
[[nodiscard]] std::string_view defaultReportTemplate();

}  // namespace voxelsieve
