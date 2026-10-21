// Operations that ship with VoxelSieve. Each wraps a library function; the command-line tools
// use the same functions.

#include <openvdb/io/File.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "voxelsieve/battery.hpp"
#include "voxelsieve/compare.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/denoise.hpp"
#include "voxelsieve/dicom.hpp"
#include "voxelsieve/diconde.hpp"
#include "voxelsieve/dimensional.hpp"
#include "voxelsieve/gzipped_xml.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/isolate.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/measure.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/model.hpp"
#include "voxelsieve/operation.hpp"
#include "voxelsieve/orientation.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/preview.hpp"
#include "voxelsieve/project.hpp"
#include "voxelsieve/report.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/telemetry.hpp"
#include "voxelsieve/tiff.hpp"

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

/// Shows the preview of an import as soon as it is read and each sharper one after it (ADR 0020),
/// and keeps the last with the step as preview.json and preview.png. The time counts from the
/// start of the operation, as the user waits.
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
         << " s: " << preview.slices_read << " of " << preview.source_dims[2] << " slices ("
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
  properties["interpolate_slices"] = {
      {"type", "integer"},
      {"minimum", 1},
      {"maximum", 16},
      {"default", 1},
      {"description",
       "Slices per slice spacing, the ones between interpolated linearly: 2 halves the pitch "
       "along z. Fills the gaps between slices thinner than their spacing and makes long voxels "
       "closer to cubes; the interpolated grey values are estimates"}};
}

/// Warnings about the voxel size of an import, also logged: an assumed size, gaps between
/// slices and long voxels, with what interpolate_slices would do about them.
Json voxelSizeWarnings(const VoxelSize& scanned, int factor, bool assumed,
                       const OperationContext& context) {
  Json warnings = Json::array();
  const auto mm = [](double value) {
    std::ostringstream text;
    text << std::setprecision(4) << value << " mm";
    return text.str();
  };
  if (assumed) {
    warnings.push_back(
        "The files give no voxel size, so 1 mm was assumed: lengths, areas and volumes are wrong "
        "until the import is repeated with voxel_size_mm.");
  }
  const double spacing = scanned.pitch_mm[2];
  const double pixel = std::max(scanned.pitch_mm[0], scanned.pitch_mm[1]);
  if (scanned.slice_thickness_mm > 0.0 && scanned.slice_thickness_mm < spacing) {
    const double unscanned = 100.0 * (1.0 - scanned.slice_thickness_mm / spacing);
    std::string text = "The slices are " + mm(scanned.slice_thickness_mm) + " thick but " +
                       mm(spacing) + " apart, so " + std::to_string(std::lround(unscanned)) +
                       " % of the volume along z was not scanned. ";
    if (factor > 1) {
      text += "The gaps were filled by linear interpolation (interpolate_slices " +
              std::to_string(factor) + "); grey values between the slices are estimates.";
    } else {
      const auto suggested = std::max<long>(2, std::lround(std::ceil(spacing / pixel)));
      text +=
          "Lengths and volumes use the spacing; to fill the gaps, import again with "
          "interpolate_slices (" +
          std::to_string(std::min<long>(16, suggested)) +
          " gives about cubic voxels), knowing that the filled values are estimates.";
    }
    warnings.push_back(text);
  } else if (factor == 1 && spacing > 2.0 * pixel) {
    const auto ratio = std::lround(spacing / pixel);
    warnings.push_back("The slices are " + mm(spacing) + " apart, " + std::to_string(ratio) +
                       " times the pixel size: surfaces and small structures look stepped along "
                       "z. interpolate_slices " +
                       std::to_string(std::min<long>(16, ratio)) +
                       " makes the voxels about cubic, with interpolated grey values.");
  }
  for (const Json& warning : warnings) {
    context.log("Warning: " + warning.get<std::string>());
  }
  return warnings;
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
    const InterpolatedSource source(*opened, p.at("interpolate_slices").get<int>());
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
    result.summary["warnings"] = voxelSizeWarnings(
        opened->voxelSize(), p.at("interpolate_slices").get<int>(), false, context);
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
    const TiffStackSource stack(p.at("path").get<std::string>(), tiff);
    const TiffStackSource& source = stack;
    const InterpolatedSource sieved(stack, p.at("interpolate_slices").get<int>());
    if (!source.folder().empty()) {
      context.log("Slices from " + source.folder());
    }
    for (const std::string& other : source.otherFolders()) {
      context.log("Also in the input: " + other + " (choose with 'folder')");
    }
    if (!tiff.voxel_size && !source.fileVoxelSize()) {
      context.log(
          "The files give no voxel size (no OME-XML, centimetre or inch resolution or "
          "key=value description); 1 mm assumed. Set voxel_size_mm.");
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
    const DatasetInfo info = writeDataset(sieved, context.output_dir / "dataset.vsieve", options);
    OperationResult result;
    result.outputs["dataset"] = "dataset.vsieve";
    result.summary = datasetSummary(info);
    result.summary["slices"] = source.dims()[2];
    result.summary["bits_per_sample"] = source.bitsPerSample();
    // Where the voxel size came from, so that a client sees when it is only assumed.
    result.summary["voxel_size_from"] = p.contains("voxel_size_mm") ? "parameters"
                                        : source.fileVoxelSize()    ? "files"
                                                                    : "assumed";
    result.summary["warnings"] =
        voxelSizeWarnings(source.voxelSize(), p.at("interpolate_slices").get<int>(),
                          !p.contains("voxel_size_mm") && !source.fileVoxelSize(), context);
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

/// Parameters of the sieve shared by the imports of DICOM stacks and gzipped XML projects.
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
                           bool assumed_voxel_size, const OperationContext& context) {
  const int factor = context.params.value("interpolate_slices", 1);
  const DatasetInfo info =
      writeDataset(InterpolatedSource(source, factor), context.output_dir / "dataset.vsieve",
                   sieveOptions(context));
  OperationResult result;
  result.outputs["dataset"] = "dataset.vsieve";
  result.summary = datasetSummary(info);
  result.summary["slices"] = source.dims()[2];
  result.summary["warnings"] =
      voxelSizeWarnings(source.voxelSize(), factor, assumed_voxel_size, context);
  result.summary["bits_stored"] = source.bitsStored();
  result.summary["signed"] = source.isSigned();
  placeObject(context, pose, result.summary);
  return result;
}

std::string describeDims(const std::array<std::int64_t, 3>& dims) {
  return std::to_string(dims[0]) + " x " + std::to_string(dims[1]) + " x " +
         std::to_string(dims[2]);
}

/// Whether the files of a DICOM import say anything about the part, as DICONDE files do.
void addDicondeSummary(const DicomStackSource& source, const OperationContext& context,
                       Json& summary) {
  const DicondeInfo info = readDicondeInfo(source);
  if (!info.component.name.empty()) {
    context.log("Component '" + info.component.name + "'" +
                (info.component.id.empty() ? "" : ", " + info.component.id) +
                (info.component.material.empty() ? "" : ", " + info.component.material));
    summary["component"] = info.component.name;
  }
  if (!info.inspection.study_id.empty()) {
    summary["study_id"] = info.inspection.study_id;
  }
  if (!info.indications.empty()) {
    context.log(std::to_string(info.indications.size()) + " indications in the files");
    summary["indications"] = info.indications.size();
  }
  if (info.derived) {
    context.log("The series holds derived images" +
                (info.image_comments.empty() ? std::string() : ": " + info.image_comments));
  }
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
            {"description",
             "Directory of slices, one slice file, or a ZIP archive of slices (read without "
             "extracting it)"}}},
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
    OperationResult result = sieveDicom(source, source.filePose(),
                                        !dicom.voxel_size && !source.fileVoxelSize(), context);
    result.summary["series"] = source.seriesUid();
    addDicondeSummary(source, context, result.summary);
    return result;
  }

 private:
  OperationInfo info_;
};

