// Built-in operations: the surface, comparisons with it, wall thickness and dimensions.
#include "operations/common.hpp"

namespace voxelsieve::operations {
namespace {

/// Parameters of the optional non-rigid registration of the comparisons (ADR 0022).
void addDeformationParameters(Json& parameters) {
  Json& properties = parameters["properties"];
  properties["non_rigid"] = {
      {"type", "boolean"},
      {"default", false},
      {"description",
       "Non-rigid registration for flexible parts: after the rigid alignment, bend the scanned "
       "surface onto the nominal one with a smooth, elastic deformation and measure the "
       "deviation on the bent surface. The displacement of every point is reported beside it."}};
  properties["non_rigid_spacing_mm"] = {
      {"type", "number"},
      {"minimum", 0},
      {"default", 0},
      {"description",
       "Spacing of the control points of the deformation in mm; features smaller than this stay "
       "deviations. 0: an eighth of the largest extent of the part"}};
  properties["non_rigid_stiffness"] = {
      {"type", "number"},
      {"minimum", 0.01},
      {"maximum", 10000},
      {"default", 1},
      {"description",
       "Resistance against stretching: high values let only thin sections bend and keep the "
       "size of the part, low values follow almost any smooth deviation"}};
}

void readDeformationParameters(const Json& p, CompareOptions& options) {
  options.deformation.enabled = p.at("non_rigid").get<bool>();
  options.deformation.spacing_mm = p.at("non_rigid_spacing_mm").get<double>();
  options.deformation.stiffness = p.at("non_rigid_stiffness").get<double>();
}

/// Summary figures of the non-rigid registration, when there was one.
void addDeformationSummary(const CompareResult& compared, Json& summary) {
  const DeformationResult& d = compared.deformation;
  if (!d.applied) {
    return;
  }
  summary["displacement_max_mm"] = d.displacement.max_mm;
  summary["displacement_mean_mm"] = d.displacement.mean_mm;
  summary["rigid_deviation_rms_mm"] = d.rigid.rms_mm;
  summary["rigid_within_tolerance_percent"] = 100.0 * d.rigid.within_tolerance;
}

/// The CAD model a comparison was made with, copied beside its compare.json.
constexpr const char* kNominalCopy = "nominal.stl";

/// The nominal geometry of a comparison step and where it lay on the scan: the CAD model of
/// compare_cad or the nominal object of compare_objects.
struct ComparedNominal {
  Mesh mesh;
  RigidTransform to_scan;
  std::string source;
  double tolerance_mm = 0.0;
  bool bent = false;  // the comparison also registered it non-rigidly
};

/// The CAD file of a comparison: the copy beside its compare.json, or for comparisons written
/// before there was one (and by vs-compare) the file where it was.
std::filesystem::path comparedCadFile(const std::filesystem::path& comparison_dir,
                                      const Json& compared) {
  if (compared.contains("cad_file")) {
    const auto copy = comparison_dir / compared.at("cad_file").get<std::string>();
    if (std::filesystem::exists(copy)) {
      return copy;
    }
  }
  return compared.at("cad_path").get<std::string>();
}

ComparedNominal comparedNominal(const OperationContext& context,
                                const std::filesystem::path& comparison_dir) {
  std::ifstream in(comparison_dir / "compare.json");
  if (!in) {
    throw std::runtime_error("Cannot read " + (comparison_dir / "compare.json").string());
  }
  const Json json = Json::parse(in);
  ComparedNominal nominal;
  nominal.to_scan = RigidTransform::fromMatrix(json.at("cad_to_scan").get<std::vector<double>>());
  nominal.tolerance_mm = json.at("tolerance_mm").get<double>();
  nominal.bent = json.contains("deformation");
  if (json.contains("cad_path")) {
    const std::filesystem::path path = json.at("cad_path").get<std::string>();
    nominal.mesh = readStl(comparedCadFile(comparison_dir, json));
    nominal.source = path.filename().string();
  } else if (json.contains("nominal_object")) {
    const std::string id = json.at("nominal_object").get<std::string>();
    const Json& object = objectEntry(context, id);
    double resolution_mm = 0.0;
    nominal.mesh = triangleSoup(objectSurface(object, 2000000, true, resolution_mm));
    nominal.source = object.at("name").get<std::string>();
  } else {
    throw std::invalid_argument("The comparison names no nominal geometry");
  }
  return nominal;
}

/// The nominal geometry of a comparison in the coordinates its compare.json places in the scan
/// with cad_to_scan: the CAD file it was made with or the nominal object.
Mesh comparisonNominal(const OperationContext& context, const Json& compared,
                       const std::filesystem::path& comparison_dir,
                       std::filesystem::path& cad_path) {
  if (!cad_path.empty()) {
    return readStl(cad_path);
  }
  if (compared.contains("cad_path")) {
    cad_path = compared.at("cad_path").get<std::string>();
    return readStl(comparedCadFile(comparison_dir, compared));
  }
  if (compared.contains("nominal_object")) {
    const std::string id = compared.at("nominal_object").get<std::string>();
    const Json& nominal = objectEntry(context, id);
    if (nominal.at("kind") != kMeshObject) {
      throw std::invalid_argument(
          "The comparison's nominal object is a volume: dimensional measurement needs a CAD model "
          "(give cad_path)");
    }
    cad_path = nominal.at("outputs").at(artifact::kMesh).get<std::string>();
    return readStl(cad_path);
  }
  throw std::invalid_argument("The comparison names no CAD model: give cad_path");
}

class Surface final : public Operation {
 public:
  Surface() {
    info_.id = "surface";
    info_.title = "Surface";
    info_.description =
        "Locates the surface of the part and stores it as a distance mask with a few bits per "
        "voxel (surface.vss): only blocks near the surface are stored, and there the codes give "
        "the distance to the surface in steps of a fraction of a voxel. Optionally also as STL "
        "mesh and VDB level set.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset"}};
    info_.outputs = {{"surface", artifact::kSurface, "Surface mask, images, mesh"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"bits", {{"type", "integer"}, {"minimum", 2}, {"maximum", 8}, {"default", 4}}},
          {"band_voxels",
           {{"type", "number"}, {"minimum", 0.25}, {"maximum", 3}, {"default", 1.0}}},
          {"iso_value",
           {{"type", "number"},
            {"description",
             "Grey value of the surface; default half way between air and "
             "material"}}},
          {"stl", {{"type", "boolean"}, {"default", false}}},
          {"vdb", {{"type", "boolean"}, {"default", false}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationEstimate estimate(const OperationContext& context) const override {
    return perActiveVoxel(context, "dataset", 0.1, 0.05);
  }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    SurfaceOptions options;
    options.progress = ProgressToken(context.progress, context.cancel);
    options.bits = p.at("bits").get<int>();
    options.band_voxels = p.at("band_voxels").get<double>();
    if (p.contains("iso_value")) {
      options.iso_value = p.at("iso_value").get<float>();
    }
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const auto dir = context.output_dir / "surface";
    std::filesystem::create_directories(dir);
    std::optional<TelemetryPhase> phase(std::in_place, "surface mask");
    const SurfaceInfo info = writeSurface(dataset, dir / "surface.vss", options);
    context.progress(0.7);
    phase.reset();
    phase.emplace("images and meshes");
    const SurfaceMask mask = SurfaceMask::open(dir / "surface.vss");
    writeJson(dir / "surface.json", toJson(info));
    writeSurfaceImages(mask, dir);
    if (p.at("stl").get<bool>()) {
      mask.writeStl(dir / "surface.stl");  // streamed chunk by chunk
    }
    if (p.at("vdb").get<bool>()) {
      openvdb::io::File file((dir / "surface.vdb").string());
      file.write({mask.toLevelSet()});
      file.close();
    }
    const auto voxels = static_cast<double>(info.dims[0] * info.dims[1] * info.dims[2]);
    OperationResult result;
    result.outputs["surface"] = "surface";
    result.summary = {{"surface_blocks", info.surface_blocks},
                      {"band_voxels", info.band_voxel_count},
                      {"file_bytes", info.file_bytes},
                      {"bits_per_voxel", 8.0 * static_cast<double>(info.file_bytes) / voxels},
                      {"compression_vs_raw", 2.0 * voxels / static_cast<double>(info.file_bytes)},
                      {"step_voxels", info.stepVoxels()},
                      {"iso_value", info.iso_value},
                      {"surface_volume_mm3", info.volume_mm3}};
    return result;
  }

 private:
  OperationInfo info_;
};

class CompareCad final : public Operation {
 public:
  CompareCad() {
    info_.id = "compare_cad";
    info_.title = "Nominal-actual comparison";
    info_.description =
        "Aligns the nominal geometry (CAD model as STL, mm) to the extracted surface and measures "
        "the signed deviation of every surface point from it: positive where the part has more "
        "material than nominal. Writes compare.json, deviation.ply (coloured mesh), views "
        "along the axes and optionally the CAD model in scan coordinates (cad_aligned.stl).";
    info_.inputs = {{"surface", artifact::kSurface, "Surface of the scan", false, "surface"}};
    info_.outputs = {{"comparison", artifact::kComparison, "Deviations from the CAD model"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"cad_path",
           {{"type", "string"}, {"format", "path"}, {"description", "CAD model as STL in mm"}}},
          {"alignment",
           {{"type", "string"},
            {"enum", {"auto", "refine", "none"}},
            {"default", "auto"},
            {"description",
             "auto: coarse by the principal axes, then fine (best fit); refine: fine only, the "
             "CAD model already lies about in scan coordinates; none: as it is"}}},
          {"tolerance_mm",
           {{"type", "number"}, {"minimum", 0.001}, {"maximum", 100}, {"default", 0.1}}},
          {"outer_surface_only",
           {{"type", "boolean"},
            {"default", true},
            {"description", "Leave the surfaces of closed internal voids (pores) out"}}},
          {"aligned_stl", {{"type", "boolean"}, {"default", false}}}}},
        {"required", {"cad_path"}}};
    addDeformationParameters(info_.parameters);
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const std::filesystem::path cad_path =
        std::filesystem::absolute(p.at("cad_path").get<std::string>());
    CompareOptions options;
    options.progress = ProgressToken(context.progress, context.cancel);
    options.alignment = alignmentFromString(p.at("alignment").get<std::string>());
    options.tolerance_mm = p.at("tolerance_mm").get<double>();
    options.outer_surface_only = p.at("outer_surface_only").get<bool>();
    readDeformationParameters(p, options);
    const Mesh cad = readStl(cad_path);
    context.progress(0.1);
    const SurfaceMask mask = SurfaceMask::open(context.inputs.at("surface") / "surface.vss");
    const CompareResult compared = compareToCad(mask, cad, options);
    context.progress(0.8);
    const TelemetryPhase phase("write results");
    const auto dir = context.output_dir / "comparison";
    writeComparison(compared, dir);
    if (p.at("aligned_stl").get<bool>()) {
      writeAlignedCad(compared, cad, dir / "cad_aligned.stl");
    }
    Json json = toJson(compared);
    json["cad_path"] = cad_path.string();
    // The project keeps the model it was compared with: later steps (nominal wall thickness,
    // dimensional measurement) read this copy, also when the file has moved or changed since.
    std::filesystem::copy_file(cad_path, dir / kNominalCopy,
                               std::filesystem::copy_options::overwrite_existing);
    json["cad_file"] = kNominalCopy;
    writeJson(dir / "compare.json", json);
    const DeviationStats& s = compared.stats;
    OperationResult result;
    result.outputs["comparison"] = "comparison";
    result.summary = {{"deviation_mean_mm", s.mean_mm},
                      {"deviation_rms_mm", s.rms_mm},
                      {"deviation_min_mm", s.min_mm},
                      {"deviation_max_mm", s.max_mm},
                      {"within_tolerance_percent", 100.0 * s.within_tolerance},
                      {"above_tolerance_percent", 100.0 * s.above_tolerance},
                      {"below_tolerance_percent", 100.0 * s.below_tolerance},
                      {"tolerance_mm", compared.tolerance_mm},
                      {"fit_rms_mm", compared.fit_rms_mm},
                      {"rotation_deg", compared.cad_to_scan.angleDegrees()},
                      {"dropped_components", compared.dropped_components}};
    addDeformationSummary(compared, result.summary);
    return result;
  }

