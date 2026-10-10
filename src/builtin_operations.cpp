// Operations that ship with VoxelSieve. Each wraps a library function; the command-line tools
// use the same functions.

#include <openvdb/io/File.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "voxelsieve/compare.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/dicom.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/model.hpp"
#include "voxelsieve/operation.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/preview.hpp"
#include "voxelsieve/project.hpp"
#include "voxelsieve/report.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/telemetry.hpp"
#include "voxelsieve/tiff.hpp"
#include "voxelsieve/vgl.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

/// The `value_type` parameter of the imports (ADR 0021).
Json valueTypeParameter() {
  return {{"type", "string"},
          {"enum", {"native", "uint16", "float"}},
          {"default", "native"},
          {"description",
           "Stored grey values: native keeps the input's 8 or 16 bits; float for viewers such as "
           "Blender and Houdini, which cannot read VoxelSieve's integer grids"}};
}

std::optional<ValueType> valueTypeOption(const Json& params) {
  const std::string name = params.value("value_type", std::string("native"));
  if (name == "native") {
    return std::nullopt;
  }
  return parseValueType(name);
}

Json datasetSummary(const DatasetInfo& info) {
  Json summary = {{"dims", info.dims}};
  writeVoxelSize(summary, info.voxel_size);
  summary.update({{"levels", info.levels.size()},
                  {"bricks", info.levels.empty() ? 0 : info.levels.front().bricks.size()},
                  {"active_voxels", info.active_voxel_count},
                  {"value_type", std::string(valueTypeName(info.value_type))},
                  {"threshold", info.threshold}});
  if (!info.value_mapping.isIdentity()) {
    summary["value_mapping"] = {{"offset", info.value_mapping.offset},
                                {"scale", info.value_mapping.scale}};
  }
  return summary;
}

/// Forwards the progress of writeDataset as one fraction: staging a slow source up to 30 %, pass
/// 1 up to 50 % (from 0 without staging), pass 2 up to 90 %, the coarser levels the rest.
std::function<void(std::string_view, double)> datasetProgress(const OperationContext& context) {
  auto staged = std::make_shared<bool>(false);
  return [&context, staged](std::string_view stage, double fraction) {
    if (stage == "staging") {
      *staged = true;
      context.progress(0.3 * fraction);
    } else if (stage == "histogram") {
      const double start = *staged ? 0.3 : 0.0;
      context.progress(start + (0.5 - start) * fraction);
    } else if (stage == "bricks") {
      context.progress(0.5 + 0.4 * fraction);
    } else {
      context.progress(0.9 + 0.1 * fraction);
    }
  };
}

