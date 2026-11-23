#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/region.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// Pores left out of the evaluation of a zone (BDG P 203: the "U" parameters of the porosity key).
struct DisregardedPores {
  /// Pores whose enclosing or equivalent diameter, or volume, is at most this value.
  std::optional<double> enclosing_diameter_mm;
  std::optional<double> equivalent_diameter_mm;
  std::optional<double> volume_mm3;
  /// Pores at least this round or compact.
  std::optional<double> sphericity;
  std::optional<double> compactness;

  [[nodiscard]] bool any() const;
  [[nodiscard]] bool disregards(const DetectedPore& pore) const;
};

/// Acceptance limits for one inspection zone, following the schemes of BDG P 202 (volume deficits
/// of castings: size of the largest pore, number of pores, porosity) and BDG P 203 (porosity by
/// CT: the parameters of its porosity key). Every limit is optional; the values come from the
/// drawing or the customer, not from VoxelSieve.
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

  // BDG P 203. The porosity of the zone above is its PROI, of the whole part its PG.
  /// PG: porosity of the whole part, whatever the region of the zone.
  std::optional<double> max_global_porosity_percent;
  /// PV: porosity in the reference volume, placed inside the part (and the region of the zone)
  /// where it is highest.
  std::optional<double> max_reference_porosity_percent;
  std::optional<ReferenceVolume> reference_volume;
  /// Vp, Øp, Øaq: volume, enclosing and equivalent diameter of the largest pore.
  std::optional<double> max_pore_volume_mm3;
  std::optional<double> max_enclosing_diameter_mm;
  std::optional<double> max_equivalent_diameter_mm;
  /// Ψ, C: every pore at least this round or compact.
  std::optional<double> min_sphericity;
  std::optional<double> min_compactness;
  /// Gap: distance between neighbouring pores, the material between them.
  std::optional<double> min_pore_gap_mm;
  /// da: distance of every pore to the outer surface of the part, the material between them.
  std::optional<double> min_surface_distance_mm;
  /// U: pores that count for none of the limits.
  DisregardedPores disregard;
};

/// Region of the part with its own limits, for example a sealing face with tighter limits than
/// the rest. The default region is the whole part.
struct InspectionZone {
  std::string name;
  InspectionRegion region;
  AcceptanceLimits limits;
  /// BDG P 203 porosity key the limits came from, as written on the drawing.
  std::string key;
};

/// Sets the limits of a BDG P 203 porosity key such as "BDG P203 - PROI2/ Øaq3/ da3/ UØaq1".
/// Parameters: PG, PROI, PV, Vp, Øp (or Ø), Øaq, Ψ, C, Gap, da, Z, each optionally with the
/// prefix U for pores to disregard; Dp, Daq, D and Psi may stand for Øp, Øaq, Ø and Ψ, and a
/// comma for the decimal point. Throws on unknown parameters.
void applyPorosityKey(std::string_view key, AcceptanceLimits& limits);

struct CriterionResult {
  /// "max_pore_size_mm", "max_pore_count", "max_porosity_percent", "loosening", and for
  /// BDG P 203 the names of the members of AcceptanceLimits. Names starting with "min_" pass
  /// when the measured value is at least the limit, the others when it is at most the limit.
  std::string name;
  double measured = 0.0;
  double limit = 0.0;
  bool passed = true;
  /// Where the value was found: "pore 3", "pores 2 and 5", the placement of a reference volume.
  std::string note;
};

struct ZoneEvaluation {
  std::string name;
  std::string region;  // description of the region
  std::string key;     // BDG P 203 porosity key, if given
  AcceptanceLimits limits;
  double part_volume_mm3 = 0.0;
  std::int64_t pore_count = 0;     // all pores in the zone
  std::int64_t counted_pores = 0;  // pores at least `count_min_size_mm`
  double largest_pore_size_mm = 0.0;
  int largest_pore_id = 0;
  double void_volume_mm3 = 0.0;
  double porosity_percent = 0.0;
  std::int64_t loosening_zones = 0;
  /// Pores in the zone that the limits disregard.
  std::int64_t disregarded_pores = 0;
  std::vector<CriterionResult> criteria;
  [[nodiscard]] bool passed() const;
};