 private:
  OperationInfo info_;
};

class WallThickness final : public Operation {
 public:
  WallThickness() {
    info_.id = "wall_thickness";
    info_.title = "Wall thickness";
    info_.description =
        "Measures the wall thickness at every point of the part surface (ADR 0030). Ray method: "
        "along the inward normal through the grey values to the opposite face of the wall, "
        "sub-voxel accurate for walls with roughly parallel faces. Sphere method: the diameter "
        "of the largest sphere inside the material that touches the point, also in corners and "
        "at ribs. Pores thin the wall. Surface regions thinner than min_wall_mm or thicker than "
        "max_wall_mm are listed. Writes wall_thickness.json, thickness.ply (coloured mesh) and "
        "views; a missing surface is extracted first, as a step of its own. With volume_map, also "
        "the local thickness of every material voxel (largest sphere containing it): the "
        "dataset thickness_map, thickness_map.json and slices through the thickest spot; it "
        "shows walls inside the part and material accumulations, and colours the slice view. "
        "With a nominal-actual comparison, also the nominal thickness of the CAD model at "
        "every point, by the same method, and the deviation of the actual thickness from it.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset"},
                    {"surface", artifact::kSurface, "Surface of the dataset", false, "surface"},
                    {"comparison", artifact::kComparison,
                     "Nominal-actual comparison: its CAD model where it lies on the scan, for the "
                     "nominal thickness",
                     true}};
    info_.outputs = {{"wall_thickness", artifact::kWallThickness, "Wall thickness"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"method",
           {{"type", "string"},
            {"enum", {"ray", "sphere"}},
            {"default", "ray"},
            {"description",
             "ray: to the opposite face along the normal (plates, housings); sphere: largest "
             "inscribed sphere (castings with ribs and fillets)"}}},
          {"min_wall_mm",
           {{"type", "number"},
            {"minimum", 0},
            {"default", 0},
            {"description", "Smallest permitted wall thickness; 0: none"}}},
          {"max_wall_mm",
           {{"type", "number"},
            {"minimum", 0},
            {"default", 0},
            {"description",
             "Largest permitted wall thickness, such as material accumulations of castings; 0: "
             "none"}}},
          {"max_thickness_mm",
           {{"type", "number"},
            {"minimum", 0},
            {"default", 0},
            {"description",
             "Thicker walls stay unmeasured; 0: half the middle extent of the part"}}},
          {"search_angle_deg",
           {{"type", "number"},
            {"minimum", 1},
            {"maximum", 90},
            {"default", 30},
            {"description",
             "Ray method: largest angle between the ray and the normal of the face it leaves the "
             "wall through"}}},
          {"outer_surface_only",
           {{"type", "boolean"},
            {"default", true},
            {"description",
             "Values only on the outer skin; internal voids still limit the walls"}}},
          {"max_triangles",
           {{"type", "integer"},
            {"minimum", 10000},
            {"maximum", 50000000},
            {"default", 2000000},
            {"description", "Triangle budget of the measured surface mesh"}}},
          {"volume_map",
           {{"type", "boolean"},
            {"default", false},
            {"description",
             "Also map the local thickness of every material voxel (slower: a few seconds per "
             "100 MB of scan for walls of 30 voxels)"}}},
          {"map_max_thickness_mm",
           {{"type", "number"},
            {"minimum", 0},
            {"default", 0},
            {"description",
             "Thickness map: thicker material gets this value; 0: the smallest extent of the "
             "part, at most 64 voxels. Time and memory grow with it"}}},
          {"nominal_thickness",
           {{"type", "boolean"},
            {"default", true},
            {"description",
             "With a comparison input: compare with the nominal thickness of its CAD model"}}},
          {"nominal_tolerance_mm",
           {{"type", "number"},
            {"minimum", 0},
            {"default", 0},
            {"description",
             "Permitted deviation from the nominal thickness; 0: twice the tolerance of the "
             "comparison (each face may deviate by it)"}}},
          {"region", regionParameterSchema()}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationEstimate estimate(const OperationContext& context) const override {
    // The volume map is a float per material voxel, with its coarser levels.
    const bool map = context.params.value("volume_map", false);
    return perActiveVoxel(context, "dataset", 0.2, map ? 5.0 : 0.05);
  }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    WallThicknessOptions options;
    options.method = thicknessMethodFromString(p.at("method").get<std::string>());
    options.min_wall_mm = p.at("min_wall_mm").get<double>();
    options.max_wall_mm = p.at("max_wall_mm").get<double>();
    options.max_thickness_mm = p.at("max_thickness_mm").get<double>();
    options.search_angle_deg = p.at("search_angle_deg").get<double>();
    options.outer_surface_only = p.at("outer_surface_only").get<bool>();
    options.max_triangles = p.at("max_triangles").get<std::size_t>();
    const std::optional<InspectionRegion> region = regionParameter(context);
    options.region = region ? &*region : nullptr;
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const auto dir = context.output_dir / "wall_thickness";
    std::filesystem::create_directories(dir);
    const std::filesystem::path surface_file = context.inputs.at("surface") / "surface.vss";
    // The thickness map takes longer than the surface.
    const bool volume_map = p.at("volume_map").get<bool>();
    const double start = 0.0;
    const double surface_end = volume_map ? 0.4 : 0.85;
    context.progress(start);
    options.progress = [&](double fraction) {
      context.progress(start + (surface_end - start) * fraction);
    };
    WallThicknessResult thickness =
        analyzeWallThickness(dataset, SurfaceMask::open(surface_file), options);
    const auto comparison = context.inputs.find("comparison");
    if (comparison != context.inputs.end() && p.at("nominal_thickness").get<bool>()) {
      const ComparedNominal nominal = comparedNominal(context, comparison->second);
      if (nominal.bent) {
        context.log(
            "The comparison also bent the CAD model; the nominal thickness is measured "
            "where its rigid alignment placed it");
      }
      NominalThicknessOptions nominal_options;
      nominal_options.source = nominal.source;
      const double tolerance = p.at("nominal_tolerance_mm").get<double>();
      nominal_options.tolerance_mm = tolerance > 0.0 ? tolerance : 2.0 * nominal.tolerance_mm;
      addNominalThickness(thickness, nominal.mesh, nominal.to_scan, nominal_options);
    }
    {
      const TelemetryPhase phase("write results");
      writeWallThickness(thickness, dir);
    }
    const ThicknessStats& s = thickness.stats;
    OperationResult result;
    result.outputs["wall_thickness"] = "wall_thickness";
    result.summary = {
        {"method", toString(thickness.method)},
        {"thickness_min_mm", s.min_mm},
        {"thickness_median_mm", s.percentiles_mm[2]},
        {"thickness_mean_mm", s.mean_mm},
        {"thickness_max_mm", s.max_mm},
        {"measured_percent", s.area_mm2 > 0.0 ? 100.0 * s.measured_area_mm2 / s.area_mm2 : 0.0},
        {"max_thickness_mm", thickness.max_thickness_mm}};
    if (region) {
      result.summary["region"] = regionName(p, *region);
    }
    if (options.min_wall_mm > 0.0) {
      result.summary["min_wall_mm"] = options.min_wall_mm;
      result.summary["below_min_percent"] = 100.0 * s.below_min;
      result.summary["thin_regions"] = thickness.thin_regions.size();
    }
    if (options.max_wall_mm > 0.0) {
      result.summary["max_wall_mm"] = options.max_wall_mm;
      result.summary["above_max_percent"] = 100.0 * s.above_max;
      result.summary["thick_regions"] = thickness.thick_regions.size();
    }
    if (thickness.nominal) {
      const DeviationStats& d = thickness.nominal->stats;
      result.summary["nominal"] = thickness.nominal->source;
      result.summary["nominal_tolerance_mm"] = thickness.nominal->tolerance_mm;
      result.summary["nominal_compared_percent"] =
          s.measured_area_mm2 > 0.0 ? 100.0 * d.area_mm2 / s.measured_area_mm2 : 0.0;
      result.summary["nominal_deviation_min_mm"] = d.min_mm;
      result.summary["nominal_deviation_max_mm"] = d.max_mm;
      result.summary["thinner_than_nominal_percent"] = 100.0 * d.below_tolerance;
      result.summary["thicker_than_nominal_percent"] = 100.0 * d.above_tolerance;
    }
    if (volume_map) {
      ThicknessMapOptions map_options;
      map_options.max_thickness_mm = p.at("map_max_thickness_mm").get<double>();
      map_options.min_wall_mm = options.min_wall_mm;
      map_options.max_wall_mm = options.max_wall_mm;
      map_options.progress = [&](double fraction) {
        context.progress(surface_end + (1.0 - surface_end) * fraction);
      };
      const ThicknessMapResult map =
          writeThicknessMap(dataset, SurfaceMask::open(surface_file), dir, map_options);
      result.summary["map_median_mm"] = map.percentiles_mm[2];
      result.summary["map_max_mm"] = map.max_mm;
      result.summary["map_range_mm"] = map.range_mm;
      if (options.min_wall_mm > 0.0) {
        result.summary["map_below_min_percent"] = 100.0 * map.below_min;
      }
      if (options.max_wall_mm > 0.0) {
        result.summary["map_above_max_percent"] = 100.0 * map.above_max;
        result.summary["map_thick_regions"] = map.thick_regions.size();
      }
    }
    return result;
  }