/// Shows the preview of an import as soon as it is read (ADR 0020) and keeps it with the step as
/// preview.json and preview.png. The time counts from the start of the operation, as the user
/// waits.
std::function<void(const ImportPreview&)> importPreview(const OperationContext& context) {
  const auto start = std::chrono::steady_clock::now();
  return [&context, start](const ImportPreview& preview) {
    const std::vector<std::uint8_t> png = previewPng(preview);
    Json summary = previewSummary(preview);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    summary["seconds"] = seconds;
    std::ofstream(context.output_dir / "preview.json") << summary.dump(2) << '\n';
    std::ofstream(context.output_dir / "preview.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << "Preview after " << seconds
         << " s: " << preview.slices.size() << " of " << preview.source_dims[2] << " slices ("
         << 100.0 * preview.fractionRead() << " % of the voxels), threshold estimate "
         << std::setprecision(0) << preview.threshold;
    context.log(text.str());
    context.preview(summary, png);
  };
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
    info_.title = "Open dataset";
    info_.description =
        "References an existing dataset written by vs-sieve. The dataset is not copied.";
    info_.outputs = {{"dataset", artifact::kDataset, "The dataset"}};
    info_.parameters = {{"type", "object"},
                        {"properties",
                         {{"path",
                           {{"type", "string"},
                            {"format", "path"},
                            {"description", "Dataset directory (.vsieve)"}}}}},
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
    info_.title = "Import raw volume";
    info_.description =
        "Removes the outside air from a raw CT volume and writes a bricked dataset. Dimensions "
        "and voxel size come from the JSON sidecar unless given; a vendor header is skipped. "
        "gzip-compressed files (.raw.gz, sidecar <name>.json) are decompressed as they are "
        "read; give header_bytes when they have a header.";
    info_.outputs = {{"dataset", artifact::kDataset, "Sieved dataset"}};
    const Json dims = {{"type", "array"},
                       {"items", {{"type", "integer"}, {"minimum", 1}}},
                       {"minItems", 3},
                       {"maxItems", 3},
                       {"description", "Volume dimensions x, y, z in voxels"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"path",
           {{"type", "string"},
            {"format", "path"},
            {"description", "Raw volume file, plain or gzip-compressed"}}},
          {"dims", dims},
          {"sample_type",
           {{"type", "string"}, {"enum", {"uint16", "uint8"}}, {"default", "uint16"}}},
          {"big_endian", {{"type", "boolean"}, {"default", false}}},
          {"header_bytes",
           {{"type", "integer"},
            {"minimum", 0},
            {"description",
             "Header size; default: file size minus voxel data (0 for gzip-compressed files)"}}},
          {"threshold",
           {{"type", "number"},
            {"description",
             "Air/material grey value; default: estimated (valley after the air peak)"}}},
          {"margin_voxels", {{"type", "integer"}, {"minimum", 0}, {"default", 3}}},
          {"min_material_voxels",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 512},
            {"default", 1},
            {"description",
             "Voxels above the threshold for an 8^3 block to count as material; raise it for "
             "noisy scans"}}},
          {"outside_air_axes",
           {{"type", "string"},
            {"pattern", "^[xyz]+$"},
            {"default", "xyz"},
            {"description",
             "Axes whose boundary faces let outside air in; xy when the first and last slice cut "
             "through the part (a pipe), so its inside is kept"}}},
          {"brick_size", {{"type", "integer"}, {"minimum", 8}, {"default", 256}}},
          {"value_type", valueTypeParameter()}}},
        {"required", {"path"}}};
    addVoxelSizeParameters(info_.parameters["properties"], "Default: from the sidecar");
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const std::filesystem::path path = p.at("path").get<std::string>();
    RawLayout layout;
    Json sidecar;
    if (const auto file = rawSidecarPath(path); std::filesystem::exists(file)) {
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
    std::uint64_t header_bytes = 0;
    const auto opened = openRawVolume(path, layout, &header_bytes);
    const VolumeSource& source = *opened;
    if (header_bytes > 0) {
      context.log("Skipped a header of " + std::to_string(header_bytes) + " bytes");
    }
    if (source.sequentialAccess()) {
      context.log("gzip-compressed: decompressed once, in slice order, into the staging copy");
    }
    DatasetOptions options;
    if (p.contains("threshold")) {
      options.threshold = p.at("threshold").get<float>();
    }
    options.margin_voxels = p.at("margin_voxels").get<int>();
    options.brick_size = p.at("brick_size").get<std::int64_t>();
    options.min_material_voxels = p.at("min_material_voxels").get<int>();
    options.outside_air_axes = parseAirAxes(p.value("outside_air_axes", std::string("xyz")));
    options.value_type = valueTypeOption(p);
    options.progress = datasetProgress(context);
    options.preview = importPreview(context);
    const DatasetInfo info = writeDataset(source, context.output_dir / "dataset.vsieve", options);
    OperationResult result;
    result.outputs["dataset"] = "dataset.vsieve";
    result.summary = datasetSummary(info);
    result.summary["header_bytes"] = header_bytes;
    return result;
  }

 private:
  OperationInfo info_;
};