struct Evaluation {
  std::vector<ZoneEvaluation> zones;
  [[nodiscard]] bool passed() const;
};

/// Largest extent of a pore in mm: the longest edge of its bounding box.
[[nodiscard]] double poreSizeMm(const DetectedPore& pore, const VoxelSize& voxel_size);

/// Assigns pores and loosened zones to the inspection zones by their centre and checks the limits.
/// A pore or zone can belong to several overlapping inspection zones. Throws when a limit needs a
/// pore characteristic that the result lacks (results written before ADR 0032). With the analysed
/// `dataset`, the part volume of a region whose boundary runs along the surface of the part is
/// measured on the voxels there (regionPartVolumeMm3).
[[nodiscard]] Evaluation evaluate(const PorosityResult& result,
                                  const std::vector<InspectionZone>& zones,
                                  const Dataset* dataset = nullptr);

/// Throws std::invalid_argument when a section of the order that is evaluated ("acceptance",
/// "wall_thickness", "fibres", "battery") holds a key nothing reads, or a section is named nearly
/// like one of them: a misspelt limit would otherwise not be checked and the part would pass. The
/// other sections describe the inspection for the report and may hold anything. Everything that
/// reads limits from an order calls this.
void checkInspectionOrder(const nlohmann::json& order);

/// Reads the inspection zones from the "acceptance" section of an inspection order (see
/// examples/inspection_order.json and examples/inspection_order_p203.json): name, region
/// (InspectionRegion::fromJson), the limits as fields named like the members of
/// AcceptanceLimits, "porosity_key" (applyPorosityKey), "reference_volume"
/// (ReferenceVolume::fromJson) and "disregard". Relative mesh paths are taken from `base_dir`.
[[nodiscard]] std::vector<InspectionZone> inspectionZonesFromJson(
    const nlohmann::json& order, const std::filesystem::path& base_dir = {});

/// The data of a report without a porosity analysis: the order with its mandatory fields, to
/// which addComparison, addWallThickness, addMaterials and addFibreOrientation add their results.
/// Throws when the order holds limits for pores, which nothing would check.
[[nodiscard]] nlohmann::json reportData(const nlohmann::json& order,
                                        std::vector<std::string>* warnings = nullptr);

/// Everything a report template can use: the inspection order as given, completed with
/// "not given" for missing mandatory fields, the analysis settings and results formatted
/// for print, the evaluation and the projection images as data URIs. `warnings` receives one
/// line per missing mandatory field.
[[nodiscard]] nlohmann::json reportData(const nlohmann::json& order, const PorosityResult& result,
                                        const PorosityOptions& options,
                                        const Evaluation& evaluation,
                                        const std::filesystem::path& image_dir,
                                        std::vector<std::string>* warnings = nullptr);

/// Adds rendered 3D views to report data: the part (images.part) and the part as glass with its
/// pores in red and loosened zones in amber, true to scale (images.pores).
void addPartImages(nlohmann::json& data, const SurfaceMask& surface, const PorosityResult& result);

/// Adds a nominal-actual comparison written by writeComparison in `dir`: its results formatted
/// for print (data.comparison), the rendered views (images.deviation_1, images.deviation_2) and
/// the deviation histogram as SVG, coloured like the views (images.deviation_histogram). `cad_name`
/// names the CAD model in the report; empty takes cad_path from compare.json.
void addComparison(nlohmann::json& data, const std::filesystem::path& dir,
                   const std::string& cad_name = {});