class ImportGzippedXml final : public Operation {
 public:
  ImportGzippedXml() {
    info_.id = "import_gzipped_xml";
    info_.title = "Import gzipped XML project";
    info_.description =
        "Imports a volume of a gzipped XML project from the files it was imported from, "
        "found where the project names them or next to the project, and places it as in the "
        "project. Removes the outside air and writes a bricked dataset. DICOM stacks are read; "
        "other inputs and further files of the project, such as masks, are reported.";
    info_.outputs = {{"dataset", artifact::kDataset, "Sieved dataset"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"path",
           {{"type", "string"}, {"format", "path"}, {"description", "gzipped XML project"}}},
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
    const GzippedXmlProject project = readGzippedXmlProject(p.at("path").get<std::string>());
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
    for (const GzippedXmlReference& other : project.other_files) {
      context.log("Not imported: " + other.description + ", " +
                  (other.file.path ? other.file.path->filename().string()
                                   : other.file.reference + " (not found)"));
    }
    const GzippedXmlVolume& volume = project.volumes[chosen];
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
    for (const GzippedXmlFile& file : volume.files) {
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
    OperationResult result = sieveDicom(source, volume.pose, false, context);
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
             "no zone of loosened microstructure"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    PorosityOptions options;
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
                      {"porosity", porosity.porosity()},
                      {"per_material", options.materials != nullptr}};
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

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    OrientationOptions options;
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

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    BatteryOptions options = batteryOptions(p);
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
    addDeformationParameters(info_.parameters);
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

std::string readTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot read " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

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

/// The inspection order of a report step: order_path, then order merged over it.
Json readOrder(const Json& p) {
  Json order = Json::object();
  if (p.contains("order_path")) {
    order = Json::parse(readTextFile(p.at("order_path").get<std::string>()));
  }
  if (p.contains("order")) {
    order.merge_patch(p.at("order"));
  }
  return order;
}

class Report final : public Operation {
 public:
  Report() {
    info_.id = "report";
    info_.title = "Test report";
    info_.description =
        "Evaluates the porosity result against the acceptance limits of the inspection order "
        "(BDG P 202 scheme) and writes report.html and report.json. With a surface, the report "
        "shows 3D views of the part and its pores; with a comparison, the nominal-actual "
        "comparison with its views and histogram. For fibre composites and other parts of "
        "several materials, a material segmentation adds the materials with their shares and "
        "coloured slices, and a fibre orientation the fibre families, evaluated against the "
        "\"fibres\" requirements of the order (direction tolerance, share).";
    info_.inputs = {
        {"porosity", artifact::kPorosity, "Porosity result"},
        {"surface", artifact::kSurface, "Surface for the 3D views of the part", true},
        {"comparison", artifact::kComparison, "Nominal-actual comparison", true},
        {"dataset", artifact::kDataset, "Dataset for the slices through the materials", true},
        {"materials", artifact::kMaterials, "Material segmentation", true},
        {"orientation", artifact::kFibreOrientation, "Fibre orientation", true}};
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
    const Json order = readOrder(p);
    const std::string report_template = p.contains("template_path")
                                            ? readTextFile(p.at("template_path").get<std::string>())
                                            : std::string(defaultReportTemplate());
    const auto porosity_dir = context.inputs.at("porosity");
    const PorosityResult porosity = loadPorosityResult(porosity_dir);
    const PorosityOptions options = usedPorosityOptions(porosity_dir);
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
    if (data.contains("evaluation")) {
      result.summary["passed"] = data["evaluation"].value("passed", true);
    }
    return result;
  }

 private:
  OperationInfo info_;
};

/// Colour "#rrggbb" as RGB.
std::array<std::uint8_t, 3> parseColor(const std::string& text) {
  const bool hex = text.size() == 7 && text[0] == '#' &&
                   std::all_of(text.begin() + 1, text.end(),
                               [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
  if (!hex) {
    throw std::invalid_argument("Colour '" + text + "' is not of the form #rrggbb");
  }
  std::array<std::uint8_t, 3> color{};
  for (std::size_t c = 0; c < 3; ++c) {
    color[c] = static_cast<std::uint8_t>(std::stoi(text.substr(1 + 2 * c, 2), nullptr, 16));
  }
  return color;
}

/// Materials of a segmentation for the summary of its step: id, name, colour, volume and, for
/// the threshold segmentation, the grey value each class starts at.
Json materialSummary(const MaterialVolumeInfo& info, bool with_thresholds) {
  Json materials = Json::array();
  for (const Material& material : info.materials) {
    Json entry = {{"id", material.id},
                  {"name", material.name},
                  {"color", material.color},
                  {"voxels", material.voxel_count},
                  {"volume_mm3", material.volume_mm3}};
    if (with_thresholds) {
      entry["from_grey_value"] = material.lower;
    }
    materials.push_back(entry);
  }
  return materials;
}

class SegmentMaterials final : public Operation {
 public:
  SegmentMaterials() {
    info_.id = "segment_materials";
    info_.title = "Material segmentation";
    info_.description =
        "Splits the part into materials by grey value: a voxel is material when it and enough "
        "of its neighbours are above the air threshold (noise spikes go, walls one voxel thin "
        "stay), material grows into touching voxels above a lower threshold, and the classes are "
        "separated by thresholds (unless given: the valleys between the peaks of the histogram "
        "when it has one per class, else multi-level Otsu). With materials 0 the classes "
        "are found in the histogram: one per peak above the air threshold, split in the valleys "
        "between them (the histogram tool shows the suggestion first). Writes a material volume "
        "with volumes per material; the slice view shows the materials in colour.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset to segment"}};
    info_.outputs = {{"materials", artifact::kMaterials, "Material volume"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"materials",
           {{"type", "integer"},
            {"minimum", 0},
            {"maximum", 8},
            {"default", 2},
            {"description", "Number of classes; 0: one per peak of the histogram"}}},
          {"air_threshold",
           {{"type", "number"}, {"description", "Default: the dataset's threshold"}}},
          {"material_thresholds",
           {{"type", "array"},
            {"items", {{"type", "number"}}},
            {"description",
             "Thresholds between the classes (materials - 1, ascending); default: the valleys "
             "between the histogram's peaks when it has one per class, else Otsu"}}},
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
            {"description", "Lower threshold between air level (0) and air threshold (1)"}}},
          {"material_names",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "Names of the classes, in order; default: Material 1, 2, ..."}}},
          {"material_colors",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "Colours of the classes as #rrggbb, in order"}}}}}};
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
    if (p.contains("material_names")) {
      options.names = p.at("material_names").get<std::vector<std::string>>();
    }
    if (p.contains("material_colors")) {
      for (const Json& color : p.at("material_colors")) {
        options.colors.push_back(parseColor(color.get<std::string>()));
      }
    }
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const MaterialVolumeInfo info =
        segmentMaterials(dataset, context.output_dir / "materials", options);
    OperationResult result;
    result.outputs["materials"] = "materials";
    result.summary = {{"air_threshold", info.air_threshold},
                      {"materials", materialSummary(info, true)}};
    if (options.materials == 0) {
      result.summary["materials_from_histogram"] = true;
    }
    return result;
  }

 private:
  OperationInfo info_;
};

