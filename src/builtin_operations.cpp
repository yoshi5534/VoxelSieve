// Operations that ship with VoxelSieve. Each wraps a library function; the command-line tools
// use the same functions.

#include <openvdb/io/File.h>

#include <fstream>
#include <iterator>
#include <stdexcept>

#include "voxelsieve/compare.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/operation.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/report.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/tiff.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

Json datasetSummary(const DatasetInfo& info) {
  Json summary = {{"dims", info.dims}};
  writeVoxelSize(summary, info.voxel_size);
  summary.update({{"levels", info.levels.size()},
                  {"bricks", info.levels.empty() ? 0 : info.levels.front().bricks.size()},
                  {"active_voxels", info.active_voxel_count},
                  {"threshold", info.threshold}});
  return summary;
}

/// Parameters for the voxel size: one edge length for cubic voxels or one per axis, and the slice
/// thickness when slices are thinner than their spacing (ADR 0012).
void addVoxelSizeParameters(Json& properties, const std::string& default_text) {
  properties["voxel_size_mm"] = {
      {"type", {"number", "array"}},
      {"items", {{"type", "number"}, {"exclusiveMinimum", 0}}},
      {"minItems", 3},
      {"maxItems", 3},
      {"exclusiveMinimum", 0},
      {"description",
       "Edge length in mm, or [x, y, z] when the voxels are not cubes (e.g. a "
       "coarser slice spacing). " +
           default_text}};
  properties["slice_thickness_mm"] = {
      {"type", "number"},
      {"exclusiveMinimum", 0},
      {"description",
       "Slice thickness in mm when the slices are thinner than their spacing (a gap between "
       "slices); recorded, measurements use the spacing"}};
}

/// Voxel size from the parameters, if given; the slice thickness alone keeps `fallback`.
std::optional<VoxelSize> voxelSizeParameter(const Json& p,
                                            const std::optional<VoxelSize>& fallback) {
  std::optional<VoxelSize> size = fallback;
  if (p.contains("voxel_size_mm")) {
    size = voxelSizeFromJson(p.at("voxel_size_mm"));
  }
  if (p.contains("slice_thickness_mm")) {
    if (!size) {
      throw std::invalid_argument("slice_thickness_mm needs a voxel size");
    }
    size->slice_thickness_mm = p.at("slice_thickness_mm").get<double>();
  }
  if (size) {
    size->validate();
  }
  return size;
}