/// Adds a material segmentation (ADR 0013) in `dir`: the classes with their grey values, volume
/// and share of the segmented volume (data.materials) and, with `dataset`, a slice through the
/// middle along each axis with the classes coloured over the grey values (images.materials_x,
/// images.materials_y, images.materials_z; one pixel per voxel of the level shown).
void addMaterials(nlohmann::json& data, const std::filesystem::path& dir,
                  const Dataset* dataset = nullptr);

/// Adds a fibre orientation written by writeOrientationResult in `dir` (ADR 0028): the families
/// with direction, angles to the axes, share and spread, the orientation tensor (data.fibres) and
/// the slices coloured by direction (images.orientation_x, _y, _z). The fibre families required
/// by the "fibres" section of `order` are each compared with the measured family closest to
/// their direction: the angle between them against `tolerance_deg` and the share against
/// `min_share_percent` and `max_share_percent`. These criteria join data.evaluation as a block of
/// their own and count for its verdict.
void addFibreOrientation(nlohmann::json& data, const std::filesystem::path& dir,
                         const nlohmann::json& order);

/// Adds a wall thickness analysis written by writeWallThickness in `dir` (ADR 0030): method,
/// statistics, the regions thinner or thicker than the limits with their location
/// (data.wall_thickness), views coloured by thickness (images.thickness_1, images.thickness_2) and
/// the histogram as SVG (images.thickness_histogram). The "wall_thickness" section of `order`
/// sets the limits (min_mm, max_mm), else those of the analysis count; with it, the thinnest wall
/// against min_mm, or the area thinner than min_mm against max_area_below_min_mm2 when given, and
/// the thickest wall against max_mm join data.evaluation as a block of their own and count for
/// its verdict.
void addWallThickness(nlohmann::json& data, const std::filesystem::path& dir,
                      const nlohmann::json& order);

/// Renders a template with a subset of Mustache: {{name}} (HTML-escaped), {{{name}}} (raw),
/// dotted names, sections {{#name}}...{{/name}} over lists, objects and true values, inverted
/// sections {{^name}}...{{/name}}, {{.}} for the current element and {{! comments }}.
[[nodiscard]] std::string renderTemplate(std::string_view text, const nlohmann::json& data);

/// The built-in report template (A4, print to PDF from the browser).
[[nodiscard]] std::string_view defaultReportTemplate();

/// Everything a first article inspection report template can use (ADR 0027): the order (supplier,
/// customer, part, submission, approval) completed with "not given" for missing mandatory fields,
/// the characteristics of the measurement written by writeMeasurement in `measurement_dir`
/// formatted for print, the measured elements, the alignment, the decision (conforming, not
/// conforming, incomplete) and the balloon views (images.measurement_1, images.measurement_2).
/// `porosity` is null or the reportData of a porosity analysis (with addPartImages): its results,
/// its evaluation against the acceptance limits, which counts for the decision, and its views go
/// into data.porosity and images. `warnings` receives one line per missing mandatory field.
[[nodiscard]] nlohmann::json firstArticleData(const nlohmann::json& order,
                                              const std::filesystem::path& measurement_dir,
                                              const nlohmann::json& porosity = nullptr,
                                              std::vector<std::string>* warnings = nullptr);

/// The built-in first article inspection report template (A4).
[[nodiscard]] std::string_view defaultFirstArticleTemplate();

/// Everything a battery cell inspection report template can use (ADR 0029): the order completed
/// with "not given" for missing mandatory fields, the results of a battery inspection written by
/// writeBatteryResult in `battery_dir` formatted for print (data.battery, data.particles,
/// data.voids), its images as data URIs and the overhang chart as SVG (data.images), and the
/// evaluation against the requirements in order.battery (data.evaluation). `warnings` receives one
/// line per missing mandatory field.
[[nodiscard]] nlohmann::json batteryReportData(const nlohmann::json& order,
                                               const std::filesystem::path& battery_dir,
                                               std::vector<std::string>* warnings = nullptr);

/// The built-in battery cell inspection report template (A4).
[[nodiscard]] std::string_view defaultBatteryReportTemplate();

}  // namespace voxelsieve