class MaterialMeshes final : public Operation {
 public:
  MaterialMeshes() {
    info_.id = "material_meshes";
    info_.title = "Material meshes";
    info_.description =
        "Exports the surface of each material of a material segmentation as a triangle mesh: "
        "one binary STL in mm per material, in the coordinates of the dataset (like the surface "
        "step's surface.stl), for CAD, 3D printing or other viewers. A material's surface is the "
        "boundary between its voxels and all others, so inner materials get closed meshes too. "
        "Large volumes are meshed from every 2nd, 4th, ... voxel until max_triangles holds. "
        "With add_objects (the default, when run as run_material_meshes), each mesh then also "
        "becomes an object of the project in its material's colour and at the pose of the "
        "segmented volume, so view_objects, alignment and comparison can use the materials "
        "one by one.";
    info_.inputs = {{"materials", artifact::kMaterials, "Material segmentation"}};
    info_.outputs = {{"meshes", artifact::kMaterialMeshes, "One STL per material"}};
    info_.parameters = {{"type", "object"},
                        {"properties",
                         {{"material_ids",
                           {{"type", "array"},
                            {"items", {{"type", "integer"}, {"minimum", 1}}},
                            {"description", "Materials to export; default: all"}}},
                          {"max_triangles",
                           {{"type", "integer"},
                            {"minimum", 0},
                            {"default", 2000000},
                            {"description", "Triangles per material at most; 0: full resolution"}}},
                          {"add_objects",
                           {{"type", "boolean"},
                            {"default", true},
                            {"description",
                             "Add each mesh as an object of the project, named and coloured like "
                             "its material"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const MaterialVolume volume = MaterialVolume::open(context.inputs.at("materials"));
    const MaterialVolumeInfo& info = volume.info();
    std::vector<int> ids(info.materials.size());
    std::ranges::transform(info.materials, ids.begin(), &Material::id);
    if (p.contains("material_ids")) {
      ids = p.at("material_ids").get<std::vector<int>>();
      for (const int id : ids) {
        if (std::ranges::none_of(info.materials, [id](const Material& m) { return m.id == id; })) {
          throw std::invalid_argument("The segmentation has no material " + std::to_string(id));
        }
      }
    }
    const auto max_triangles = p.at("max_triangles").get<std::size_t>();
    const std::filesystem::path dir = context.output_dir / "meshes";
    std::filesystem::create_directories(dir);
    Json meshes = Json::array();
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const Material& material =
          *std::ranges::find_if(info.materials, [&](const Material& m) { return m.id == ids[i]; });
      const IndexedMesh voxels = materialMesh(volume, material.id, max_triangles);
      Mesh mesh;
      mesh.triangles.reserve(voxels.triangles.size());
      const auto mm = [&](std::uint32_t index) {
        const auto& q = voxels.points[index];
        return std::array<float, 3>{static_cast<float>(q[0] * info.voxel_size[0]),
                                    static_cast<float>(q[1] * info.voxel_size[1]),
                                    static_cast<float>(q[2] * info.voxel_size[2])};
      };
      for (const auto& t : voxels.triangles) {
        mesh.triangles.push_back({mm(t[0]), mm(t[1]), mm(t[2])});
      }
      std::string name;
      for (const char c : material.name) {
        name += std::isalnum(static_cast<unsigned char>(c)) != 0 ? c : '_';
      }
      const std::string file = "material_" + std::to_string(material.id) + "_" + name + ".stl";
      writeStl(dir / file, mesh);
      std::string color = "#";
      for (const std::uint8_t channel : material.color) {
        constexpr std::string_view kHex = "0123456789abcdef";
        color += kHex[channel >> 4U];
        color += kHex[channel & 15U];
      }
      meshes.push_back({{"id", material.id},
                        {"name", material.name},
                        {"color", color},
                        {"file", file},
                        {"triangles", mesh.triangles.size()}});
      context.progress(static_cast<double>(i + 1) / static_cast<double>(ids.size()));
    }
    OperationResult result;
    result.outputs["meshes"] = "meshes";
    result.summary = {{"meshes", meshes}};
    return result;
  }

 private:
  OperationInfo info_;
};

class MeasureObjects final : public Operation {
 public:
  MeasureObjects() {
    info_.id = "measure_objects";
    info_.title = "Measure objects";
    info_.description =
        "Measures every object of a material segmentation: each connected piece of a material "
        "(voxels sharing faces), such as a separated part, a bone or an organ, with its volume, "
        "surface area, centroid, bounding box and its lengths along its principal axes and along "
        "an axis you give. Material 0 measures enclosed air, the voids that do not reach the "
        "border, such as a swim bladder or a cavity. Writes a table (objects.csv) and "
        "objects.json; coordinates in mm in the volume's own coordinates.";
    info_.inputs = {{"materials", artifact::kMaterials, "Material segmentation"}};
    info_.outputs = {{"measurements", artifact::kMeasurements, "objects.csv and objects.json"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"material_ids",
           {{"type", "array"},
            {"items", {{"type", "integer"}, {"minimum", 0}}},
            {"description", "Materials to measure, 0 for enclosed air; default: every material"}}},
          {"min_voxels",
           {{"type", "integer"},
            {"minimum", 1},
            {"default", 27},
            {"description", "Objects smaller than this many voxels are left out"}}},
          {"max_objects", {{"type", "integer"}, {"minimum", 1}, {"default", 100}}},
          {"axis",
           {{"type", "array"},
            {"items", {{"type", "number"}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description",
             "Direction along the volume's x, y, z to measure each object's length along, such "
             "as [0, 0, 1]"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    MeasureOptions options;
    if (p.contains("material_ids")) {
      options.material_ids = p.at("material_ids").get<std::vector<int>>();
    }
    options.min_voxels = p.at("min_voxels").get<std::int64_t>();
    options.max_objects = p.at("max_objects").get<int>();
    if (p.contains("axis")) {
      options.axis = p.at("axis").get<std::array<double, 3>>();
    }
    const MaterialVolume volume = MaterialVolume::open(context.inputs.at("materials"));
    const MeasureInfo info = measureObjects(volume, options);
    context.progress(0.9);
    const std::filesystem::path dir = context.output_dir / "measurements";
    std::filesystem::create_directories(dir);
    writeObjectsCsv(dir / "objects.csv", info);
    Json objects = Json::array();
    for (std::size_t k = 0; k < info.objects.size(); ++k) {
      const MeasuredObject& object = info.objects[k];
      Json json = {{"object", k + 1},
                   {"material", object.material},
                   {"material_name", object.material_name},
                   {"voxels", object.voxels},
                   {"volume_mm3", object.volume_mm3},
                   {"surface_mm2", object.surface_mm2},
                   {"centroid_mm", object.centroid_mm},
                   {"min_mm", object.min_mm},
                   {"max_mm", object.max_mm},
                   {"principal_axes", object.principal_axes},
                   {"principal_lengths_mm", object.principal_lengths_mm},
                   {"touches_border", object.touches_border}};
      if (object.axis_length_mm) {
        json["axis_length_mm"] = *object.axis_length_mm;
      }
      objects.push_back(std::move(json));
    }
    std::ofstream(dir / "objects.json") << Json{{"stride", info.stride},
                                                {"objects_found", info.objects_found},
                                                {"objects", objects}}
                                               .dump(2)
                                        << '\n';
    OperationResult result;
    result.outputs["measurements"] = "measurements";
    // The summary keeps the largest objects; the files hold all.
    constexpr std::size_t kSummaryObjects = 20;
    Json shown = Json::array();
    for (std::size_t k = 0; k < std::min(kSummaryObjects, objects.size()); ++k) {
      Json brief = objects[k];
      brief.erase("principal_axes");
      brief.erase("min_mm");
      brief.erase("max_mm");
      shown.push_back(std::move(brief));
    }
    result.summary = {{"objects_found", info.objects_found},
                      {"objects_measured", info.objects.size()},
                      {"stride", info.stride},
                      {"objects", shown},
                      {"table", "measurements/objects.csv"}};
    if (info.stride > 1) {
      context.log("Measured on every " + std::to_string(info.stride) +
                  "th voxel per axis: the volume is large");
    }
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
    result.summary = {{"model", info.model}, {"materials", materialSummary(info, false)}};
    return result;
  }

 private:
  OperationInfo info_;
};

class Denoise final : public Operation {
 public:
  Denoise() {
    info_.id = "denoise";
    info_.title = "Denoise";
    info_.description =
        "Reduces the noise of a dataset and writes a new dataset with the same part; the input "
        "stays in the project. Non-local means averages voxels whose neighbourhoods look alike, "
        "measured against the noise of the scan including the correlation of neighbouring "
        "voxels, and leaves out neighbourhoods that differ more than noise explains, so edges, "
        "thin walls and pores of a few voxels keep their contrast. A learned denoising model "
        "(.vsm, trained on the noisy scan itself with tools/models/train_denoiser.py) can run "
        "instead. Later steps of the object use the denoised dataset.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset to denoise"}};
    info_.outputs = {{"dataset", artifact::kDataset, "Denoised dataset"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"method",
           {{"type", "string"},
            {"enum", {"non_local_means", "model"}},
            {"default", "non_local_means"}}},
          {"strength",
           {{"type", "number"},
            {"exclusiveMinimum", 0},
            {"default", 1.0},
            {"description", "Non-local means: larger values smooth more"}}},
          {"similarity_limit",
           {{"type", "number"},
            {"exclusiveMinimum", 0},
            {"default", 1.3},
            {"description",
             "Non-local means: neighbourhoods that differ by more than this multiple of the "
             "noise get no weight; larger values smooth small details more"}}},
          {"search_radius",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 7},
            {"default", 3},
            {"description", "Non-local means: half edge of the search window in voxels"}}},
          {"patch_radius",
           {{"type", "integer"},
            {"minimum", 0},
            {"maximum", 3},
            {"default", 1},
            {"description", "Non-local means: half edge of the compared patches in voxels"}}},
          {"noise_sigma",
           {{"type", "number"},
            {"minimum", 0},
            {"description",
             "Non-local means: noise in grey values, taken as uncorrelated; default: measured "
             "from the dataset, correlation included"}}},
          {"model_path",
           {{"type", "string"},
            {"format", "path"},
            {"description", "Method model: the learned denoising model (.vsm)"}}},
          {"tile",
           {{"type", "integer"},
            {"minimum", 0},
            {"default", 0},
            {"description", "Tile edge in voxels; 0: 64 for non-local means, 128 for a model"}}},
          {"value_type", valueTypeParameter()}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    DenoiseOptions options;
    std::optional<Model> model;
    if (p.at("method").get<std::string>() == "model") {
      if (!p.contains("model_path")) {
        throw std::invalid_argument("Denoising with a model needs model_path");
      }
      model.emplace(Model::load(std::filesystem::absolute(p.at("model_path").get<std::string>())));
      options.method = DenoiseMethod::kModel;
      options.model = &*model;
    }
    options.non_local_means.strength = p.at("strength").get<double>();
    options.non_local_means.similarity_limit = p.at("similarity_limit").get<double>();
    options.non_local_means.search_radius = p.at("search_radius").get<int>();
    options.non_local_means.patch_radius = p.at("patch_radius").get<int>();
    if (p.contains("noise_sigma")) {
      options.noise_sigma = p.at("noise_sigma").get<double>();
    }
    options.tile = p.at("tile").get<std::int64_t>();
    options.output.value_type = valueTypeOption(p);
    options.output.progress = [&context](std::string_view stage, double fraction) {
      context.progress(stage == "bricks" ? 0.95 * fraction : 0.95 + 0.05 * fraction);
    };
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const DenoiseInfo info = denoiseDataset(dataset, context.output_dir / "dataset", options);
    const NoiseModel& noise = info.noise;
    const double after = estimateNoise(Dataset::open(context.output_dir / "dataset"), 1).sigma;
    std::ostringstream text;
    text << std::fixed << std::setprecision(0) << "Noise " << noise.sigma << " -> " << after
         << " grey values (correlation of neighbours " << std::setprecision(2)
         << noise.correlation(1, 0, 0) << ", " << noise.correlation(0, 1, 0) << ", "
         << noise.correlation(0, 0, 1) << ")";
    context.log(text.str());
    OperationResult result;
    result.outputs["dataset"] = "dataset";
    result.summary = datasetSummary(info.dataset);
    result.summary.update(
        {{"method", std::string(denoiseMethodName(info.method))},
         {"noise_sigma", noise.sigma},
         {"noise_sigma_after", after},
         {"noise_correlation",
          {noise.correlation(1, 0, 0), noise.correlation(0, 1, 0), noise.correlation(0, 0, 1)}}});
    if (!info.model.empty()) {
      result.summary["model"] = info.model;
    }
    return result;
  }

 private:
  OperationInfo info_;
};

class IsolatePart final : public Operation {
 public:
  IsolatePart() {
    info_.id = "isolate_part";
    info_.title = "Isolate part";
    info_.description =
        "Removes what is not the part from a dataset and writes a new dataset; the input stays "
        "in the project. Voxels outside a region (a box in voxels, a crop) and objects other "
        "than the largest ones, such as a holder, a support or loose debris that the scan also "
        "shows, become outside air, so material segmentation, porosity, surfaces and volumes "
        "see the part alone. An object is material connected across faces, with its voids. "
        "Objects that touch are one object unless separate_voxels splits them at a thin neck; "
        "cut planes and a cylinder cut them apart where they touch broadly. Later steps of the "
        "object use the new dataset.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset to isolate the part in"}};
    info_.outputs = {{"dataset", artifact::kDataset, "Dataset with the part alone"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"region_min",
           {{"type", "array"},
            {"items", {{"type", "integer"}, {"minimum", 0}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description", "First voxel (x, y, z) of the region to keep; default: 0, 0, 0"}}},
          {"region_max",
           {{"type", "array"},
            {"items", {{"type", "integer"}, {"minimum", 1}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description",
             "End of the region (x, y, z), exclusive; default: the dimensions of the dataset"}}},
          {"keep_objects",
           {{"type", "integer"},
            {"minimum", 0},
            {"default", 1},
            {"description", "Keep the largest objects, 1: only the largest; 0: all objects"}}},
          {"separate_voxels",
           {{"type", "integer"},
            {"minimum", 0},
            {"default", 0},
            {"description",
             "Separates touching objects, such as a holder glued to the part, where they meet "
             "through a neck thinner than about twice this many voxels (erosion, then growing "
             "back within the material); 0: only air separates objects"}}},
          {"cut_planes",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"point",
                 {{"type", "array"},
                  {"items", {{"type", "number"}}},
                  {"minItems", 3},
                  {"maxItems", 3},
                  {"description", "A point of the plane, in voxels (x, y, z)"}}},
                {"normal",
                 {{"type", "array"},
                  {"items", {{"type", "number"}}},
                  {"minItems", 3},
                  {"maxItems", 3},
                  {"description", "Points to the side that is removed"}}}}},
              {"required", {"point", "normal"}}}},
            {"description",
             "Planes that remove what lies on their normal's side, such as everything below a "
             "holder: point [0, 0, 120] with normal [0, 0, -1] removes all below z = 120. "
             "Several planes approximate a curved cut"}}},
          {"cylinder",
           {{"type", "object"},
            {"properties",
             {{"axis", {{"type", "string"}, {"enum", {"x", "y", "z"}}, {"default", "z"}}},
              {"center",
               {{"type", "array"},
                {"items", {{"type", "number"}}},
                {"minItems", 2},
                {"maxItems", 2},
                {"description", "Centre in the two other axes, in voxels (x, y for axis z)"}}},
              {"radius", {{"type", "number"}, {"exclusiveMinimum", 0}}}}},
            {"required", {"center", "radius"}},
            {"description", "Keep only the inside of this cylinder, such as a part in a tube"}}},
          {"threshold",
           {{"type", "number"},
            {"description",
             "Grey value above which a voxel is material; default: the dataset's"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    IsolateOptions options;
    if (p.contains("region_min") || p.contains("region_max")) {
      Box region{{0, 0, 0}, dataset.info().dims};
      if (p.contains("region_min")) {
        region.min = p.at("region_min").get<std::array<std::int64_t, 3>>();
      }
      if (p.contains("region_max")) {
        region.max = p.at("region_max").get<std::array<std::int64_t, 3>>();
      }
      options.region = region;
    }
    options.keep_objects = p.at("keep_objects").get<int>();
    options.separate_voxels = p.at("separate_voxels").get<int>();
    for (const Json& plane : p.value("cut_planes", Json::array())) {
      options.cut_planes.push_back({plane.at("point").get<std::array<double, 3>>(),
                                    plane.at("normal").get<std::array<double, 3>>()});
    }
    if (p.contains("cylinder")) {
      const Json& cylinder = p.at("cylinder");
      options.cylinder = RegionCylinder{cylinder.value("axis", std::string("z"))[0] - 'x',
                                        cylinder.at("center").get<std::array<double, 2>>(),
                                        cylinder.at("radius").get<double>()};
    }
    if (p.contains("threshold")) {
      options.threshold = p.at("threshold").get<float>();
    }
    options.progress = [&context](std::string_view stage, double fraction) {
      context.progress(stage == "objects"  ? 0.3 * fraction
                       : stage == "bricks" ? 0.3 + 0.65 * fraction
                                           : 0.95 + 0.05 * fraction);
    };
    const IsolateInfo info = isolatePart(dataset, context.output_dir / "dataset", options);
    std::ostringstream text;
    text << info.objects << " objects found; kept " << info.kept_material_voxels.size()
         << ", removed " << info.removed_voxels << " voxels";
    context.log(text.str());
    OperationResult result;
    result.outputs["dataset"] = "dataset";
    result.summary = datasetSummary(info.dataset);
    result.summary.update({{"objects_found", info.objects},
                           {"kept_material_voxels", info.kept_material_voxels},
                           {"removed_voxels", info.removed_voxels},
                           {"cell_size", info.cell_size}});
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
         {{"path", {{"type", "string"}, {"format", "path"}, {"description", "STL file in mm"}}},
          {"color",
           {{"type", "string"},
            {"pattern", "^#[0-9a-fA-F]{6}$"},
            {"description",
             "Colour of the object in views as #rrggbb; default: by its place in the list"}}},
          {"pose",
           {{"type", "array"},
            {"items", {{"type", "number"}}},
            {"minItems", 12},
            {"maxItems", 16},
            {"description",
             "Pose of the new object, object to global coordinates, as a row-major 3x4 or 4x4 "
             "matrix in mm; default: the coordinates of the file"}}}}},
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
    if (context.params.contains("pose")) {
      // The object starts at this pose instead of the identity.
      const RigidTransform pose =
          RigidTransform::fromMatrix(context.params.at("pose").get<std::vector<double>>());
      result.summary[kMovedObjectsKey] = Json::array({context.object});
      result.summary[kMotionKey] = pose.matrix();
    }
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
        "`pose` places objects at a given pose. Moves apply on top of the earlier ones, in "
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
             "New pose of the objects (which must share one pose, such as a single object), "
             "object to global coordinates, as a row-major 3x4 or 4x4 matrix in mm; replaces "
             "the rotation and translation"}}}}},
        {"required", {"objects"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const auto ids = movedObjects(context);
    RigidTransform motion;
    if (p.contains("pose")) {
      const auto current = entryPose(objectEntry(context, ids.front())).matrix();
      for (const std::string& id : ids) {
        if (entryPose(objectEntry(context, id)).matrix() != current) {
          throw std::invalid_argument("'pose' places objects that share one pose");
        }
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

/// The nominal geometry of a comparison in the coordinates its compare.json places in the scan
/// with cad_to_scan: the CAD file it was made with or the nominal object.
Mesh comparisonNominal(const OperationContext& context, const Json& compared,
                       std::filesystem::path& cad_path) {
  if (!cad_path.empty()) {
    return readStl(cad_path);
  }
  if (compared.contains("cad_path")) {
    cad_path = compared.at("cad_path").get<std::string>();
    return readStl(cad_path);
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
    info_.inputs = {{"surface", artifact::kSurface, "Surface of the scan"},
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
    const Mesh cad = comparisonNominal(context, compared, cad_path);
    const RigidTransform cad_to_scan =
        RigidTransform::fromMatrix(compared.at("cad_to_scan").get<std::vector<double>>());
    DimensionalOptions options;
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
      const Evaluation evaluation = evaluate(result, inspectionZonesFromJson(order));
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
    std::ofstream(dir / "report.html", std::ios::binary) << renderTemplate(report_template, data);
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
    std::ofstream(dir / "report.html", std::ios::binary) << renderTemplate(report_template, data);
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
      if (const auto zones = inspectionZonesFromJson(order); !zones.empty()) {
        evaluation = evaluate(result, zones);
      }
    }
    applyInspectionOrder(order, options, evaluation ? &*evaluation : nullptr);

    const auto dir = context.output_dir / "diconde";
    const auto dataset = Dataset::open(context.inputs.at("dataset"), Dataset::kDefaultCacheBytes,
                                       BrickLoading::kOnAccess);
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

void registerBuiltinOperations(OperationRegistry& registry) {
  registry.add(std::make_shared<OpenDataset>());
  registry.add(std::make_shared<ImportRaw>());
  registry.add(std::make_shared<ImportTiff>());
  registry.add(std::make_shared<ImportDicom>());
  registry.add(std::make_shared<ImportGzippedXml>());
  registry.add(std::make_shared<Porosity>());
  registry.add(std::make_shared<FibreOrientation>());
  registry.add(std::make_shared<BatteryInspection>());
  registry.add(std::make_shared<BatteryImages>());
  registry.add(std::make_shared<SegmentMaterials>());
  registry.add(std::make_shared<SegmentWithModel>());
  registry.add(std::make_shared<MaterialMeshes>());
  registry.add(std::make_shared<MeasureObjects>());
  registry.add(std::make_shared<Denoise>());
  registry.add(std::make_shared<IsolatePart>());
  registry.add(std::make_shared<Surface>());
  registry.add(std::make_shared<CompareCad>());
  registry.add(std::make_shared<Report>());
  registry.add(std::make_shared<AddMesh>());
  registry.add(std::make_shared<MoveObjects>());
  registry.add(std::make_shared<AlignPoints>());
  registry.add(std::make_shared<AlignSurfaces>());
  registry.add(std::make_shared<CompareObjects>());
  registry.add(std::make_shared<MeasureDimensions>());
  registry.add(std::make_shared<FirstArticleReport>());
  registry.add(std::make_shared<BatteryReport>());
  registry.add(std::make_shared<ExportDiconde>());
}

}  // namespace voxelsieve
