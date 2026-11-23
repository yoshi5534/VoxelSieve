// Built-in operations: opening and importing volumes.
#include "operations/common.hpp"

namespace voxelsieve::operations {
namespace {

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
    // Replaced as the preview gets sharper, while the GUI may read it.
    writeJson(context.output_dir / "preview.json", summary);
    writeFile(context.output_dir / "preview.png",
              std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
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

/// Parameters of the sieve shared by every import.
void addSieveParameters(Json& properties) { properties.update(sieveParameters()); }

DatasetOptions importSieveOptions(const OperationContext& context) {
  DatasetOptions options = sieveOptions(context.params);
  options.progress = datasetProgress(context);
  options.preview = importPreview(context);
  // The directory is the step's: what it holds of another input or other options is replaced.
  options.replace_other_import = true;
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
                   importSieveOptions(context));
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
    info_.resumable = true;
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
             "Header size; default: file size minus voxel data (0 for gzip-compressed files)"}}}}},
        {"required", {"path"}}};
    addVoxelSizeParameters(info_.parameters["properties"], "Default: from the sidecar");
    addSieveParameters(info_.parameters["properties"]);
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
    const DatasetInfo info =
        writeDataset(source, context.output_dir / "dataset.vsieve", importSieveOptions(context));
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
    info_.resumable = true;
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
             "labels or masks"}}}}},
        {"required", {"path"}}};
    addVoxelSizeParameters(info_.parameters["properties"], "Default: from the files, else 1 mm");
    addSieveParameters(info_.parameters["properties"]);
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
    const DatasetInfo info =
        writeDataset(sieved, context.output_dir / "dataset.vsieve", importSieveOptions(context));
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

class ImportDicom final : public Operation {
 public:
  ImportDicom() {
    info_.id = "import_dicom";
    info_.resumable = true;
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
    info_.resumable = true;
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
        // The project file names them: they are read with the rights of the caller.
        context.check_path(*file.path);
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

}  // namespace

void registerImportOperations(OperationRegistry& registry) {
  registry.add(std::make_shared<OpenDataset>());
  registry.add(std::make_shared<ImportRaw>());
  registry.add(std::make_shared<ImportTiff>());
  registry.add(std::make_shared<ImportDicom>());
  registry.add(std::make_shared<ImportGzippedXml>());
}

}  // namespace voxelsieve::operations