 private:
  OperationInfo info_;
};

class CompareObjects final : public Operation {
 public:
  CompareObjects() {
    info_.id = "compare_objects";
    info_.title = "Compare with nominal object";
    info_.description =
        "Nominal-actual comparison of two placed objects: the signed deviation of every point of "
        "the scanned surface from the nominal object (a mesh such as a CAD model, or another "
        "volume's surface), where the objects lie now. Positive where the part has more material "
        "than nominal. Place the objects first with move or the align operations; refine=true "
        "fits once more for the measurement only, without moving them.";
    info_.inputs = {
        {"surface", artifact::kSurface, "Surface of the scanned object", false, "surface"}};
    info_.outputs = {{"comparison", artifact::kComparison, "Deviations from the nominal object"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"nominal", {{"type", "string"}, {"description", "Id of the nominal object"}}},
          {"tolerance_mm",
           {{"type", "number"}, {"minimum", 0.001}, {"maximum", 100}, {"default", 0.1}}},
          {"outer_surface_only",
           {{"type", "boolean"},
            {"default", true},
            {"description", "Leave the surfaces of closed internal voids (pores) out"}}},
          {"refine", {{"type", "boolean"}, {"default", false}}}}},
        {"required", {"nominal"}}};
    addDeformationParameters(info_.parameters);
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    if (context.object.empty()) {
      throw std::invalid_argument("The surface belongs to no object");
    }
    const std::string nominal_id = p.at("nominal").get<std::string>();
    if (nominal_id == context.object) {
      throw std::invalid_argument("An object cannot be compared with itself");
    }
    const Json& actual = objectEntry(context, context.object);
    const Json& nominal_object = objectEntry(context, nominal_id);
    CompareOptions options;
    options.progress = ProgressToken(context.progress, context.cancel);
    options.alignment = p.at("refine").get<bool>() ? CompareOptions::Alignment::kRefine
                                                   : CompareOptions::Alignment::kNone;
    options.tolerance_mm = p.at("tolerance_mm").get<double>();
    options.outer_surface_only = p.at("outer_surface_only").get<bool>();
    readDeformationParameters(p, options);
    // Nominal object coordinates to those of the scanned object.
    options.initial = entryPose(actual).inverse().after(entryPose(nominal_object));
    double resolution_mm = 0.0;
    const Mesh nominal =
        triangleSoup(objectSurface(nominal_object, options.max_triangles, true, resolution_mm));
    context.progress(0.1);
    const SurfaceMask mask = SurfaceMask::open(context.inputs.at("surface") / "surface.vss");
    const CompareResult compared = compareToCad(mask, nominal, options);
    context.progress(0.8);
    const TelemetryPhase phase("write results");
    const auto dir = context.output_dir / "comparison";
    writeComparison(compared, dir);
    Json json = toJson(compared);
    json["nominal_object"] = nominal_id;
    json["nominal_name"] = nominal_object.at("name");
    writeJson(dir / "compare.json", json);
    const DeviationStats& s = compared.stats;
    OperationResult result;
    result.outputs["comparison"] = "comparison";
    result.summary = {{"nominal", nominal_object.at("name")},
                      {"deviation_mean_mm", s.mean_mm},
                      {"deviation_rms_mm", s.rms_mm},
                      {"deviation_min_mm", s.min_mm},
                      {"deviation_max_mm", s.max_mm},
                      {"within_tolerance_percent", 100.0 * s.within_tolerance},
                      {"above_tolerance_percent", 100.0 * s.above_tolerance},
                      {"below_tolerance_percent", 100.0 * s.below_tolerance},
                      {"tolerance_mm", compared.tolerance_mm},
                      {"fit_rms_mm", compared.fit_rms_mm},
                      {"dropped_components", compared.dropped_components}};
    addDeformationSummary(compared, result.summary);
    return result;
  }

 private:
  OperationInfo info_;
};