class OpenDataset final : public Operation {
 public:
  OpenDataset() {
    info_.id = "open_dataset";
    info_.title = "Datensatz öffnen";
    info_.description =
        "References an existing dataset written by vs-sieve. The dataset is not copied.";
    info_.outputs = {{"dataset", artifact::kDataset, "The dataset"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"path", {{"type", "string"}, {"description", "Dataset directory (.vsieve)"}}}}},
        {"required", {"path"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const std::filesystem::path path =
        std::filesystem::absolute(context.params.at("path").get<std::string>());
    const auto dataset = Dataset::open(path, 1U << 20U);
    OperationResult result;
    result.outputs["dataset"] = path;
    result.summary = datasetSummary(dataset.info());
    return result;
  }

 private:
  OperationInfo info_;
};

class ImportRaw final : public Operation {
 public:
  ImportRaw() {
    info_.id = "import_raw";
    info_.title = "Rohdaten importieren";
    info_.description =
        "Removes the outside air from a raw CT volume and writes a bricked dataset. Dimensions "
        "and voxel size come from the JSON sidecar unless given; a vendor header is skipped.";
    info_.outputs = {{"dataset", artifact::kDataset, "Sieved dataset"}};
    const Json dims = {{"type", "array"},
                       {"items", {{"type", "integer"}, {"minimum", 1}}},
                       {"minItems", 3},
                       {"maxItems", 3},
                       {"description", "Volume dimensions x, y, z in voxels"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"path", {{"type", "string"}, {"description", "Raw volume file"}}},
          {"dims", dims},
          {"sample_type",
           {{"type", "string"}, {"enum", {"uint16", "uint8"}}, {"default", "uint16"}}},
          {"big_endian", {{"type", "boolean"}, {"default", false}}},
          {"header_bytes",
           {{"type", "integer"},
            {"minimum", 0},
            {"description", "Header size; default: file size minus voxel data"}}},
          {"threshold",
           {{"type", "number"}, {"description", "Air/material grey value; default: Otsu"}}},
          {"margin_voxels", {{"type", "integer"}, {"minimum", 0}, {"default", 3}}},
          {"min_material_voxels",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 512},
            {"default", 1},
            {"description",
             "Voxels above the threshold for an 8^3 block to count as material; raise it for "
             "noisy scans"}}},
          {"brick_size", {{"type", "integer"}, {"minimum", 8}, {"default", 256}}}}},
        {"required", {"path"}}};
    addVoxelSizeParameters(info_.parameters["properties"], "Default: from the sidecar");
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const std::filesystem::path path = p.at("path").get<std::string>();
    RawLayout layout;
    Json sidecar;
    if (auto file = std::filesystem::path(path).replace_extension(".json");
        std::filesystem::exists(file)) {
      std::ifstream in(file);
      sidecar = Json::parse(in);
    }
    if (p.contains("dims")) {
      layout.dims = p.at("dims").get<std::array<std::int64_t, 3>>();
    } else if (sidecar.contains("dims")) {
      layout.dims = sidecar.at("dims").get<std::array<std::int64_t, 3>>();
    } else {
      throw std::invalid_argument("No dims given and no sidecar next to " + path.string());
    }
    const auto voxel_size = voxelSizeParameter(p, sidecar.contains("voxel_size_mm")
                                                      ? std::optional(readVoxelSize(sidecar))
                                                      : std::nullopt);
    if (!voxel_size) {
      throw std::invalid_argument("No voxel_size_mm given and no sidecar next to " + path.string());
    }
    layout.voxel_size = *voxel_size;
    layout.sample_type = p.at("sample_type").get<std::string>() == "uint8" ? SampleType::kUInt8
                                                                           : SampleType::kUInt16;
    layout.byte_order = p.at("big_endian").get<bool>() ? std::endian::big : std::endian::little;
    if (p.contains("header_bytes")) {
      layout.header_bytes = p.at("header_bytes").get<std::uint64_t>();
    }
    const MappedRawSource source(path, layout);
    if (source.headerBytes() > 0) {
      context.log("Skipped a header of " + std::to_string(source.headerBytes()) + " bytes");
    }
    DatasetOptions options;
    if (p.contains("threshold")) {
      options.threshold = p.at("threshold").get<float>();
    }
    options.margin_voxels = p.at("margin_voxels").get<int>();
    options.brick_size = p.at("brick_size").get<std::int64_t>();
    options.min_material_voxels = p.at("min_material_voxels").get<int>();
    const DatasetInfo info = writeDataset(source, context.output_dir / "dataset.vsieve", options);
    OperationResult result;
    result.outputs["dataset"] = "dataset.vsieve";
    result.summary = datasetSummary(info);
    result.summary["header_bytes"] = source.headerBytes();
    return result;
  }

 private:
  OperationInfo info_;
};

