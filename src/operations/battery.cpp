// Built-in operations: battery cells.
#include "operations/common.hpp"

namespace voxelsieve::operations {
namespace {

/// A measured value, or null where nothing could be measured.
Json finiteOrNull(double value) { return std::isfinite(value) ? Json(value) : Json(); }

/// Analysis parameters shared by the battery inspections of datasets and of section images.
Json batteryProperties() {
  return {{"angles",
           {{"type", "integer"},
            {"minimum", 8},
            {"maximum", 3600},
            {"default", 360},
            {"description", "Radial profiles per cross-section of a cylindrical cell"}}},
          {"sections",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 50},
            {"default", 5},
            {"description",
             "Cross-sections along the axis, or profiles per in-plane direction of a stack"}}},
          {"average_voxels",
           {{"type", "integer"},
            {"minimum", 0},
            {"maximum", 10},
            {"default", 2},
            {"description", "Voxels on each side averaged along the layers against noise"}}},
          {"cathode_brighter",
           {{"type", "boolean"},
            {"default", true},
            {"description",
             "The cathode layers are the brighter electrode; false when the anode is, as when "
             "its copper foil is resolved"}}},
          {"particle_threshold",
           {{"type", "number"},
            {"description",
             "Grey value of foreign particles; default as far above the brighter electrode as "
             "that is above the gaps between the layers"}}},
          {"void_threshold",
           {{"type", "number"},
            {"description",
             "Grey value below which a voxel is gas; default half way between air and the "
             "gaps between the layers"}}},
          {"min_particle_voxels", {{"type", "integer"}, {"minimum", 1}, {"default", 2}}},
          {"min_void_voxels", {{"type", "integer"}, {"minimum", 1}, {"default", 8}}},
          {"max_end_deviation_mm",
           {{"type", "number"},
            {"exclusiveMinimum", 0},
            {"default", 1.0},
            {"description",
             "Layer ends farther than this from the median end of their edge (lost layers, tabs) "
             "are left out of the overhang"}}},
          {"gap_factor",
           {{"type", "number"},
            {"minimum", 1.1},
            {"default", 1.6},
            {"description",
             "A space between neighbouring layers more than this times the usual one is a "
             "gap"}}}};
}

BatteryOptions batteryOptions(const Json& p) {
  BatteryOptions options;
  options.angles = p.at("angles").get<int>();
  options.sections = p.at("sections").get<int>();
  if (p.contains("overhang_sections")) {
    options.overhang_sections = p.at("overhang_sections").get<int>();
  }
  options.average_voxels = p.at("average_voxels").get<int>();
  options.cathode_brighter = p.at("cathode_brighter").get<bool>();
  if (p.contains("particle_threshold")) {
    options.particle_threshold = p.at("particle_threshold").get<float>();
  }
  if (p.contains("void_threshold")) {
    options.void_threshold = p.at("void_threshold").get<float>();
  }
  options.min_particle_voxels = p.at("min_particle_voxels").get<std::int64_t>();
  options.min_void_voxels = p.at("min_void_voxels").get<std::int64_t>();
  options.gap_factor = p.at("gap_factor").get<double>();
  options.max_end_deviation_mm = p.at("max_end_deviation_mm").get<double>();
  return options;
}

/// Writes the result into the battery artifact and summarises it.
OperationResult batteryOutcome(const BatteryResult& battery, const OperationContext& context) {
  context.progress(0.9);
  const TelemetryPhase phase("write results");
  writeBatteryResult(battery, context.output_dir / "battery");
  for (const std::string& warning : battery.warnings) {
    context.log(warning);
  }
  OperationResult result;
  result.outputs["battery"] = "battery";
  Json edges = Json::array();
  for (const BatteryEdge& edge : battery.edges) {
    edges.push_back({{"edge", edge.name},
                     {"samples", edge.samples},
                     {"min_overhang_mm", finiteOrNull(edge.min_mm)},
                     {"mean_overhang_mm", finiteOrNull(edge.mean_mm)}});
  }
  result.summary = {
      {"cell_type", battery.type == CellType::kCylindrical ? "cylindrical" : "stacked"},
      {"axis", std::string(1, "xyz"[battery.axis])},
      {"outer_size_mm", battery.outer_size_mm},
      {"cathode_layers", battery.cathode_layers},
      {"anode_layers", battery.anode_layers},
      {"pitch_mm", finiteOrNull(battery.pitch_mm)},
      {"min_overhang_mm", finiteOrNull(battery.min_overhang_mm)},
      {"edges", edges},
      {"gaps", battery.gaps.size()},
      {"particles", battery.particles.size()},
      {"voids", battery.voids.size()}};
  if (battery.type == CellType::kCylindrical) {
    result.summary["diameter_mm"] = finiteOrNull(battery.outer_mean_mm);
    result.summary["out_of_roundness_mm"] = finiteOrNull(battery.max_out_of_roundness_mm);
  } else {
    result.summary["min_thickness_mm"] = finiteOrNull(battery.outer_min_size_mm);
    result.summary["max_thickness_mm"] = finiteOrNull(battery.outer_max_size_mm);
  }
  return result;
}

