// Built-in operations: porosity, fibres, regions, reports and the DICONDE export.
#include "operations/common.hpp"

namespace voxelsieve::operations {
namespace {

/// The options a porosity step ran with, from its options.json.
PorosityOptions usedPorosityOptions(const std::filesystem::path& porosity_dir) {
  PorosityOptions options;
  if (std::filesystem::exists(porosity_dir / "options.json")) {
    const Json used = Json::parse(readTextFile(porosity_dir / "options.json"));
    options.min_pore_voxels = used.value("min_pore_voxels", options.min_pore_voxels);
    options.zone_sigma = used.value("zone_sigma", options.zone_sigma);
    options.min_zone_void_fraction =
        used.value("min_zone_void_fraction", options.min_zone_void_fraction);
  }
  return options;
}

/// The inspection zones of an order run in a project: regions that name a mesh object of the
/// project ("mesh_object") get its STL and the pose from its coordinates to those of the scanned
/// volume of the step, and relative mesh paths are taken from the order file's directory.
std::vector<InspectionZone> orderZones(const Json& order, const OperationContext& context) {
  Json resolved = order;
  const std::filesystem::path base =
      context.params.contains("order_path")
          ? std::filesystem::path(context.params.at("order_path").get<std::string>()).parent_path()
          : std::filesystem::path();
  const auto resolve = [&](Json& solids) { resolveRegionSolids(solids, context, base); };
  if (resolved.contains("acceptance") && resolved["acceptance"].contains("zones")) {
    for (Json& zone : resolved["acceptance"]["zones"]) {
      if (!zone.is_object() || !zone.contains("region")) {
        continue;
      }
      Json& region = zone["region"];
      if (region.is_array()) {
        resolve(region);
      } else if (region.is_object()) {
        for (const char* part : {"include", "exclude"}) {
          if (region.contains(part)) {
            resolve(region[part]);
          }
        }
      }
    }
  }
  return inspectionZonesFromJson(resolved, base);
}

class Porosity final : public Operation {
 public:
  Porosity() {
    info_.id = "porosity";
    info_.title = "Porosity analysis";
    info_.description =
        "Finds pores and zones of loosened microstructure; writes the result, projection images "
        "and a VDB file with pores and zones.";
    info_.inputs = {
        {"dataset", artifact::kDataset, "Dataset to analyse"},
        {"materials", artifact::kMaterials,
         "Material segmentation: each material is compared with its own grey value", true}};
    info_.outputs = {{"porosity", artifact::kPorosity, "Analysis result"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"min_pore_voxels", {{"type", "integer"}, {"minimum", 1}, {"default", 3}}},
          {"zone_sigma", {{"type", "number"}, {"minimum", 0}, {"default", 5.0}}},
          {"min_zone_void_fraction",
           {{"type", "number"}, {"minimum", 0}, {"maximum", 1}, {"default", 0.01}}},
          {"per_material",
           {{"type", "boolean"},
            {"default", true},
            {"description",
             "With a material segmentation of the dataset: compare each material with its own "
             "grey value, so a darker material (resin between fibre bundles, a light alloy) is "
             "no zone of loosened microstructure"}}},
          {"region", regionParameterSchema()}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationEstimate estimate(const OperationContext& context) const override {
    return perActiveVoxel(context, "dataset", 0.5, 0.2);
  }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    PorosityOptions options;
    options.progress = ProgressToken(context.progress, context.cancel);
    options.min_pore_voxels = p.at("min_pore_voxels").get<std::int64_t>();
    options.zone_sigma = p.at("zone_sigma").get<double>();
    options.min_zone_void_fraction = p.at("min_zone_void_fraction").get<double>();
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    std::optional<MaterialVolume> materials;
    if (const auto it = context.inputs.find("materials");
        it != context.inputs.end() && p.value("per_material", true)) {
      materials = MaterialVolume::open(it->second);
      if (materials->info().dims != dataset.info().dims) {
        throw std::invalid_argument(
            "The material segmentation is of a volume with other dimensions than the dataset");
      }
      options.materials = &*materials;
    }
    const std::optional<InspectionRegion> region = regionParameter(context);
    PorosityResult porosity = analyzePorosity(dataset, options);
    if (region) {
      // Pores are told from the outside air through all of the part; the region then keeps
      // those whose centre lies in it.
      restrictToRegion(porosity, *region, &dataset);
    }
    context.progress(0.8);
    const TelemetryPhase phase("write results");
    const auto dir = context.output_dir / "porosity";
    savePorosityResult(porosity, dir);
    writeJson(dir / "options.json", p);
    writePorosityImages(dataset, porosity, dir);
    writePorosityVdb(dataset, porosity, dir / "porosity.vdb");
    OperationResult result;
    result.outputs["porosity"] = "porosity";
    result.summary = {{"pores", porosity.pores.size()},
                      {"pore_volume_mm3", porosity.poreVolumeMm3()},
                      {"zones", porosity.zones.size()},
                      {"zone_void_volume_mm3", porosity.zoneVoidVolumeMm3()},
                      {"part_volume_mm3", porosity.part_volume_mm3},
                      {"porosity", porosity.porosity()},
                      {"per_material", options.materials != nullptr}};
    if (region) {
      result.summary["region"] = regionName(p, *region);
    }
    return result;
  }

 private:
  OperationInfo info_;
};

class FibreOrientation final : public Operation {
 public:
  FibreOrientation() {
    info_.id = "fibre_orientation";
    info_.title = "Fibre orientation";
    info_.description =
        "Directions of the fibres of a fibre-reinforced part from the structure tensor of the "
        "grey values, one per block, grouped into fibre families (warp and weft of a fabric, the "
        "plies of a laminate) with their share. With a material segmentation only the fibre "
        "material counts (by default the one with the highest grey value, such as the fibre "
        "bundles next to the resin). Writes orientation.json, slices coloured by direction and a "
        "VDB file with the directions.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset to analyse"},
                    {"materials", artifact::kMaterials,
                     "Material segmentation: only the fibre material is analysed", true}};
    info_.outputs = {{"orientation", artifact::kFibreOrientation, "Fibre directions"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"block_size",
           {{"type", "integer"},
            {"minimum", 4},
            {"maximum", 128},
            {"default", 16},
            {"description", "Edge in voxels of the blocks that get one direction each"}}},
          {"families",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 6},
            {"default", 2},
            {"description",
             "Number of fibre families: 2 for a woven fabric, one per ply direction of a "
             "laminate"}}},
          {"material",
           {{"type", "integer"},
            {"minimum", 0},
            {"default", 0},
            {"description",
             "Material id of the fibres in the segmentation; 0: the one with the highest grey "
             "value"}}},
          {"smoothing_sigma",
           {{"type", "number"},
            {"minimum", 0},
            {"maximum", 4},
            {"default", 1.0},
            {"description", "Gaussian smoothing in voxels before the gradients"}}},
          {"min_coherence",
           {{"type", "number"},
            {"minimum", 0},
            {"maximum", 1},
            {"default", 0.1},
            {"description", "Blocks less directional than this are left out of the families"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationEstimate estimate(const OperationContext& context) const override {
    return perActiveVoxel(context, "dataset", 0.1, 0.05);
  }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    OrientationOptions options;
    options.progress = ProgressToken(context.progress, context.cancel);
    options.block_size = p.at("block_size").get<int>();
    options.families = p.at("families").get<int>();
    options.material = p.at("material").get<int>();
    options.smoothing_sigma = p.at("smoothing_sigma").get<double>();
    options.min_coherence = p.at("min_coherence").get<double>();
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    std::optional<MaterialVolume> materials;
    if (const auto it = context.inputs.find("materials"); it != context.inputs.end()) {
      materials = MaterialVolume::open(it->second);
      options.materials = &*materials;
    }
    const OrientationResult orientation = analyzeOrientation(dataset, options);
    context.progress(0.9);
    const TelemetryPhase phase("write results");
    writeOrientationResult(dataset, orientation, context.output_dir / "orientation");
    OperationResult result;
    result.outputs["orientation"] = "orientation";
    Json families = Json::array();
    for (const FibreFamily& family : orientation.families) {
      families.push_back({{"id", family.id},
                          {"direction", family.direction},
                          {"fraction", family.fraction},
                          {"spread_deg", family.spread_deg}});
    }
    result.summary = {{"material", orientation.material},
                      {"blocks", orientation.blocks.size()},
                      {"mean_coherence", orientation.mean_coherence},
                      {"families", families}};
    return result;
  }

 private:
  OperationInfo info_;
};

class InspectionRegions final : public Operation {
 public:
  InspectionRegions() {
    info_.id = "inspection_regions";
    info_.title = "Inspection regions";
    info_.description =
        "Shows the inspection zones of an inspection order next to the part: writes the region "
        "of every zone as a closed STL mesh in the coordinates of the scanned volume (boxes, "
        "spheres and cylinders tessellated, meshes as given) and, with add_objects (the default), "
        "adds each as an object of the project at the pose of the volume, so the slice view "
        "shows where the regions cut the part and the 3D view shows them around it. Excluded "
        "solids become objects of their own. Use it to check the regions before the report.";
    info_.inputs = {{"dataset", artifact::kDataset,
                     "Scanned volume whose coordinates the regions are in", true}};
    info_.outputs = {{"regions", artifact::kInspectionRegions, "One STL per inspection region"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"order",
           {{"type", "object"},
            {"description", "Inspection order with the inspection zones (acceptance.zones)"}}},
          {"order_path",
           {{"type", "string"}, {"format", "path"}, {"description", "Inspection order file"}}},
          {"add_objects",
           {{"type", "boolean"},
            {"default", true},
            {"description", "Add each region as a mesh object of the project"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  void check(const Json& params) const override {
    Operation::check(params);
    // A misspelt limit is found before the step runs, not after the analysis.
    checkInspectionOrder(readOrder(params));
  }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const auto zones = orderZones(readOrder(context.params), context);
    if (zones.empty()) {
      throw std::invalid_argument("The inspection order has no inspection zones");
    }
    const std::filesystem::path dir = context.output_dir / "regions";
    std::filesystem::create_directories(dir);
    // Colours that stand out from the grey part and the red pores.
    static constexpr std::array<std::string_view, 6> kColors{"#2f80ed", "#27ae60", "#9b51e0",
                                                             "#f2994a", "#00b8d9", "#eb5aa6"};
    Json meshes = Json::array();
    for (std::size_t i = 0; i < zones.size(); ++i) {
      const InspectionZone& zone = zones[i];
      for (const bool excluded : {false, true}) {
        const Mesh mesh = zone.region.mesh(excluded);
        if (mesh.triangles.empty()) {
          continue;
        }
        const std::string file =
            "zone_" + std::to_string(i + 1) + (excluded ? "_excluded" : "") + ".stl";
        writeStl(dir / file, mesh);
        meshes.push_back({{"zone", zone.name},
                          {"name", zone.name + (excluded ? " (excluded)" : "")},
                          {"region", zone.region.description()},
                          {"color", excluded ? std::string("#828282")
                                             : std::string(kColors[i % kColors.size()])},
                          {"file", file},
                          {"triangles", mesh.triangles.size()}});
      }
      if (zone.region.wholePart()) {
        context.log("Zone '" + zone.name + "' is the whole part");
      }
    }
    OperationResult result;
    result.outputs["regions"] = "regions";
    result.summary = {{"zones", zones.size()}, {"meshes", meshes}};
    return result;
  }

 private:
  OperationInfo info_;
};

class DefineRegion final : public Operation {
 public:
  DefineRegion() {
    info_.id = "define_region";
    info_.title = "Define region";
    info_.description =
        "Defines a region of a scan (ADR 0041): the union of the included solids (boxes, "
        "spheres, cylinders, meshes, a mesh object of the project such as the CAD model) minus "
        "the excluded ones, in mm in the coordinates of the dataset. Analyses that take a region "
        "(porosity, wall thickness) report only what lies in it. A later step with the same name "
        "replaces the region, one with remove deletes it. Writes region.json and the region as "
        "closed STL meshes for the views.";
    info_.inputs = {{"dataset", artifact::kDataset, "Scan whose coordinates the region is in"}};
    info_.outputs = {{"region", artifact::kRegion, "The region and its meshes"}};
    Json properties = regionParameterSchema().at("properties");
    properties["name"] = {
        {"type", "string"}, {"minLength", 1}, {"description", "Name of the region"}};
    properties["remove"] = {
        {"type", "boolean"}, {"default", false}, {"description", "Delete the region of this name"}};
    info_.parameters = {
        {"type", "object"}, {"required", {"name"}}, {"properties", std::move(properties)}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const std::string name = p.at("name").get<std::string>();
    const std::filesystem::path dir = context.output_dir / "region";
    std::filesystem::create_directories(dir);
    OperationResult result;
    result.outputs["region"] = "region";
    if (p.value("remove", false)) {
      writeJson(dir / "region.json", {{"name", name}, {"remove", true}});
      result.summary = {{"name", name}, {"removed", true}};
      return result;
    }
    Json region = {{"name", name},
                   {"include", p.value("include", Json::array())},
                   {"exclude", p.value("exclude", Json::array())}};
    if (p.contains("color")) {
      region["color"] = p.at("color");
    }
    OperationContext resolved_context = context;
    resolved_context.params = {{"region", region}};
    const std::optional<InspectionRegion> resolved = regionParameter(resolved_context);
    if (!resolved) {
      throw std::invalid_argument("The region '" + name + "' has no solid");
    }
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const DatasetInfo& info = dataset.info();
    writeJson(dir / "region.json", region);
    Json meshes = Json::object();
    for (const bool excluded : {false, true}) {
      const Mesh mesh = resolved->mesh(excluded);
      if (!mesh.triangles.empty()) {
        const std::string file = excluded ? "excluded.stl" : "region.stl";
        writeStl(dir / file, mesh);
        meshes[excluded ? "excluded" : "included"] = file;
      }
    }
    result.summary = {{"name", name},
                      {"description", resolved->description()},
                      {"region", region},
                      {"meshes", meshes}};
    if (const auto bounds = resolved->bounds()) {
      result.summary["bounds_mm"] = *bounds;
      // A region beside the scan is most likely a typo in a coordinate.
      bool apart = false;
      for (std::size_t a = 0; a < 3; ++a) {
        const double extent = info.voxel_size[a] * static_cast<double>(info.dims[a]);
        apart = apart || (*bounds)[1][a] < 0.0 || (*bounds)[0][a] > extent;
      }
      if (apart) {
        context.log("The region '" + name + "' lies outside the scan");
      }
    }
    return result;
  }

 private:
  OperationInfo info_;
};

class Report final : public Operation {
 public:
  Report() {
    info_.id = "report";
    info_.title = "Test report";
    info_.description =
        "Evaluates the porosity result against the acceptance limits of the inspection order "
        "and writes report.html and report.json. Each inspection zone has its limits in the "
        "scheme of BDG P 202 (largest pore, pore count, porosity) or BDG P 203 (porosity_key as "
        "on the drawing, e.g. \"PROI2/Øaq3/da3/UØaq1\", with a reference_volume for PV) and a "
        "freely defined region: boxes, spheres, cylinders and closed meshes (an STL file or a "
        "mesh object of the project, \"mesh_object\"), joined and cut out (see "
        "examples/inspection_order_p203.json); inspection_regions shows them. With a surface, the "
        "report "
        "shows 3D views of the part and its pores; with a comparison, the nominal-actual "
        "comparison with its views and histogram. For fibre composites and other parts of "
        "several materials, a material segmentation adds the materials with their shares and "
        "coloured slices, and a fibre orientation the fibre families, evaluated against the "
        "\"fibres\" requirements of the order (direction tolerance, share). A wall thickness "
        "analysis adds its statistics, views and thin regions, evaluated against the "
        "\"wall_thickness\" limits of the order (min_mm, max_mm). Without a porosity result the "
        "report covers the other analyses alone; the order then holds no limits for pores.";
    info_.inputs = {
        {"porosity", artifact::kPorosity, "Porosity result", true},
        {"surface", artifact::kSurface, "Surface for the 3D views of the part", true},
        {"comparison", artifact::kComparison, "Nominal-actual comparison", true},
        {"dataset", artifact::kDataset, "Dataset for the slices through the materials", true},
        {"materials", artifact::kMaterials, "Material segmentation", true},
        {"orientation", artifact::kFibreOrientation, "Fibre orientation", true},
        {"wall_thickness", artifact::kWallThickness, "Wall thickness", true}};
    info_.outputs = {{"report", artifact::kReport, "Report directory"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"order",
           {{"type", "object"},
            {"description",
             "Inspection order: laboratory, customer, part, scan, acceptance limits (see "
             "examples/inspection_order.json)"}}},
          {"order_path",
           {{"type", "string"}, {"format", "path"}, {"description", "Inspection order file"}}},
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
                                            : std::string(defaultReportTemplate());
    std::optional<TelemetryPhase> phase(std::in_place, "evaluation");
    std::vector<std::string> warnings;
    Json data;
    std::size_t evaluated_zones = 0;
    if (const auto found = context.inputs.find("porosity"); found != context.inputs.end()) {
      const auto& porosity_dir = found->second;
      const PorosityResult porosity = loadPorosityResult(porosity_dir);
      const PorosityOptions options = usedPorosityOptions(porosity_dir);
      const auto zones = orderZones(order, context);
      // The dataset measures the part volume where a region ends at the part's surface.
      std::optional<Dataset> analysed;
      if (const auto it = context.inputs.find("dataset"); it != context.inputs.end()) {
        analysed = Dataset::open(it->second);
      }
      const Evaluation evaluation = evaluate(porosity, zones, analysed ? &*analysed : nullptr);
      evaluated_zones = evaluation.zones.size();
      data = reportData(order, porosity, options, evaluation, porosity_dir, &warnings);
      if (const auto surface = context.inputs.find("surface"); surface != context.inputs.end()) {
        context.log("Rendering the part and its pores");
        phase.reset();
        phase.emplace("images");
        addPartImages(data, SurfaceMask::open(surface->second / "surface.vss"), porosity);
      }
    } else {
      // A report of the deviations, the wall thickness, the fibres or the materials alone.
      const bool any = std::ranges::any_of(
          std::array{"comparison", "materials", "orientation", "wall_thickness"},
          [&](const char* name) { return context.inputs.contains(name); });
      if (!any) {
        throw std::invalid_argument(
            "Nothing to report: the project has no result of a porosity analysis, a "
            "nominal-actual comparison, a wall thickness analysis, a fibre orientation analysis "
            "or a material segmentation");
      }
      data = reportData(order, &warnings);
    }
    if (const auto comparison = context.inputs.find("comparison");
        comparison != context.inputs.end()) {
      addComparison(data, comparison->second);
    }
    if (const auto materials = context.inputs.find("materials");
        materials != context.inputs.end()) {
      std::optional<Dataset> dataset;
      if (const auto it = context.inputs.find("dataset"); it != context.inputs.end()) {
        dataset = Dataset::open(it->second);
      }
      addMaterials(data, materials->second, dataset ? &*dataset : nullptr);
    }
    if (const auto orientation = context.inputs.find("orientation");
        orientation != context.inputs.end()) {
      addFibreOrientation(data, orientation->second, order);
    }
    if (const auto thickness = context.inputs.find("wall_thickness");
        thickness != context.inputs.end()) {
      addWallThickness(data, thickness->second, order);
    }
    phase.reset();
    phase.emplace("write report");
    const auto dir = context.output_dir / "report";
    std::filesystem::create_directories(dir);
    writeFile(dir / "report.html", renderTemplate(report_template, data));
    data.erase("images");
    writeJson(dir / "report.json", data);
    for (const std::string& warning : warnings) {
      context.log(warning);
    }
    OperationResult result;
    result.outputs["report"] = "report";
    result.summary = {{"evaluated_zones", evaluated_zones}, {"missing_fields", warnings.size()}};
    if (data.contains("evaluation")) {
      result.summary["passed"] = data["evaluation"].value("passed", true);
    }
    return result;
  }

 private:
  OperationInfo info_;
};

class FirstArticleReport final : public Operation {
 public:
  FirstArticleReport() {
    info_.id = "first_article_report";
    info_.title = "First article inspection report";
    info_.description =
        "Writes the first article inspection report of a dimensional measurement: cover sheet "
        "(supplier, customer, part, drawing and revision, reason for the submission), the "
        "decision, the list of characteristics with nominal value, tolerance, actual value and "
        "result, the ballooned views and the measured elements; with a comparison or a porosity "
        "result also those, and porosity limits of the order count for the decision. Writes "
        "report.html (print to PDF from the browser), report.json and characteristics.csv.";
    info_.inputs = {{"measurement", artifact::kDimensionalMeasurement, "Dimensional measurement"},
                    {"comparison", artifact::kComparison, "Nominal-actual comparison", true},
                    {"porosity", artifact::kPorosity, "Porosity result", true},
                    {"surface", artifact::kSurface, "Surface for the view of the pores", true}};
    info_.outputs = {{"report", artifact::kReport, "Report directory"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"order",
           {{"type", "object"},
            {"description",
             "report {number, date}, supplier {name, address, code}, customer {name, plant}, "
             "part {name, number, drawing, revision, material, serial}, submission {reason, "
             "quantity}, approval {inspector, approver}, optional scan, acceptance (porosity "
             "limits) and notes (see examples/first_article_order.json)"}}},
          {"order_path", {{"type", "string"}, {"format", "path"}, {"description", "Order file"}}},
          {"template_path",
           {{"type", "string"}, {"format", "path"}, {"description", "Report template file"}}},
          {"include_comparison",
           {{"type", "boolean"},
            {"default", true},
            {"description", "Include the nominal-actual comparison when there is one"}}},
          {"include_porosity",
           {{"type", "boolean"},
            {"default", true},
            {"description", "Include the porosity result when there is one"}}}}}};
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
                                            : std::string(defaultFirstArticleTemplate());
    std::optional<TelemetryPhase> phase(std::in_place, "evaluation");
    Json porosity_data;
    if (const auto porosity = context.inputs.find("porosity");
        p.at("include_porosity").get<bool>() && porosity != context.inputs.end()) {
      const PorosityResult result = loadPorosityResult(porosity->second);
      const Evaluation evaluation = evaluate(result, orderZones(order, context));
      porosity_data = reportData(order, result, usedPorosityOptions(porosity->second), evaluation,
                                 porosity->second);
      if (const auto surface = context.inputs.find("surface"); surface != context.inputs.end()) {
        context.log("Rendering the part and its pores");
        phase.reset();
        phase.emplace("images");
        addPartImages(porosity_data, SurfaceMask::open(surface->second / "surface.vss"), result);
      }
    }
    std::vector<std::string> warnings;
    const auto measurement_dir = context.inputs.at("measurement");
    Json data = firstArticleData(order, measurement_dir, porosity_data, &warnings);
    if (const auto comparison = context.inputs.find("comparison");
        p.at("include_comparison").get<bool>() && comparison != context.inputs.end()) {
      addComparison(data, comparison->second);
    }
    if (const auto materials = context.inputs.find("materials");
        materials != context.inputs.end()) {
      std::optional<Dataset> dataset;
      if (const auto it = context.inputs.find("dataset"); it != context.inputs.end()) {
        dataset = Dataset::open(it->second);
      }
      addMaterials(data, materials->second, dataset ? &*dataset : nullptr);
    }
    if (const auto orientation = context.inputs.find("orientation");
        orientation != context.inputs.end()) {
      addFibreOrientation(data, orientation->second, order);
    }
    phase.reset();
    phase.emplace("write report");
    const auto dir = context.output_dir / "report";
    std::filesystem::create_directories(dir);
    writeFile(dir / "report.html", renderTemplate(report_template, data));
    std::filesystem::copy_file(measurement_dir / "characteristics.csv",
                               dir / "characteristics.csv");
    data.erase("images");
    writeJson(dir / "report.json", data);
    for (const std::string& warning : warnings) {
      context.log(warning);
    }
    OperationResult result;
    result.outputs["report"] = "report";
    result.summary = {{"decision", data.at("decision").at("result")},
                      {"characteristics", data.at("measurement").at("characteristics")},
                      {"failed", data.at("measurement").at("failed")},
                      {"not_measured", data.at("measurement").at("not_measured")},
                      {"missing_fields", warnings.size()}};
    return result;
  }

 private:
  OperationInfo info_;
};

class ExportDiconde final : public Operation {
 public:
  ExportDiconde() {
    info_.id = "export_diconde";
    info_.title = "Export DICONDE";
    info_.description =
        "Writes the scan as DICONDE (ASTM E2339, DICOM for nondestructive evaluation): one CT "
        "image per slice with the exact grey values, voxel size and position in the global "
        "coordinates. With a porosity result, its pores and loosened zones become indications; "
        "with a material segmentation, its labels a second, derived series in the same "
        "coordinates. The inspection order fills in the component (part, material, serial "
        "number), the inspection (laboratory, customer, report and order number, inspector) and "
        "the scan settings, and with acceptance limits the evaluation.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset to export"},
                    {"porosity", artifact::kPorosity, "Pores and zones as indications", true},
                    {"materials", artifact::kMaterials, "Material labels as derived series", true}};
    info_.outputs = {{"diconde", artifact::kDiconde, "DICONDE series (volume/, materials/)"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"order",
           {{"type", "object"},
            {"description",
             "Inspection order: laboratory, customer, part, scan, acceptance limits (see "
             "examples/inspection_order.json)"}}},
          {"order_path",
           {{"type", "string"}, {"format", "path"}, {"description", "Inspection order file"}}},
          {"path",
           {{"type", "string"},
            {"format", "path"},
            {"description",
             "Directory to copy the series to as well, for other programs; must be empty"}}},
          {"series_description",
           {{"type", "string"},
            {"description", "Description of the volume series; default: the object's name"}}},
          {"indications",
           {{"type", "boolean"},
            {"default", true},
            {"description", "Write the pores and zones of the porosity result as indications"}}},
          {"material_labels",
           {{"type", "boolean"},
            {"default", true},
            {"description", "Write the material segmentation as a series of labels"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  void check(const Json& params) const override {
    // "path" is where the export goes: it need not exist yet.
    Json read = params;
    read.erase("path");
    Operation::check(read);
  }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    Json order = Json::object();
    if (p.contains("order_path")) {
      order = Json::parse(readTextFile(p.at("order_path").get<std::string>()));
    }
    if (p.contains("order")) {
      order.merge_patch(p.at("order"));
    }
    std::filesystem::path copy_to;
    if (p.contains("path")) {
      copy_to = std::filesystem::absolute(p.at("path").get<std::string>());
      if (std::filesystem::exists(copy_to) && !std::filesystem::is_empty(copy_to)) {
        throw std::invalid_argument("Directory is not empty: " + copy_to.string());
      }
    }
    DicondeWriteOptions options;
    options.progress = ProgressToken(context.progress, context.cancel);
    std::string object_name;
    if (!context.object.empty()) {
      const Json& object = objectEntry(context, context.object);
      options.pose = entryPose(object);
      object_name = object.at("name").get<std::string>();
    }
    options.series_description = p.value("series_description", object_name);

    std::optional<Evaluation> evaluation;
    const auto porosity = context.inputs.find("porosity");
    if (porosity != context.inputs.end() && p.at("indications").get<bool>()) {
      const PorosityResult result = loadPorosityResult(porosity->second);
      options.indications = porosityIndications(result, options.pose);
      if (const auto zones = orderZones(order, context); !zones.empty()) {
        evaluation = evaluate(result, zones);
      }
    }
    applyInspectionOrder(order, options, evaluation ? &*evaluation : nullptr);

    const auto dir = context.output_dir / "diconde";
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const DicondeSeries volume = writeDicondeDataset(dataset, dir / "volume", options);
    Json series = Json::array({{{"directory", "volume"},
                                {"series_uid", volume.series_uid},
                                {"files", volume.files},
                                {"indications", options.indications.size()}}});
    const auto materials = context.inputs.find("materials");
    if (materials != context.inputs.end() && p.at("material_labels").get<bool>()) {
      context.progress(0.6);
      DicondeWriteOptions labels = options;
      labels.inspection.study_uid = volume.study_uid;
      labels.frame_of_reference_uid = volume.frame_of_reference_uid;
      labels.series_uid.clear();
      labels.series_number = 2;
      labels.series_description = "Materials";
      labels.indications.clear();
      const DicondeSeries written =
          writeDicondeMaterials(MaterialVolume::open(materials->second), dir / "materials", labels);
      series.push_back({{"directory", "materials"},
                        {"series_uid", written.series_uid},
                        {"files", written.files}});
    }
    writeJson(dir / "diconde.json", {{"study_uid", volume.study_uid},
                                     {"frame_of_reference_uid", volume.frame_of_reference_uid},
                                     {"series", series}});
    if (!copy_to.empty()) {
      context.log("Copying to " + copy_to.string());
      std::filesystem::create_directories(copy_to);
      std::filesystem::copy(dir, copy_to, std::filesystem::copy_options::recursive);
    }
    OperationResult result;
    result.outputs["diconde"] = "diconde";
    result.summary = {{"series", series.size()},
                      {"slices", volume.files},
                      {"indications", options.indications.size()},
                      {"study_uid", volume.study_uid}};
    if (evaluation) {
      result.summary["passed"] = evaluation->passed();
    }
    if (!copy_to.empty()) {
      result.summary["path"] = copy_to.string();
    }
    return result;
  }

 private:
  OperationInfo info_;
};

}  // namespace

void registerInspectionOperations(OperationRegistry& registry) {
  registry.add(std::make_shared<Porosity>());
  registry.add(std::make_shared<FibreOrientation>());
  registry.add(std::make_shared<InspectionRegions>());
  registry.add(std::make_shared<DefineRegion>());
  registry.add(std::make_shared<Report>());
  registry.add(std::make_shared<FirstArticleReport>());
  registry.add(std::make_shared<ExportDiconde>());
}

}  // namespace voxelsieve::operations