class ImportTiff final : public Operation {
 public:
  ImportTiff() {
    info_.id = "import_tiff";
    info_.title = "Import TIFF stack";
    info_.description =
        "Removes the outside air from a TIFF stack and writes a bricked dataset. Reads a "
        "directory of slices, a multi-page TIFF or a ZIP archive of either without extracting "
        "it; slices are sorted by name, numbers by value. Float slices are mapped linearly onto "
        "16-bit grey values, and the mapping is kept with the dataset.";
    info_.outputs = {{"dataset", artifact::kDataset, "Sieved dataset"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"path",
           {{"type", "string"},
            {"format", "path"},
            {"description", "Directory, TIFF file or ZIP archive"}}},
          {"value_range",
           {{"type", "array"},
            {"items", {{"type", "number"}}},
            {"minItems", 2},
            {"maxItems", 2},
            {"description",
             "Float slices: the values mapped to grey 0 and 65535; values outside are clipped. "
             "Default: estimated from a few slices"}}},
          {"folder",
           {{"type", "string"},
            {"description",
             "Folder of the slices when there are several; default: the grey values, not "
             "labels or masks"}}},
          {"threshold",
           {{"type", "number"},
            {"description",
             "Air/material grey value; default: estimated (valley after the air peak)"}}},
          {"margin_voxels", {{"type", "integer"}, {"minimum", 0}, {"default", 3}}},
          {"min_material_voxels",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 512},
            {"default", 1},
            {"description",
             "Voxels above the threshold for an 8^3 block to count as material; raise it for "
             "noisy scans"}}},
          {"outside_air_axes",
           {{"type", "string"},
            {"pattern", "^[xyz]+$"},
            {"default", "xyz"},
            {"description",
             "Axes whose boundary faces let outside air in; xy when the first and last slice cut "
             "through the part (a pipe), so its inside is kept"}}},
          {"brick_size", {{"type", "integer"}, {"minimum", 8}, {"default", 256}}},
          {"value_type", valueTypeParameter()}}},
        {"required", {"path"}}};
    addVoxelSizeParameters(info_.parameters["properties"], "Default: from the files, else 1 mm");
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    TiffStackOptions tiff;
    tiff.folder = p.value("folder", std::string());
    if (p.contains("value_range")) {
      tiff.value_range = p.at("value_range").get<std::array<double, 2>>();
    }
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
    if (source.isFloat()) {
      const auto range = source.valueRange();
      context.log("Float values " + std::to_string(range[0]) + " to " + std::to_string(range[1]) +
                  " mapped to grey 0 to 65535" +
                  (tiff.value_range ? "" : " (estimated; set value_range)"));
    }
    DatasetOptions options;
    if (p.contains("threshold")) {
      options.threshold = p.at("threshold").get<float>();
    }
    options.margin_voxels = p.at("margin_voxels").get<int>();
    options.brick_size = p.at("brick_size").get<std::int64_t>();
    options.min_material_voxels = p.at("min_material_voxels").get<int>();
    options.outside_air_axes = parseAirAxes(p.value("outside_air_axes", std::string("xyz")));
    options.value_type = valueTypeOption(p);
    options.progress = datasetProgress(context);
    options.preview = importPreview(context);
    const DatasetInfo info = writeDataset(source, context.output_dir / "dataset.vsieve", options);
    OperationResult result;
    result.outputs["dataset"] = "dataset.vsieve";
    result.summary = datasetSummary(info);
    result.summary["slices"] = source.dims()[2];
    result.summary["bits_per_sample"] = source.bitsPerSample();
    if (source.isFloat()) {
      result.summary["value_range"] = source.valueRange();
      result.summary["clipped_values"] = source.clippedValues();
      if (source.clippedValues() > 0) {
        context.log(std::to_string(source.clippedValues()) +
                    " values lay outside the value range and were clipped; widen value_range");
      }
    }
    return result;
  }

 private:
  OperationInfo info_;
};

/// Parameters of the sieve shared by the imports of DICOM stacks and VGStudio projects.
void addSieveParameters(Json& properties) {
  properties["threshold"] = {
      {"type", "number"},
      {"description", "Air/material grey value; default: estimated (valley after the air peak)"}};
  properties["margin_voxels"] = {{"type", "integer"}, {"minimum", 0}, {"default", 3}};
  properties["min_material_voxels"] = {
      {"type", "integer"},
      {"minimum", 1},
      {"maximum", 512},
      {"default", 1},
      {"description",
       "Voxels above the threshold for an 8^3 block to count as material; raise it for noisy "
       "scans"}};
  properties["outside_air_axes"] = {
      {"type", "string"},
      {"pattern", "^[xyz]+$"},
      {"default", "xyz"},
      {"description",
       "Axes whose boundary faces let outside air in; xy when the first and last slice cut "
       "through the part (a pipe), so its inside is kept"}};
  properties["brick_size"] = {{"type", "integer"}, {"minimum", 8}, {"default", 256}};
  properties["value_type"] = valueTypeParameter();
}

DatasetOptions sieveOptions(const OperationContext& context) {
  const Json& p = context.params;
  DatasetOptions options;
  if (p.contains("threshold")) {
    options.threshold = p.at("threshold").get<float>();
  }
  options.margin_voxels = p.value("margin_voxels", 3);
  options.brick_size = p.value("brick_size", std::int64_t{256});
  options.min_material_voxels = p.value("min_material_voxels", 1);
  options.outside_air_axes = parseAirAxes(p.value("outside_air_axes", std::string("xyz")));
  options.value_type = valueTypeOption(p);
  options.progress = datasetProgress(context);
  options.preview = importPreview(context);
  return options;
}

/// Places the object a step creates where its source says it lies (ADR 0018), as the motion of
/// the step; nothing for the identity.
void placeObject(const OperationContext& context, const RigidTransform& pose, Json& summary) {
  if (context.object.empty() || pose.matrix() == RigidTransform{}.matrix()) {
    return;
  }
  summary[kMovedObjectsKey] = Json::array({context.object});
  summary[kMotionKey] = pose.matrix();
}