class ImportTiff final : public Operation {
 public:
  ImportTiff() {
    info_.id = "import_tiff";
    info_.title = "TIFF-Stapel importieren";
    info_.description =
        "Removes the outside air from a TIFF stack and writes a bricked dataset. Reads a "
        "directory of slices, a multi-page TIFF or a ZIP archive of either without extracting "
        "it; slices are sorted by name, numbers by value.";
    info_.outputs = {{"dataset", artifact::kDataset, "Sieved dataset"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"path", {{"type", "string"}, {"description", "Directory, TIFF file or ZIP archive"}}},
          {"folder",
           {{"type", "string"},
            {"description",
             "Folder of the slices when there are several; default: the grey values, not "
             "labels or masks"}}},
          {"threshold",
           {{"type", "number"}, {"description", "Air/material grey value; default: Otsu"}}},
          {"margin_voxels", {{"type", "integer"}, {"minimum", 0}, {"default", 3}}},
          {"min_material_voxels",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 512},
            {"default", 1},
            {"description",
             "Voxels above the threshold for an 8^3 block to count as material; raise it for "
             "noisy scans"}}},
          {"brick_size", {{"type", "integer"}, {"minimum", 8}, {"default", 256}}}}},
        {"required", {"path"}}};
    addVoxelSizeParameters(info_.parameters["properties"], "Default: from the files, else 1 mm");
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    TiffStackOptions tiff;
    tiff.folder = p.value("folder", std::string());
    if (!p.contains("voxel_size_mm") && p.contains("slice_thickness_mm")) {
      // The thickness alone: the spacing comes from the files.
      tiff.voxel_size = TiffStackSource(p.at("path").get<std::string>(), tiff).voxelSize();
    }
    tiff.voxel_size = voxelSizeParameter(p, tiff.voxel_size);
    const TiffStackSource source(p.at("path").get<std::string>(), tiff);
    if (!source.folder().empty()) {
      context.log("Slices from " + source.folder());
    }
    for (const std::string& other : source.otherFolders()) {
      context.log("Also in the input: " + other + " (choose with 'folder')");
    }
    if (!tiff.voxel_size && !source.fileVoxelSize()) {
      context.log("The files give no voxel size; 1 mm assumed. Set voxel_size_mm.");
    }
    context.log("Voxel size " + describe(source.voxelSize()));
    DatasetOptions options;
    if (p.contains("threshold")) {
      options.threshold = p.at("threshold").get<float>();
    }
    options.margin_voxels = p.at("margin_voxels").get<int>();
    options.brick_size = p.at("brick_size").get<std::int64_t>();
    options.min_material_voxels = p.at("min_material_voxels").get<int>();
    const DatasetInfo info = writeDataset(source, context.output_dir / "dataset.vsieve", options);
    OperationResult result;
    result.outputs["dataset"] = "dataset.vsieve";
    result.summary = datasetSummary(info);
    result.summary["slices"] = source.dims()[2];
    result.summary["bits_per_sample"] = source.bitsPerSample();
    return result;
  }

 private:
  OperationInfo info_;
};

class Porosity final : public Operation {
 public:
  Porosity() {
    info_.id = "porosity";
    info_.title = "Porositätsanalyse";
    info_.description =
        "Finds pores and zones of loosened microstructure; writes the result, projection images "
        "and a VDB file with pores and zones.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset to analyse"}};
    info_.outputs = {{"porosity", artifact::kPorosity, "Analysis result"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"min_pore_voxels", {{"type", "integer"}, {"minimum", 1}, {"default", 3}}},
          {"zone_sigma", {{"type", "number"}, {"minimum", 0}, {"default", 5.0}}},
          {"min_zone_void_fraction",
           {{"type", "number"}, {"minimum", 0}, {"maximum", 1}, {"default", 0.01}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    PorosityOptions options;
    options.min_pore_voxels = p.at("min_pore_voxels").get<std::int64_t>();
    options.zone_sigma = p.at("zone_sigma").get<double>();
    options.min_zone_void_fraction = p.at("min_zone_void_fraction").get<double>();
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const PorosityResult porosity = analyzePorosity(dataset, options);
    context.progress(0.8);
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
                      {"porosity", porosity.porosity()}};
    return result;
  }

 private:
  OperationInfo info_;
};