class BatteryInspection final : public Operation {
 public:
  BatteryInspection() {
    info_.id = "battery_inspection";
    info_.title = "Battery cell inspection";
    info_.description =
        "Inspects a battery cell, cylindrical (wound electrodes in a round can) or stacked "
        "(pouch, prismatic): outer size, diameter and out-of-roundness, dents and wall "
        "thickness of the housing; core of a wound cell; number and pitch of the cathode and "
        "anode layers and gaps between them; the anode overhang beyond the cathode at every "
        "edge of the electrode stack, per layer and section; foreign particles brighter than "
        "the electrodes and gas voids in the electrode stack. The cell axis or stack normal "
        "lies along a volume axis. Writes battery.json and images: a cross-section with the "
        "layers marked, a longitudinal section and the section with the smallest overhang per "
        "edge.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset of the cell"}};
    info_.outputs = {{"battery", artifact::kBattery, "Cell inspection result and images"}};
    Json properties = batteryProperties();
    properties["cell_type"] = {
        {"type", "string"},
        {"enum", {"auto", "cylindrical", "stacked"}},
        {"default", "auto"},
        {"description",
         "cylindrical: wound in a round can; stacked: sheets in a pouch or prismatic can; "
         "auto: cylindrical when the cross-section is round"}};
    properties["axis"] = {{"type", "string"},
                          {"enum", {"auto", "x", "y", "z"}},
                          {"default", "auto"},
                          {"description", "Cell axis or stack normal"}};
    properties["overhang_sections"] = {
        {"type", "integer"},
        {"minimum", 4},
        {"maximum", 720},
        {"default", 36},
        {"description",
         "Sections across the electrode edges: angles around a cylindrical cell; a stack gets a "
         "quarter of them per edge"}};
    info_.parameters = {{"type", "object"}, {"properties", properties}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationEstimate estimate(const OperationContext& context) const override {
    return perActiveVoxel(context, "dataset", 0.3, 0.05);
  }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    BatteryOptions options = batteryOptions(p);
    options.progress = ProgressToken(context.progress, context.cancel);
    if (const std::string type = p.at("cell_type").get<std::string>(); type != "auto") {
      options.cell_type = type == "cylindrical" ? CellType::kCylindrical : CellType::kStacked;
    }
    if (const std::string axis = p.at("axis").get<std::string>(); axis != "auto") {
      options.axis = axis[0] - 'x';
    }
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    return batteryOutcome(analyzeBattery(dataset, options), context);
  }

 private:
  OperationInfo info_;
};