void logDicom(const DicomStackSource& source, const OperationContext& context) {
  for (const auto& [uid, slices] : source.otherSeries()) {
    context.log("Also in the input: series " + uid + " with " + std::to_string(slices) +
                " slices (choose with 'series')");
  }
  if (source.skippedFiles() > 0) {
    context.log("Skipped " + std::to_string(source.skippedFiles()) +
                " files that are not DICOM images");
  }
  if (const ValueMapping mapping = source.valueMapping(); !mapping.isIdentity()) {
    context.log("Stored values = " + std::to_string(mapping.offset) + " + " +
                std::to_string(mapping.scale) + " * grey" +
                (source.isSigned() ? " (signed samples shifted by 32768)" : ""));
  }
}

/// Writes the dataset of a DICOM stack and describes it.
OperationResult sieveDicom(const DicomStackSource& source, const RigidTransform& pose,
                           const OperationContext& context) {
  const DatasetInfo info =
      writeDataset(source, context.output_dir / "dataset.vsieve", sieveOptions(context));
  OperationResult result;
  result.outputs["dataset"] = "dataset.vsieve";
  result.summary = datasetSummary(info);
  result.summary["slices"] = source.dims()[2];
  result.summary["bits_stored"] = source.bitsStored();
  result.summary["signed"] = source.isSigned();
  placeObject(context, pose, result.summary);
  return result;
}

std::string describeDims(const std::array<std::int64_t, 3>& dims) {
  return std::to_string(dims[0]) + " x " + std::to_string(dims[1]) + " x " +
         std::to_string(dims[2]);
}

class ImportDicom final : public Operation {
 public:
  ImportDicom() {
    info_.id = "import_dicom";
    info_.title = "Import DICOM stack";
    info_.description =
        "Removes the outside air from a stack of DICOM slices and writes a bricked dataset. "
        "Slices are sorted by their position, the voxel size comes from the files, and the "
        "object is placed where the files say it lies. Signed samples and the rescale slope and "
        "intercept are kept with the dataset as a value mapping.";
    info_.outputs = {{"dataset", artifact::kDataset, "Sieved dataset"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"path",
           {{"type", "string"},
            {"format", "path"},
            {"description", "Directory of slices, or one slice file"}}},
          {"series",
           {{"type", "string"},
            {"description",
             "Series Instance UID when the directory holds several; default: the series with "
             "the most slices"}}}}},
        {"required", {"path"}}};
    addSieveParameters(info_.parameters["properties"]);
    addVoxelSizeParameters(info_.parameters["properties"], "Default: from the files");
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    DicomStackOptions dicom;
    dicom.series = p.value("series", std::string());
    const std::filesystem::path path = p.at("path").get<std::string>();
    if (!p.contains("voxel_size_mm") && p.contains("slice_thickness_mm")) {
      // The thickness alone: the spacing comes from the files.
      dicom.voxel_size = DicomStackSource(path, dicom).voxelSize();
    }
    dicom.voxel_size = voxelSizeParameter(p, dicom.voxel_size);
    const DicomStackSource source(path, dicom);
    logDicom(source, context);
    if (!dicom.voxel_size && !source.fileVoxelSize()) {
      context.log("The files give no pixel spacing; 1 mm assumed. Set voxel_size_mm.");
    }
    context.log(describeDims(source.dims()) + " voxels, voxel size " +
                describe(source.voxelSize()));
    OperationResult result = sieveDicom(source, source.filePose(), context);
    result.summary["series"] = source.seriesUid();
    return result;
  }

 private:
  OperationInfo info_;
};