class Surface final : public Operation {
 public:
  Surface() {
    info_.id = "surface";
    info_.title = "Oberfläche";
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

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    SurfaceOptions options;
    options.bits = p.at("bits").get<int>();
    options.band_voxels = p.at("band_voxels").get<double>();
    if (p.contains("iso_value")) {
      options.iso_value = p.at("iso_value").get<float>();
    }
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const auto dir = context.output_dir / "surface";
    std::filesystem::create_directories(dir);
    const SurfaceInfo info = writeSurface(dataset, dir / "surface.vss", options);
    context.progress(0.7);
    const SurfaceMask mask = SurfaceMask::open(dir / "surface.vss");
    writeJson(dir / "surface.json", toJson(info));
    writeSurfaceImages(mask, dir);
    if (p.at("stl").get<bool>()) {
      writeStl(dir / "surface.stl", mask.toMesh());
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
    info_.title = "Soll-Ist-Vergleich";
    info_.description =
        "Aligns the nominal geometry (CAD model as STL, mm) to the extracted surface and measures "
        "the signed deviation of every surface point from it: positive where the part has more "
        "material than nominal. Writes compare.json, deviation.ply (coloured mesh), views "
        "along the axes and optionally the CAD model in scan coordinates (cad_aligned.stl).";
    info_.inputs = {{"surface", artifact::kSurface, "Surface of the scan"}};
    info_.outputs = {{"comparison", artifact::kComparison, "Deviations from the CAD model"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"cad_path", {{"type", "string"}, {"description", "CAD model as STL in mm"}}},
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
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const std::filesystem::path cad_path =
        std::filesystem::absolute(p.at("cad_path").get<std::string>());
    CompareOptions options;
    options.alignment = alignmentFromString(p.at("alignment").get<std::string>());
    options.tolerance_mm = p.at("tolerance_mm").get<double>();
    options.outer_surface_only = p.at("outer_surface_only").get<bool>();
    const Mesh cad = readStl(cad_path);
    context.progress(0.1);
    const SurfaceMask mask = SurfaceMask::open(context.inputs.at("surface") / "surface.vss");
    const CompareResult compared = compareToCad(mask, cad, options);
    context.progress(0.8);
    const auto dir = context.output_dir / "comparison";
    writeComparison(compared, dir);
    if (p.at("aligned_stl").get<bool>()) {
      writeAlignedCad(compared, cad, dir / "cad_aligned.stl");
    }
    Json json = toJson(compared);
    json["cad_path"] = cad_path.string();
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
    return result;
  }

 private:
  OperationInfo info_;
};

class Report final : public Operation {
 public:
  Report() {
    info_.id = "report";
    info_.title = "Prüfbericht";
    info_.description =
        "Evaluates the porosity result against the acceptance limits of the inspection order "
        "(BDG P 202 scheme) and writes report.html and report.json. With a surface, the report "
        "shows 3D views of the part and its pores; with a comparison, the nominal-actual "
        "comparison with its views and histogram.";
    info_.inputs = {{"porosity", artifact::kPorosity, "Porosity result"},
                    {"surface", artifact::kSurface, "Surface for the 3D views of the part", true},
                    {"comparison", artifact::kComparison, "Nominal-actual comparison", true}};
    info_.outputs = {{"report", artifact::kReport, "Report directory"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"order",
           {{"type", "object"},
            {"description",
             "Inspection order: laboratory, customer, part, scan, acceptance limits (see "
             "examples/inspection_order.json)"}}},
          {"order_path", {{"type", "string"}, {"description", "Inspection order file"}}},
          {"template_path", {{"type", "string"}, {"description", "Report template file"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    Json order = Json::object();
    if (p.contains("order_path")) {
      order = Json::parse(readText(p.at("order_path").get<std::string>()));
    }
    if (p.contains("order")) {
      order.merge_patch(p.at("order"));
    }
    const std::string report_template = p.contains("template_path")
                                            ? readText(p.at("template_path").get<std::string>())
                                            : std::string(defaultReportTemplate());
    const auto porosity_dir = context.inputs.at("porosity");
    const PorosityResult porosity = loadPorosityResult(porosity_dir);
    PorosityOptions options;
    if (std::filesystem::exists(porosity_dir / "options.json")) {
      const Json used = Json::parse(readText(porosity_dir / "options.json"));
      options.min_pore_voxels = used.value("min_pore_voxels", options.min_pore_voxels);
      options.zone_sigma = used.value("zone_sigma", options.zone_sigma);
      options.min_zone_void_fraction =
          used.value("min_zone_void_fraction", options.min_zone_void_fraction);
    }
    const auto zones = inspectionZonesFromJson(order);
    const Evaluation evaluation = evaluate(porosity, zones);
    std::vector<std::string> warnings;
    Json data = reportData(order, porosity, options, evaluation, porosity_dir, &warnings);
    if (const auto surface = context.inputs.find("surface"); surface != context.inputs.end()) {
      context.log("Rendering the part and its pores");
      addPartImages(data, SurfaceMask::open(surface->second / "surface.vss"), porosity);
    }
    if (const auto comparison = context.inputs.find("comparison");
        comparison != context.inputs.end()) {
      addComparison(data, comparison->second);
    }
    const auto dir = context.output_dir / "report";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "report.html", std::ios::binary) << renderTemplate(report_template, data);
    data.erase("images");
    writeJson(dir / "report.json", data);
    for (const std::string& warning : warnings) {
      context.log(warning);
    }
    OperationResult result;
    result.outputs["report"] = "report";
    result.summary = {{"evaluated_zones", evaluation.zones.size()},
                      {"missing_fields", warnings.size()}};
    if (!zones.empty()) {
      result.summary["passed"] = evaluation.passed();
    }
    return result;
  }

 private:
  static std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      throw std::runtime_error("Cannot read " + path.string());
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  OperationInfo info_;
};

class SegmentMaterials final : public Operation {
 public:
  SegmentMaterials() {
    info_.id = "segment_materials";
    info_.title = "Materialsegmentierung";
    info_.description =
        "Splits the part into materials by grey value: a voxel is material when it and enough "
        "of its neighbours are above the air threshold (noise spikes go, walls one voxel thin "
        "stay), material grows into touching voxels above a lower threshold, and the classes are "
        "separated by thresholds (multi-level Otsu unless given). Writes a material volume with "
        "volumes per material; the slice view shows the materials in colour.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset to segment"}};
    info_.outputs = {{"materials", artifact::kMaterials, "Material volume"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"materials", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}, {"default", 2}}},
          {"air_threshold",
           {{"type", "number"}, {"description", "Default: the dataset's threshold"}}},
          {"material_thresholds",
           {{"type", "array"},
            {"items", {{"type", "number"}}},
            {"description",
             "Thresholds between the classes (materials - 1, ascending); default: Otsu"}}},
          {"min_neighbours",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 27},
            {"default", 6},
            {"description", "Voxels of the 3x3x3 neighbourhood above the air threshold"}}},
          {"grow_steps", {{"type", "integer"}, {"minimum", 0}, {"default", 1}}},
          {"grow_fraction",
           {{"type", "number"},
            {"minimum", 0},
            {"maximum", 1},
            {"default", 0.5},
            {"description", "Lower threshold between air level (0) and air threshold (1)"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    SegmentationOptions options;
    options.materials = p.at("materials").get<int>();
    if (p.contains("air_threshold")) {
      options.air_threshold = p.at("air_threshold").get<float>();
    }
    if (p.contains("material_thresholds")) {
      options.material_thresholds = p.at("material_thresholds").get<std::vector<float>>();
    }
    options.min_neighbours = p.at("min_neighbours").get<int>();
    options.grow_steps = p.at("grow_steps").get<int>();
    options.grow_fraction = p.at("grow_fraction").get<float>();
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const MaterialVolumeInfo info =
        segmentMaterials(dataset, context.output_dir / "materials", options);
    OperationResult result;
    result.outputs["materials"] = "materials";
    Json materials = Json::array();
    for (const Material& material : info.materials) {
      materials.push_back({{"id", material.id},
                           {"from_grey_value", material.lower},
                           {"voxels", material.voxel_count},
                           {"volume_mm3", material.volume_mm3}});
    }
    result.summary = {{"air_threshold", info.air_threshold}, {"materials", materials}};
    return result;
  }

 private:
  OperationInfo info_;
};

}  // namespace

void registerBuiltinOperations(OperationRegistry& registry) {
  registry.add(std::make_shared<OpenDataset>());
  registry.add(std::make_shared<ImportRaw>());
  registry.add(std::make_shared<ImportTiff>());
  registry.add(std::make_shared<Porosity>());
  registry.add(std::make_shared<SegmentMaterials>());
  registry.add(std::make_shared<Surface>());
  registry.add(std::make_shared<CompareCad>());
  registry.add(std::make_shared<Report>());
}

}  // namespace voxelsieve