class BatteryImages final : public Operation {
 public:
  BatteryImages() {
    info_.id = "battery_images";
    info_.title = "Battery cell inspection from section images";
    info_.description =
        "Inspects a cylindrical battery cell given as section images instead of a volume: a "
        "folder or ZIP archive with a folder per cell holding axial_images/<angle>.png "
        "(longitudinal sections through the axis, top row at the high end, right half at the "
        "angle, left half opposite) and radial_images/<position mm>.png (cross-sections). "
        "Housing and layers come from the cross-sections, the anode overhang at both ends from "
        "the longitudinal sections, particles and voids from both. The same result as "
        "battery_inspection, for battery_report.";
    info_.outputs = {{"battery", artifact::kBattery, "Cell inspection result and images"}};
    Json properties = batteryProperties();
    properties["path"] = {{"type", "string"},
                          {"format", "path"},
                          {"description", "Folder or ZIP archive with the section images"}};
    properties["cell"] = {
        {"type", "string"},
        {"description", "Name of the cell's folder; the result lists the cells when omitted"}};
    properties["pixel_size_mm"] = {
        {"type", "number"},
        {"exclusiveMinimum", 0},
        {"description",
         "Pixel edge length; default from the positions of the cross-sections over the height "
         "of the longitudinal sections"}};
    info_.parameters = {{"type", "object"}, {"properties", properties}, {"required", {"path"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const std::string path = p.at("path").get<std::string>();
    if (!p.contains("cell")) {
      const std::vector<std::string> cells = listCellImages(path);
      throw std::invalid_argument(
          "Choose a cell with 'cell'; " + std::to_string(cells.size()) + " in " + path + ": " +
          (cells.empty() ? std::string("none")
                         : std::accumulate(std::next(cells.begin()), cells.end(), cells.front(),
                                           [](std::string a, const std::string& b) {
                                             return std::move(a) + ", " + b;
                                           })));
    }
    BatteryOptions options = batteryOptions(p);
    options.progress = ProgressToken(context.progress, context.cancel);
    if (p.contains("pixel_size_mm")) {
      options.image_pixel_mm = p.at("pixel_size_mm").get<double>();
    }
    const CellImages images = readCellImages(path, p.at("cell").get<std::string>());
    context.progress(0.2);
    return batteryOutcome(analyzeBatteryImages(images, options), context);
  }

 private:
  OperationInfo info_;
};

class BatteryReport final : public Operation {
 public:
  BatteryReport() {
    info_.id = "battery_report";
    info_.title = "Battery cell inspection report";
    info_.description =
        "Writes the inspection report of a battery cell inspection: order, test item, method, "
        "acquisition, the cell and its housing per section, the anode overhang per edge with a "
        "chart and the sections of the smallest overhang, foreign particles and gas voids, and "
        "the assessment against the requirements in order.battery (overhang, layer count, "
        "dimensions, roundness, wall, gaps, particles, voids). Writes report.html (print to PDF "
        "from the browser) and report.json.";
    info_.inputs = {{"battery", artifact::kBattery, "Battery cell inspection"}};
    info_.outputs = {{"report", artifact::kReport, "Report directory"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"order",
           {{"type", "object"},
            {"description",
             "report {number, date}, laboratory, customer, order, part {name, drawing, material "
             "(cell chemistry), serial, state_of_charge}, scan, approval and battery, the "
             "requirements: guideline, min_overhang_mm, max_overhang_mm, max_edge_spread_mm, "
             "cathode_layers {min, max}, diameter_mm / length_mm {nominal, tolerance} or {min, "
             "max}, thickness_mm {min, max}, max_swelling_mm, max_out_of_roundness_mm, "
             "max_dent_mm, min_wall_mm, max_core_out_of_roundness_mm, max_gaps, max_particles, "
             "max_particle_size_mm, max_voids, max_void_size_mm (see "
             "examples/battery_order.json)"}}},
          {"order_path", {{"type", "string"}, {"format", "path"}, {"description", "Order file"}}},
          {"template_path",
           {{"type", "string"}, {"format", "path"}, {"description", "Report template file"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  void check(const Json& params) const override {
    Operation::check(params);
    // A misspelt limit is found before the step runs, not after the analysis.
    checkInspectionOrder(readOrder(params));
  }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const Json order = readOrder(p);
    const std::string report_template = p.contains("template_path")
                                            ? readTextFile(p.at("template_path").get<std::string>())
                                            : std::string(defaultBatteryReportTemplate());
    std::vector<std::string> warnings;
    Json data;
    {
      const TelemetryPhase phase("evaluation");
      data = batteryReportData(order, context.inputs.at("battery"), &warnings);
    }
    const TelemetryPhase phase("write report");
    const auto dir = context.output_dir / "report";
    std::filesystem::create_directories(dir);
    writeFile(dir / "report.html", renderTemplate(report_template, data));
    data.erase("images");
    for (Json& edge : data.at("battery").at("edges")) {
      edge.erase("image");
    }
    writeJson(dir / "report.json", data);
    for (const std::string& warning : warnings) {
      context.log(warning);
    }
    OperationResult result;
    result.outputs["report"] = "report";
    result.summary = {{"missing_fields", warnings.size()}};
    if (data.contains("evaluation")) {
      result.summary["passed"] = data.at("evaluation").at("passed");
      std::int64_t failed = 0;
      for (const Json& c : data.at("evaluation").at("criteria")) {
        failed += c.at("passed").get<bool>() ? 0 : 1;
      }
      result.summary["criteria"] = data.at("evaluation").at("criteria").size();
      result.summary["failed"] = failed;
    }
    return result;
  }

 private:
  OperationInfo info_;
};

}  // namespace

void registerBatteryOperations(OperationRegistry& registry) {
  registry.add(std::make_shared<BatteryInspection>());
  registry.add(std::make_shared<BatteryImages>());
  registry.add(std::make_shared<BatteryReport>());
}

}  // namespace voxelsieve::operations