class MeasureDimensions final : public Operation {
 public:
  MeasureDimensions() {
    info_.id = "measure_dimensions";
    info_.title = "Dimensional measurement";
    info_.description =
        "Measures the characteristics of a drawing on the scanned surface: fits planes, "
        "cylinders, spheres and points where the CAD model has them, aligns the part on its "
        "datums (3-2-1, RPS) or keeps the best fit of the comparison, and evaluates sizes, "
        "distances, angles, form, orientation, position and profile against their tolerances "
        "(see examples/first_article_plan.json). Elements are found on the CAD model by a point "
        "on them ('at'); cad_features lists the ones it has. Writes measurement.json, "
        "characteristics.csv and views with the characteristic numbers as balloons.";
    info_.inputs = {{"surface", artifact::kSurface, "Surface of the scan", false, "surface"},
                    {"comparison", artifact::kComparison,
                     "Nominal-actual comparison that placed the CAD model"}};
    info_.outputs = {
        {"measurement", artifact::kDimensionalMeasurement, "Measured characteristics"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"plan",
           {{"type", "object"},
            {"description",
             "Inspection plan in CAD (drawing) coordinates, mm. elements: [{id, type: plane | "
             "cylinder | sphere | point, at: [x,y,z] a point on the feature of the CAD model "
             "(from cad_features), optional search_mm, edge_margin_mm}]; instead of at, explicit "
             "geometry (plane: point, normal, radius_mm; cylinder: point, direction, diameter, "
             "length; "
             "sphere: center, diameter; point: point, normal). alignment: \"best_fit\" or "
             "{datums: [\"A\", \"B\", \"C\"]} in order of precedence (3-2-1), or with "
             "{element, directions: [\"x\"]} per datum for RPS points. characteristics: "
             "[{number, label, type, elements: [ids] or element, datum, nominal, upper, lower or "
             "tolerance (± or zone width), drawing_zone, note}]; types diameter, radius, "
             "distance (optional along: x|y|z), angle, coordinate (axis: x|y|z), position "
             "(datum-based zone), flatness, cylindricity, sphericity, form, parallelism, "
             "perpendicularity (datum), profile, deviation. nominal defaults to the CAD value. "
             "part: {name, number, drawing, revision} for the report"}}},
          {"plan_path",
           {{"type", "string"}, {"format", "path"}, {"description", "Inspection plan file"}}},
          {"cad_path",
           {{"type", "string"},
            {"format", "path"},
            {"description",
             "CAD model as STL in mm; default: the one of the comparison or its nominal object"}}},
          {"search_mm",
           {{"type", "number"},
            {"minimum", 0},
            {"default", 0},
            {"description",
             "How far from the nominal surface scanned points are taken; 0: five voxels"}}},
          {"edge_margin_mm",
           {{"type", "number"},
            {"minimum", 0},
            {"default", 0},
            {"description",
             "Points this close to the edge of an element are left out; 0: two voxels"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    Json plan = Json::object();
    if (p.contains("plan_path")) {
      plan = Json::parse(readTextFile(p.at("plan_path").get<std::string>()));
    }
    if (p.contains("plan")) {
      plan.merge_patch(p.at("plan"));
    }
    const auto comparison_dir = context.inputs.at("comparison");
    const Json compared = Json::parse(readTextFile(comparison_dir / "compare.json"));
    std::filesystem::path cad_path;
    if (p.contains("cad_path")) {
      cad_path = std::filesystem::absolute(p.at("cad_path").get<std::string>());
    }
    const Mesh cad = comparisonNominal(context, compared, comparison_dir, cad_path);
    const RigidTransform cad_to_scan =
        RigidTransform::fromMatrix(compared.at("cad_to_scan").get<std::vector<double>>());
    DimensionalOptions options;
    options.progress = ProgressToken(context.progress, context.cancel);
    options.search_mm = p.at("search_mm").get<double>();
    options.edge_margin_mm = p.at("edge_margin_mm").get<double>();
    context.progress(0.05);
    const SurfaceMask mask = SurfaceMask::open(context.inputs.at("surface") / "surface.vss");
    Measurement measurement = measure(mask, cad, cad_to_scan, plan, options);
    measurement.json["cad_path"] = cad_path.string();
    if (plan.contains("part")) {
      measurement.json["part"] = plan.at("part");
    }
    context.progress(0.8);
    const TelemetryPhase phase("write results");
    writeMeasurement(measurement, cad, context.output_dir / "measurement");
    OperationResult result;
    result.outputs["measurement"] = "measurement";
    result.summary = measurement.json.at("summary");
    result.summary["alignment"] = measurement.json.at("alignment").at("method");
    return result;
  }

 private:
  OperationInfo info_;
};

}  // namespace

void registerSurfaceOperations(OperationRegistry& registry) {
  registry.add(std::make_shared<Surface>());
  registry.add(std::make_shared<CompareCad>());
  registry.add(std::make_shared<WallThickness>());
  registry.add(std::make_shared<CompareObjects>());
  registry.add(std::make_shared<MeasureDimensions>());
}

}  // namespace voxelsieve::operations