class ImportVgl final : public Operation {
 public:
  ImportVgl() {
    info_.id = "import_vgl";
    info_.title = "Import VGStudio project";
    info_.description =
        "Imports a volume of a VGStudio project (.vgl) from the files it was imported from, "
        "found where the project names them or next to the project, and places it as in the "
        "project. Removes the outside air and writes a bricked dataset. DICOM stacks are read; "
        "other inputs and further files of the project, such as masks, are reported.";
    info_.outputs = {{"dataset", artifact::kDataset, "Sieved dataset"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"path",
           {{"type", "string"}, {"format", "path"}, {"description", "VGStudio project (.vgl)"}}},
          {"volume",
           {{"type", "integer"},
            {"minimum", 0},
            {"default", 0},
            {"description", "Which volume of the project, counted from 0"}}}}},
        {"required", {"path"}}};
    addSieveParameters(info_.parameters["properties"]);
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const VglProject project = readVglProject(p.at("path").get<std::string>());
    if (project.volumes.empty()) {
      throw std::runtime_error("The project holds no volume");
    }
    const auto chosen = p.value("volume", std::size_t{0});
    if (chosen >= project.volumes.size()) {
      throw std::invalid_argument("The project holds " + std::to_string(project.volumes.size()) +
                                  " volumes; 'volume' counts from 0");
    }
    context.log("Project of " + project.app_name + " " + project.app_version + " with " +
                std::to_string(project.volumes.size()) + " volume(s)");
    for (std::size_t i = 0; i < project.volumes.size(); ++i) {
      if (i != chosen) {
        context.log("Also in the project: volume " + std::to_string(i) + " '" +
                    project.volumes[i].name + "' (choose with 'volume')");
      }
    }
    for (const VglReference& other : project.other_files) {
      context.log("Not imported: " + other.description + ", " +
                  (other.file.path ? other.file.path->filename().string()
                                   : other.file.reference + " (not found)"));
    }
    const VglVolume& volume = project.volumes[chosen];
    context.log("Volume '" + volume.name + "': " + describeDims(volume.dims) + " " +
                volume.sample_type + ", voxel size " + describe(volume.voxel_size));
    for (const std::string& note : volume.notes) {
      context.log("Not applied: " + note);
    }
    if (volume.format != "dicom") {
      throw std::runtime_error("Volume '" + volume.name + "' was imported with " +
                               volume.import_class +
                               ", which VoxelSieve does not read yet; DICOM stacks are supported");
    }
    std::vector<std::filesystem::path> files;
    std::size_t missing = 0;
    for (const VglFile& file : volume.files) {
      if (file.path) {
        files.push_back(*file.path);
      } else if (missing++ == 0) {
        context.log("Missing: " + file.reference);
      }
    }
    if (missing > 0) {
      throw std::runtime_error(std::to_string(missing) + " of " +
                               std::to_string(volume.files.size()) +
                               " files of the volume were not found, neither where the project "
                               "names them nor next to it");
    }
    DicomStackOptions dicom;
    dicom.voxel_size = volume.voxel_size;
    const DicomStackSource source(files, dicom);
    logDicom(source, context);
    if (source.dims() != volume.dims) {
      context.log("The files hold " + describeDims(source.dims()) + " voxels, the project " +
                  describeDims(volume.dims));
    }
    if (const auto from_files = source.fileVoxelSize();
        from_files && describe(*from_files) != describe(volume.voxel_size)) {
      context.log("Voxel size of the project used; the files give " + describe(*from_files));
    }
    OperationResult result = sieveDicom(source, volume.pose, context);
    result.summary["volume"] = volume.name;
    result.summary["format"] = volume.format;
    result.summary["not_imported"] = project.other_files.size();
    return result;
  }

 private:
  OperationInfo info_;
};

class Porosity final : public Operation {
 public:
  Porosity() {
    info_.id = "porosity";
    info_.title = "Porosity analysis";
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
    std::optional<TelemetryPhase> phase(std::in_place, "surface mask");
    const SurfaceInfo info = writeSurface(dataset, dir / "surface.vss", options);
    context.progress(0.7);
    phase.reset();
    phase.emplace("images and meshes");
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
    info_.title = "Nominal-actual comparison";
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
    const TelemetryPhase phase("write results");
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
    info_.title = "Test report";
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
          {"order_path",
           {{"type", "string"}, {"format", "path"}, {"description", "Inspection order file"}}},
          {"template_path",
           {{"type", "string"}, {"format", "path"}, {"description", "Report template file"}}}}}};
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
    std::optional<TelemetryPhase> phase(std::in_place, "evaluation");
    const auto zones = inspectionZonesFromJson(order);
    const Evaluation evaluation = evaluate(porosity, zones);
    std::vector<std::string> warnings;
    Json data = reportData(order, porosity, options, evaluation, porosity_dir, &warnings);
    if (const auto surface = context.inputs.find("surface"); surface != context.inputs.end()) {
      context.log("Rendering the part and its pores");
      phase.reset();
      phase.emplace("images");
      addPartImages(data, SurfaceMask::open(surface->second / "surface.vss"), porosity);
    }
    if (const auto comparison = context.inputs.find("comparison");
        comparison != context.inputs.end()) {
      addComparison(data, comparison->second);
    }
    phase.reset();
    phase.emplace("write report");
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
    info_.title = "Material segmentation";
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

class SegmentWithModel final : public Operation {
 public:
  SegmentWithModel() {
    info_.id = "segment_model";
    info_.title = "Learned segmentation";
    info_.description =
        "Splits the part into materials with a learned model (.vsm, a small 3D network trained "
        "for example with tools/models). VoxelSieve runs it on the CPU tile by tile, so it works "
        "on datasets larger than RAM. Writes a material volume like the threshold segmentation; "
        "the slice view shows the materials in colour.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset to segment"}};
    info_.outputs = {{"materials", artifact::kMaterials, "Material volume"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"model_path",
           {{"type", "string"}, {"format", "path"}, {"description", "Learned model (.vsm)"}}},
          {"tile",
           {{"type", "integer"},
            {"minimum", 16},
            {"default", 128},
            {"description", "Edge of the tiles the model runs on, in voxels"}}}}},
        {"required", {"model_path"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const Model model =
        Model::load(std::filesystem::absolute(p.at("model_path").get<std::string>()));
    ModelSegmentationOptions options;
    options.tile = p.at("tile").get<std::int64_t>();
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const MaterialVolumeInfo info =
        segmentMaterialsWithModel(dataset, model, context.output_dir / "materials", options);
    OperationResult result;
    result.outputs["materials"] = "materials";
    Json materials = Json::array();
    for (const Material& material : info.materials) {
      materials.push_back({{"id", material.id},
                           {"name", material.name},
                           {"voxels", material.voxel_count},
                           {"volume_mm3", material.volume_mm3}});
    }
    result.summary = {{"model", info.model}, {"materials", materials}};
    return result;
  }

 private:
  OperationInfo info_;
};

class AddMesh final : public Operation {
 public:
  AddMesh() {
    info_.id = "add_mesh";
    info_.title = "Add mesh";
    info_.description =
        "Adds a triangle mesh in mm, such as a CAD model, as an object of the project. It starts "
        "in the coordinates of the file; move or align it afterwards. The file is not copied.";
    info_.outputs = {{"mesh", artifact::kMesh, "The mesh (STL)"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"path", {{"type", "string"}, {"format", "path"}, {"description", "STL file in mm"}}}}},
        {"required", {"path"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const std::filesystem::path path =
        std::filesystem::absolute(context.params.at("path").get<std::string>());
    const Mesh mesh = readStl(path);
    if (mesh.triangles.empty()) {
      throw std::invalid_argument("The mesh has no triangles: " + path.string());
    }
    const Bounds bounds = meshBounds(mesh);
    OperationResult result;
    result.outputs["mesh"] = path;
    result.summary = {{"triangles", mesh.triangles.size()},
                      {"bounds_min_mm", bounds.min},
                      {"bounds_max_mm", bounds.max},
                      {"volume_mm3", meshVolumeMm3(mesh)}};
    return result;
  }

 private:
  OperationInfo info_;
};

/// An entry of OperationContext::objects.
const Json& objectEntry(const OperationContext& context, const std::string& id) {
  for (const Json& object : context.objects) {
    if (object.at("id") == id) {
      return object;
    }
  }
  throw std::invalid_argument("No object '" + id + "'");
}

RigidTransform entryPose(const Json& object) {
  return RigidTransform::fromMatrix(object.at("pose").get<std::vector<double>>());
}

/// The surface of an object in its own coordinates (mm): a mesh object's triangles, or the
/// extracted surface of a volume (which needs a surface step). `resolution_mm` is set to half the
/// smallest voxel pitch of a volume and left alone for a mesh.
IndexedMesh objectSurface(const Json& object, std::size_t max_triangles, bool outer_only,
                          double& resolution_mm) {
  const Json& outputs = object.at("outputs");
  const std::string name = object.at("name").get<std::string>();
  if (object.at("kind") == kMeshObject) {
    return indexedMesh(readStl(outputs.at(artifact::kMesh).get<std::string>()));
  }
  if (!outputs.contains(artifact::kSurface)) {
    throw std::invalid_argument("Object '" + name +
                                "' has no surface yet: run the surface extraction on it first");
  }
  const SurfaceMask mask = SurfaceMask::open(
      std::filesystem::path(outputs.at(artifact::kSurface).get<std::string>()) / "surface.vss");
  const double half_voxel = 0.5 * mask.info().voxel_size.minMm();
  resolution_mm = resolution_mm > 0.0 ? std::min(resolution_mm, half_voxel) : half_voxel;
  return scanSurface(mask, max_triangles, outer_only).mesh;
}

/// Writes pose.json and fills the summary of a step that moves objects.
OperationResult movedResult(const OperationContext& context, const std::vector<std::string>& ids,
                            const RigidTransform& motion, Json summary) {
  Json poses = Json::object();
  for (const std::string& id : ids) {
    poses[id] = motion.after(entryPose(objectEntry(context, id))).matrix();
  }
  summary[kMovedObjectsKey] = ids;
  summary[kMotionKey] = motion.matrix();
  summary["rotation_deg"] = motion.angleDegrees();
  summary["translation_mm"] = motion.translation;
  Json record = summary;
  record["poses"] = poses;
  std::ofstream(context.output_dir / "pose.json") << record.dump(2) << '\n';
  OperationResult result;
  result.outputs["pose"] = "pose.json";
  result.summary = std::move(summary);
  return result;
}

/// The ids in the `objects` parameter, each known and named once.
std::vector<std::string> movedObjects(const OperationContext& context) {
  const auto ids = context.params.at("objects").get<std::vector<std::string>>();
  for (const std::string& id : ids) {
    if (std::ranges::count(ids, id) > 1) {
      throw std::invalid_argument("Object '" + id + "' is named twice");
    }
    (void)objectEntry(context, id);
  }
  return ids;
}

const Json& movedObjectsSchema() {
  static const Json schema = {
      {"type", "array"},
      {"items", {{"type", "string"}}},
      {"minItems", 1},
      {"description", "Ids of the objects to move together; the first one is fitted"}};
  return schema;
}

class MoveObjects final : public Operation {
 public:
  MoveObjects() {
    const auto vector3 = [](const Json& extra) {
      Json schema = {
          {"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};
      schema.update(extra);
      return schema;
    };
    info_.id = "move";
    info_.title = "Move objects";
    info_.description =
        "Moves objects together in the global coordinate system of the project by a rigid "
        "motion: a rotation about an axis through a centre, then a translation. Alternatively "
        "`pose` places one object at a given pose. Moves apply on top of the earlier ones, in "
        "order; undo takes them back. The data is not touched.";
    info_.outputs = {{"pose", artifact::kPose, "The motion and the new poses (pose.json)"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"objects",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"minItems", 1},
            {"description", "Ids of the objects to move together"}}},
          {"translation_mm", vector3({{"default", {0.0, 0.0, 0.0}}})},
          {"rotation_axis", vector3({{"default", {0.0, 0.0, 1.0}}})},
          {"rotation_deg", {{"type", "number"}, {"default", 0.0}}},
          {"center_mm", vector3({{"default", {0.0, 0.0, 0.0}},
                                 {"description", "Point the rotation axis passes through"}})},
          {"pose",
           {{"type", "array"},
            {"items", {{"type", "number"}}},
            {"minItems", 12},
            {"maxItems", 16},
            {"description",
             "New pose of a single object, object to global coordinates, as a row-major 3x4 or "
             "4x4 matrix in mm; replaces the rotation and translation"}}}}},
        {"required", {"objects"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const auto ids = movedObjects(context);
    RigidTransform motion;
    if (p.contains("pose")) {
      if (ids.size() != 1) {
        throw std::invalid_argument("'pose' places exactly one object");
      }
      motion = RigidTransform::fromMatrix(p.at("pose").get<std::vector<double>>())
                   .after(entryPose(objectEntry(context, ids.front())).inverse());
    } else {
      const auto center = p.at("center_mm").get<std::array<double, 3>>();
      const auto translation = p.at("translation_mm").get<std::array<double, 3>>();
      const RigidTransform rotation = RigidTransform::fromAxisAngle(
          p.at("rotation_axis").get<std::array<double, 3>>(), p.at("rotation_deg").get<double>());
      // x' = R (x - c) + c + t
      const auto rotated = rotation.rotate(center);
      motion = rotation;
      for (std::size_t i = 0; i < 3; ++i) {
        motion.translation[i] = center[i] - rotated[i] + translation[i];
      }
    }
    return movedResult(context, ids, motion, Json::object());
  }

 private:
  OperationInfo info_;
};

class AlignPoints final : public Operation {
 public:
  AlignPoints() {
    info_.id = "align_points";
    info_.title = "Align by point pairs";
    info_.description =
        "Moves objects so that points picked on them meet their partners on the target: the "
        "rigid motion with the least squared distances (at least three pairs, not on a line). "
        "Points are in global coordinates (mm) where they lie now. Applies on top of the earlier "
        "moves; the distance left per pair is in the result.";
    info_.outputs = {{"pose", artifact::kPose, "The motion, the new poses and the residuals"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"objects", movedObjectsSchema()},
          {"target", {{"type", "string"}, {"description", "Id of the object aligned to"}}},
          {"pairs",
           {{"type", "array"},
            {"items", {{"type", "object"}}},
            {"minItems", 3},
            {"description",
             "Point pairs {moving: [x, y, z], target: [x, y, z]}, global coordinates in mm"}}}}},
        {"required", {"objects", "pairs"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const auto ids = movedObjects(context);
    if (p.contains("target")) {
      const std::string target = p.at("target").get<std::string>();
      (void)objectEntry(context, target);
      if (std::ranges::count(ids, target) > 0) {
        throw std::invalid_argument("The target must not move with the objects");
      }
    }
    std::vector<std::array<double, 3>> moving;
    std::vector<std::array<double, 3>> target;
    for (const Json& pair : p.at("pairs")) {
      if (!pair.contains("moving") || !pair.contains("target")) {
        throw std::invalid_argument("A point pair needs 'moving' and 'target'");
      }
      moving.push_back(pair.at("moving").get<std::array<double, 3>>());
      target.push_back(pair.at("target").get<std::array<double, 3>>());
    }
    const RigidFit fit = fitRigid(moving, target);
    Json summary = {{"pairs", moving.size()},
                    {"residuals_mm", fit.residuals_mm},
                    {"rms_mm", fit.rms_mm},
                    {"max_residual_mm", std::ranges::max(fit.residuals_mm)}};
    if (p.contains("target")) {
      summary["target"] = p.at("target");
    }
    return movedResult(context, ids, fit.transform, std::move(summary));
  }

 private:
  OperationInfo info_;
};

class AlignSurfaces final : public Operation {
 public:
  AlignSurfaces() {
    info_.id = "align_surfaces";
    info_.title = "Align surfaces (best fit)";
    info_.description =
        "Moves objects so that the surface of the first one fits the surface of the target best: "
        "robust point-to-plane ICP as in the nominal-actual comparison, from where the objects "
        "lie now or, with start=principal_axes, from the best match of their principal axes. "
        "Volumes need a surface step first; only their outer skin is fitted. Applies on top of "
        "the earlier moves.";
    info_.outputs = {{"pose", artifact::kPose, "The motion, the new poses and the fit"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"objects", movedObjectsSchema()},
          {"target", {{"type", "string"}, {"description", "Id of the object aligned to"}}},
          {"start",
           {{"type", "string"},
            {"enum", {"current", "principal_axes"}},
            {"default", "current"},
            {"description",
             "current: refine where the objects lie (after a coarse step such as point pairs); "
             "principal_axes: find the orientation first, for objects far apart"}}},
          {"fit_points", {{"type", "integer"}, {"minimum", 100}, {"default", 20000}}},
          {"max_triangles", {{"type", "integer"}, {"minimum", 1000}, {"default", 2000000}}}}},
        {"required", {"objects", "target"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const auto ids = movedObjects(context);
    const std::string target_id = p.at("target").get<std::string>();
    if (std::ranges::count(ids, target_id) > 0) {
      throw std::invalid_argument("The target must not move with the objects");
    }
    const Json& moving_object = objectEntry(context, ids.front());
    const Json& target_object = objectEntry(context, target_id);
    const auto max_triangles = p.at("max_triangles").get<std::size_t>();
    SurfaceAlignOptions options;
    options.coarse = p.at("start") == "principal_axes";
    options.fit_points = p.at("fit_points").get<std::size_t>();
    IndexedMesh moving;
    IndexedMesh target;
    {
      const TelemetryPhase phase("surfaces");
      moving = transformed(objectSurface(moving_object, max_triangles, true, options.resolution_mm),
                           entryPose(moving_object));
      context.progress(0.2);
      target = transformed(objectSurface(target_object, max_triangles, true, options.resolution_mm),
                           entryPose(target_object));
      context.progress(0.4);
    }
    const SurfaceAlignment aligned = alignSurfaces(moving, target, options);
    context.progress(0.95);
    return movedResult(context, ids, aligned.motion,
                       {{"target", target_id},
                        {"start", p.at("start")},
                        {"fit_rms_mm", aligned.rms_mm},
                        {"fit_inliers", aligned.inliers},
                        {"fit_iterations", aligned.iterations},
                        {"resolution_mm", aligned.resolution_mm}});
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
    info_.inputs = {{"surface", artifact::kSurface, "Surface of the scanned object"}};
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
    options.alignment = p.at("refine").get<bool>() ? CompareOptions::Alignment::kRefine
                                                   : CompareOptions::Alignment::kNone;
    options.tolerance_mm = p.at("tolerance_mm").get<double>();
    options.outer_surface_only = p.at("outer_surface_only").get<bool>();
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
  registry.add(std::make_shared<ImportDicom>());
  registry.add(std::make_shared<ImportVgl>());
  registry.add(std::make_shared<Porosity>());
  registry.add(std::make_shared<SegmentMaterials>());
  registry.add(std::make_shared<SegmentWithModel>());
  registry.add(std::make_shared<Surface>());
  registry.add(std::make_shared<CompareCad>());
  registry.add(std::make_shared<Report>());
  registry.add(std::make_shared<AddMesh>());
  registry.add(std::make_shared<MoveObjects>());
  registry.add(std::make_shared<AlignPoints>());
  registry.add(std::make_shared<AlignSurfaces>());
  registry.add(std::make_shared<CompareObjects>());
}

}  // namespace voxelsieve
