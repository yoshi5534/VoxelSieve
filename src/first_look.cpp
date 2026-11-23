// The first look at a scan (ADR 0039): what a file is, how to open it, and which analyses suit it.

#include "voxelsieve/first_look.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>

#include "voxelsieve/beam_hardening.hpp"
#include "voxelsieve/dicom.hpp"
#include "voxelsieve/gzipped_xml.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/tiff.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

std::string lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) {
    return static_cast<char>(c < 0x80 ? std::tolower(c) : c);
  });
  return text;
}

std::string lowerExtension(const std::filesystem::path& path) {
  return lower(path.extension().string());
}

/// The name of a scan without extensions such as .raw.gz.
std::string scanName(const std::filesystem::path& path) {
  std::filesystem::path name = path.filename();
  if (name.empty()) {
    name = path.parent_path().filename();
  }
  for (int i = 0; i < 2; ++i) {
    const std::string extension = name.extension().string();
    if (extension.size() > 1 && extension.size() <= 6 &&
        std::isdigit(static_cast<unsigned char>(extension[1])) == 0) {
      name = name.stem();
    }
  }
  return name.string();
}

std::string sampleTypeName(SampleType type) {
  return type == SampleType::kUInt8 ? "uint8" : "uint16";
}

Json voxelSizeJson(const VoxelSize& size) {
  Json json;
  writeVoxelSize(json, size);
  return json.at("voxel_size_mm");
}

std::string formatMm(double value) {
  std::ostringstream text;
  text << std::setprecision(4) << value << " mm";
  return text.str();
}

}  // namespace

std::vector<std::filesystem::path> cadModelsNextTo(const std::filesystem::path& path) {
  std::vector<std::filesystem::path> meshes;
  std::error_code error;
  const std::filesystem::path dir = path.parent_path();
  constexpr int kLookedAt = 256;
  int looked_at = 0;
  for (auto it = std::filesystem::directory_iterator(
           dir, std::filesystem::directory_options::skip_permission_denied, error);
       !error && it != std::filesystem::directory_iterator() && looked_at < kLookedAt;
       it.increment(error), ++looked_at) {
    if (lowerExtension(it->path()) == ".stl" && it->is_regular_file(error)) {
      meshes.push_back(it->path());
    }
  }
  std::ranges::sort(meshes);
  // The one named like the scan first.
  const std::string name = lower(scanName(path));
  std::ranges::stable_partition(meshes, [&](const std::filesystem::path& mesh) {
    return lower(mesh.stem().string()) == name;
  });
  constexpr std::size_t kMaxMeshes = 8;
  if (meshes.size() > kMaxMeshes) {
    meshes.resize(kMaxMeshes);
  }
  return meshes;
}

namespace {

Json fromRaw(const std::filesystem::path& path, Json& result) {
  Json notes = Json::array();
  try {
    const RawInput input = resolveRawInput(path, Json::object());
    const RawLayout& layout = input.layout;
    result["operation"] = "import_raw";
    Json params = {{"path", path.string()},
                   {"dims", layout.dims},
                   {"sample_type", sampleTypeName(layout.sample_type)},
                   {"big_endian", layout.byte_order == std::endian::big}};
    if (layout.header_bytes) {
      params["header_bytes"] = *layout.header_bytes;
    }
    if (input.voxel_size_known) {
      params["voxel_size_mm"] = voxelSizeJson(layout.voxel_size);
    }
    result["params"] = params;
    result["found"] = {{"dims", layout.dims},
                       {"sample_type", sampleTypeName(layout.sample_type)},
                       {"byte_order", layout.byte_order == std::endian::big ? "big" : "little"},
                       {"header_bytes", layout.header_bytes.value_or(0)}};
    if (input.voxel_size_known) {
      result["found"]["voxel_size_mm"] = voxelSizeJson(layout.voxel_size);
    }
    result["from"] = input.from;
    const std::string dims_from = input.from.at("dims");
    if (dims_from == "grey values") {
      std::ostringstream text;
      text << "Dimensions, sample type and header were read from the grey values ("
           << std::lround(100.0 * input.confidence)
           << " % clear); check the slice preview while the import runs.";
      notes.push_back(text.str());
      result["check"] = true;
    } else if (dims_from == "file name") {
      notes.push_back("Dimensions from the file name.");
    }
    if (!input.voxel_size_known) {
      notes.push_back(
          "Nothing gives the voxel size: 1 mm is assumed. Enter it if you know it, so that "
          "lengths and volumes are right.");
      result["check"] = true;
    }
  } catch (const std::invalid_argument& error) {
    result["operation"] = "import_raw";
    result["params"] = {{"path", path.string()}};
    notes.push_back(std::string(error.what()) + ". Enter the dimensions (x, y, z in voxels).");
    result["check"] = true;
    result["ready"] = false;
  }
  return notes;
}

Json fromTiff(const std::filesystem::path& path, Json& result) {
  Json notes = Json::array();
  result["operation"] = "import_tiff";
  result["params"] = {{"path", path.string()}};
  const TiffStackSource stack(path);
  result["found"] = {{"dims", stack.dims()},
                     {"bits", stack.bitsPerSample()},
                     {"float", stack.isFloat()},
                     {"slices", stack.dims()[2]}};
  if (!stack.folder().empty()) {
    result["found"]["folder"] = stack.folder();
  }
  if (const auto size = stack.fileVoxelSize()) {
    result["found"]["voxel_size_mm"] = voxelSizeJson(*size);
    result["from"] = {{"dims", "files"}, {"voxel_size", "files"}};
  } else {
    result["from"] = {{"dims", "files"}, {"voxel_size", "assumed"}};
    notes.push_back(
        "The files give no voxel size: 1 mm is assumed. Enter it if you know it, so that "
        "lengths and volumes are right.");
    result["check"] = true;
  }
  if (stack.isFloat()) {
    notes.push_back(
        "Float slices: the values are mapped onto 16-bit grey values, and the "
        "mapping is kept with the dataset.");
  }
  return notes;
}

Json fromDicom(const std::filesystem::path& path, Json& result) {
  Json notes = Json::array();
  result["operation"] = "import_dicom";
  result["params"] = {{"path", path.string()}};
  const DicomStackSource stack(path);
  result["found"] = {{"dims", stack.dims()},
                     {"voxel_size_mm", voxelSizeJson(stack.voxelSize())},
                     {"slices", stack.dims()[2]}};
  result["from"] = {{"dims", "files"}, {"voxel_size", "files"}};
  return notes;
}

}  // namespace

RawInput resolveRawInput(const std::filesystem::path& path, const Json& params) {
  RawInput input;
  Json sidecar;
  if (const auto file = rawSidecarPath(path); std::filesystem::exists(file)) {
    std::ifstream in(file);
    sidecar = Json::parse(in);
  }
  // The file name and grey values only when the parameters and the sidecar lack the dimensions.
  std::optional<RawLayoutGuess> guess;
  if (!params.contains("dims") && !sidecar.contains("dims")) {
    guess = detectRawLayout(path);
    if (!guess) {
      throw std::invalid_argument("No dims given, no sidecar next to " + path.string() +
                                  ", and neither its name nor its grey values show the dimensions");
    }
    input.confidence = guess->confidence;
  }
  RawLayout& layout = input.layout;
  if (params.contains("dims")) {
    layout.dims = params.at("dims").get<std::array<std::int64_t, 3>>();
    input.from["dims"] = "parameters";
  } else if (sidecar.contains("dims")) {
    layout.dims = sidecar.at("dims").get<std::array<std::int64_t, 3>>();
    input.from["dims"] = "sidecar";
  } else if (guess) {
    layout.dims = guess->layout.dims;
    input.from["dims"] = guess->dims_from;
  }
  const Json format =
      sidecar.is_object() ? sidecar.value("format", Json::object()) : Json::object();
  if (params.contains("sample_type")) {
    layout.sample_type =
        params.at("sample_type") == "uint8" ? SampleType::kUInt8 : SampleType::kUInt16;
    input.from["sample_type"] = "parameters";
  } else if (format.contains("dtype")) {
    const std::string dtype = format.at("dtype").get<std::string>();
    if (dtype != "uint8" && dtype != "uint16") {
      throw std::invalid_argument("The sidecar gives the sample type '" + dtype +
                                  "'; raw files of uint8 or uint16 are read");
    }
    layout.sample_type = dtype == "uint8" ? SampleType::kUInt8 : SampleType::kUInt16;
    input.from["sample_type"] = "sidecar";
  } else if (guess) {
    layout.sample_type = guess->layout.sample_type;
    input.from["sample_type"] = guess->dims_from;
  } else {
    input.from["sample_type"] = "assumed";
  }
  if (params.contains("big_endian")) {
    layout.byte_order =
        params.at("big_endian").get<bool>() ? std::endian::big : std::endian::little;
  } else if (format.contains("endianness")) {
    layout.byte_order = format.at("endianness") == "big" ? std::endian::big : std::endian::little;
  } else if (guess) {
    layout.byte_order = guess->layout.byte_order;
  }
  if (params.contains("header_bytes")) {
    layout.header_bytes = params.at("header_bytes").get<std::uint64_t>();
  } else if (guess && guess->dims_from == "grey values") {
    layout.header_bytes = guess->layout.header_bytes;
  }
  if (params.contains("voxel_size_mm")) {
    layout.voxel_size = voxelSizeFromJson(params.at("voxel_size_mm"));
    input.voxel_size_known = true;
    input.from["voxel_size"] = "parameters";
  } else if (sidecar.contains("voxel_size_mm")) {
    layout.voxel_size = readVoxelSize(sidecar);
    input.voxel_size_known = true;
    input.from["voxel_size"] = "sidecar";
  } else {
    std::optional<VoxelSize> named;
    if (guess && guess->voxel_size_known) {
      named = guess->layout.voxel_size;
    } else if (!guess) {
      // Dimensions from the sidecar or the parameters: the name may still give the voxel size.
      named = voxelSizeFromName(path);
    }
    if (named) {
      layout.voxel_size = *named;
      input.voxel_size_known = true;
      input.from["voxel_size"] = "file name";
    } else {
      layout.voxel_size = VoxelSize(1.0);
      input.from["voxel_size"] = "assumed";
    }
  }
  checkVolumeDims(layout.dims, path.filename().string());
  return input;
}

std::filesystem::path projectDirFor(const std::filesystem::path& path) {
  const std::filesystem::path absolute = std::filesystem::absolute(path);
  const std::string name = scanName(absolute);
  const std::filesystem::path dir = absolute.parent_path();
  for (int n = 1;; ++n) {
    const std::filesystem::path candidate =
        dir / (name + (n == 1 ? "" : "-" + std::to_string(n)) + ".vsproj");
    std::error_code error;
    if (!std::filesystem::exists(candidate, error)) {
      return candidate;
    }
    // A project that imported this scan before is opened again.
    if (std::filesystem::exists(candidate / "project.json", error)) {
      try {
        std::ifstream in(candidate / "project.json");
        const Json project = Json::parse(in);
        const Json steps = project.value("steps", Json::array());
        for (const Json& step : steps) {
          const Json step_params = step.value("params", Json::object());
          if (step_params.contains("path") && step_params.at("path").is_string() &&
              std::filesystem::path(step_params.at("path").get<std::string>()) == absolute) {
            return candidate;
          }
        }
      } catch (const std::exception&) {
        continue;  // not readable: a new project beside it
      }
    }
  }
}

Json inspectScan(const std::filesystem::path& requested) {
  const std::filesystem::path path = std::filesystem::absolute(requested);
  if (!std::filesystem::exists(path)) {
    throw std::invalid_argument("No such file or directory: " + path.string());
  }
  Json result = {{"path", path.string()}, {"name", scanName(path)},  {"check", false},
                 {"ready", true},         {"found", Json::object()}, {"from", Json::object()}};
  Json notes = Json::array();
  const bool directory = std::filesystem::is_directory(path);
  if (!directory) {
    result["found"]["size_bytes"] = std::filesystem::file_size(path);
  }
  const std::string extension = lowerExtension(path);
  try {
    if (directory && std::filesystem::exists(path / "project.json")) {
      result["kind"] = "project";
      result["operation"] = nullptr;
    } else if (directory && std::filesystem::exists(path / "index.json")) {
      result["kind"] = "dataset";
      result["operation"] = "open_dataset";
      result["params"] = {{"path", path.string()}};
      const DatasetInfo info = readDatasetInfo(path);
      result["found"] = {{"dims", info.dims}, {"voxel_size_mm", voxelSizeJson(info.voxel_size)}};
      result["from"] = {{"dims", "dataset"}, {"voxel_size", "dataset"}};
    } else if (!directory && extension == ".stl") {
      result["kind"] = "mesh";
      result["operation"] = "add_mesh";
      result["params"] = {{"path", path.string()}};
    } else if (isDicomArchive(path) || (directory ? containsDicom(path) : isDicomFile(path))) {
      result["kind"] = "dicom";
      notes = fromDicom(path, result);
    } else if (directory ? containsTiff(path)
                         : extension == ".tif" || extension == ".tiff" || extension == ".zip") {
      result["kind"] = "tiff";
      notes = fromTiff(path, result);
    } else if (!directory && isGzippedXmlFile(path)) {
      result["kind"] = "gzipped_xml";
      result["operation"] = "import_gzipped_xml";
      result["params"] = {{"path", path.string()}};
      notes.push_back("The volume is read from the files the project names, and placed as there.");
    } else if (!directory) {
      result["kind"] = "raw";
      notes = fromRaw(path, result);
    } else {
      result["kind"] = "unknown";
      result["operation"] = nullptr;
      result["ready"] = false;
      notes.push_back("This folder holds no TIFF or DICOM slices, dataset or project.");
    }
  } catch (const std::exception& error) {
    if (!result.contains("kind")) {
      result["kind"] = "unknown";
    }
    if (!result.contains("operation")) {
      result["operation"] = nullptr;
    }
    result["ready"] = false;
    notes.push_back(std::string("Cannot read it: ") + error.what());
  }
  if (result.at("kind") != "project" && result.at("kind") != "mesh") {
    result["project_dir"] = projectDirFor(path).string();
    Json meshes = Json::array();
    if (!directory) {
      for (const auto& mesh : cadModelsNextTo(path)) {
        meshes.push_back(mesh.string());
      }
    }
    result["meshes"] = meshes;
    if (!meshes.empty()) {
      notes.push_back("A CAD model lies next to the scan (" +
                      std::filesystem::path(meshes.front().get<std::string>()).filename().string() +
                      "): it can be compared with the scan.");
    }
  }
  result["notes"] = notes;
  return result;
}

namespace {

/// Whether any of `words` appears in `name` as a word or at the start of one.
bool mentions(const std::string& name, std::initializer_list<const char*> words) {
  const std::string text = lower(name);
  for (const char* word : words) {
    const std::string w = word;
    for (std::size_t at = text.find(w); at != std::string::npos; at = text.find(w, at + 1)) {
      const bool starts = at == 0 || std::isalnum(static_cast<unsigned char>(text[at - 1])) == 0;
      if (starts) {
        return true;
      }
    }
  }
  return false;
}

/// The finest level with at most `max_voxels`, else the coarsest.
int levelWithAtMost(const DatasetInfo& info, std::int64_t max_voxels) {
  for (const LevelInfo& level : info.levels) {
    if (level.dims[0] * level.dims[1] * level.dims[2] <= max_voxels) {
      return level.level;
    }
  }
  return info.levels.back().level;
}

/// How round the part is across each axis: the share of the bounding rectangle (in mm) of its
/// projection along the axis that the projection fills, against a disk's pi/4, and how square
/// that rectangle is.
struct Roundness {
  int axis = -1;           // the axis the part is round across, or -1
  double diameter_mm = 0;  // of the round cross-section
  double length_mm = 0;    // along the axis
};

Roundness measureRoundness(const Dataset& dataset, int level) {
  const LevelInfo& info = dataset.level(level);
  const auto& dims = info.dims;
  std::vector<float> values(static_cast<std::size_t>(dims[0] * dims[1] * dims[2]));
  dataset.readRegion(level, Box{{0, 0, 0}, dims}, values, 0.0F);
  const auto threshold = static_cast<float>(dataset.info().threshold);
  const auto at = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
    return values[static_cast<std::size_t>(x + (dims[0] * (y + (dims[1] * z))))] > threshold;
  };
  Roundness best;
  double best_score = 0.0;
  for (int axis = 0; axis < 3; ++axis) {
    const int u = (axis + 1) % 3;
    const int v = (axis + 2) % 3;
    std::vector<std::uint8_t> shadow(static_cast<std::size_t>(dims[u] * dims[v]), 0);
    std::array<std::int64_t, 3> lo = {dims[0], dims[1], dims[2]};
    std::array<std::int64_t, 3> hi = {-1, -1, -1};
    for (std::int64_t z = 0; z < dims[2]; ++z) {
      for (std::int64_t y = 0; y < dims[1]; ++y) {
        for (std::int64_t x = 0; x < dims[0]; ++x) {
          if (!at(x, y, z)) {
            continue;
          }
          const std::array<std::int64_t, 3> p = {x, y, z};
          shadow[static_cast<std::size_t>(p[u] + (dims[u] * p[v]))] = 1;
          for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
          }
        }
      }
    }
    if (hi[0] < 0) {
      return {};
    }
    const double filled =
        static_cast<double>(std::count(shadow.begin(), shadow.end(), std::uint8_t{1}));
    const auto width = static_cast<double>(hi[u] - lo[u] + 1);
    const auto height = static_cast<double>(hi[v] - lo[v] + 1);
    const double width_mm = width * info.voxel_size[u];
    const double height_mm = height * info.voxel_size[v];
    const double square = std::min(width_mm, height_mm) / std::max(width_mm, height_mm);
    const double disk = filled / (width * height) / (std::numbers::pi / 4.0);
    // A disk fills pi/4 of its square; a rectangle 4/pi of that, an L or a frame less.
    const double score = square * (1.0 - std::abs(disk - 1.0));
    constexpr double kRound = 0.9;
    if (square > kRound && std::abs(disk - 1.0) < 0.08 && score > best_score) {
      best_score = score;
      best.axis = axis;
      best.diameter_mm = 0.5 * (width_mm + height_mm);
      best.length_mm = static_cast<double>(hi[axis] - lo[axis] + 1) * info.voxel_size[axis];
    }
  }
  return best;
}

}  // namespace

Json suggestAnalyses(const Dataset& dataset, const AnalysisContext& context) {
  const DatasetInfo& info = dataset.info();
  const auto has = [&](const char* operation) {
    return std::ranges::find(context.available, operation) != context.available.end();
  };
  const auto done = [&](const char* operation) {
    return std::ranges::find(context.done, operation) != context.done.end();
  };
  Json facts = Json::object();

  // Materials, from the histogram of a coarse level (averaging narrows the peaks).
  const int histogram_level = levelWithAtMost(info, std::int64_t{1} << 24U);
  const auto threshold = static_cast<float>(info.threshold);
  const MaterialSuggestion materials =
      suggestMaterials(datasetHistogram(dataset, histogram_level), threshold, 8);
  // Classes of the mixture that hold a fair share of the voxels and are narrow for their grey
  // value: the voxels at edges (partial volume) form a broad class of a few percent between air
  // and the material, which is no material of its own.
  constexpr double kMinShare = 0.05;
  constexpr double kMaxSpread = 0.3;
  int material_count = 0;
  for (const MixtureComponent& component : materials.components) {
    if (component.weight >= kMinShare &&
        component.sigma <= kMaxSpread * (component.mean - threshold)) {
      ++material_count;
    }
  }
  material_count = std::max(1, material_count);
  facts["materials"] = material_count;
  facts["material_classes"] = materials.peaks.size();
  facts["material_grey_values"] = materials.peaks;
  facts["ambiguous_fraction"] = materials.ambiguous_fraction;

  const int shape_level = levelWithAtMost(info, std::int64_t{1} << 21U);
  double cupping = 0.0;
  try {
    cupping = measureCupping(dataset, shape_level).cupping;
  } catch (const std::exception&) {
    cupping = 0.0;
  }
  facts["cupping"] = cupping;
  const Roundness round = measureRoundness(dataset, shape_level);
  if (round.axis >= 0) {
    facts["round_across"] = std::string(1, static_cast<char>('x' + round.axis));
    facts["diameter_mm"] = round.diameter_mm;
  }
  const bool battery_name =
      mentions(context.name, {"battery", "batterie", "cell", "zelle", "akku", "li-ion", "liion",
                              "18650", "21700", "2170", "4680", "4695", "26650", "pouch"});
  const bool fibre_name =
      mentions(context.name, {"cfrp", "gfrp", "cfk", "gfk", "frp", "fibre", "fiber", "faser",
                              "composite", "laminate", "prepreg", "twill", "roving", "woven"});

  Json suggestions = Json::array();
  const auto add = [&](const char* operation, const std::string& title, const std::string& why,
                       Json params = Json::object()) {
    if (!has(operation)) {
      return;
    }
    suggestions.push_back({{"operation", operation},
                           {"title", title},
                           {"why", why},
                           {"params", std::move(params)},
                           {"done", done(operation)}});
  };

  // Cylindrical cells: round, several materials (housing, electrodes, separator), or so named.
  const bool battery = battery_name || (round.axis >= 0 && material_count >= 3);
  if (battery) {
    add("battery_inspection", "Battery cell inspection",
        battery_name
            ? "The name suggests a battery cell: electrode layers, anode overhang, "
              "particles and voids are measured."
            : "The part is round across " + facts["round_across"].get<std::string>() + " (" +
                  formatMm(round.diameter_mm) + ") and holds " + std::to_string(material_count) +
                  " materials, like a cylindrical cell: electrode layers, anode "
                  "overhang, particles and voids are measured.");
  }
  if (fibre_name) {
    add("fibre_orientation", "Fibre orientation",
        "The name suggests a fibre composite: the fibre directions and fibre families are "
        "measured from the structure of the grey values.");
  }
  if (!context.nearby_meshes.empty() && context.meshes.empty()) {
    const auto& mesh = context.nearby_meshes.front();
    add("compare_cad", "Nominal-actual comparison",
        "The CAD model " + mesh.filename().string() +
            " lies next to the scan: the scan's surface is aligned to it and every deviation "
            "is shown in colour.",
        {{"cad_path", mesh.string()}});
  }
  for (const auto& [id, name] : context.meshes) {
    add("compare_objects", "Compare with " + name,
        "The project holds the CAD model " + name +
            ": the scan is aligned to it and every deviation is shown in colour.",
        {{"nominal", id}});
    break;
  }
  if (!battery && !fibre_name) {
    add("porosity", "Porosity analysis",
        material_count > 1
            ? "Finds pores, shrinkage cavities and zones of loosened microstructure inside the "
              "part, per material once the materials are segmented."
            : "Finds pores, shrinkage cavities and zones of loosened microstructure inside the "
              "part, with their size and position.");
  }
  if (material_count > 1) {
    std::ostringstream text;
    text << "The histogram shows " << material_count
         << " materials, with clear valleys between them: each gets its colour, volume and "
            "analyses of its own.";
    add("segment_materials", "Material segmentation", text.str());
  }
  if (!battery && !fibre_name) {
    add("wall_thickness", "Wall thickness",
        "Measures the wall thickness everywhere on the surface and marks thin and thick walls.");
  }
  constexpr double kCupping = 0.1;
  if (cupping > kCupping) {
    std::ostringstream text;
    text << "The material is " << std::lround(100.0 * cupping)
         << " % darker in the core than at the surface (beam hardening): the correction "
            "evens out the grey values before thresholds and pores are measured.";
    add("beam_hardening", "Correct beam hardening", text.str());
  }
  constexpr double kAmbiguous = 0.25;
  if (material_count > 1 && materials.ambiguous_fraction > kAmbiguous) {
    std::ostringstream text;
    text << std::lround(100.0 * materials.ambiguous_fraction)
         << " % of the voxels lie between the materials' grey values (noise): denoising "
            "separates them before a segmentation.";
    add("denoise", "Denoise", text.str());
  }
  if (done("porosity") && !done("report")) {
    add("report", "Test report",
        "The porosity analysis is done: the report evaluates it against acceptance limits "
        "(BDG P 202) and documents the inspection.");
  }
  // Done ones last, in their order.
  std::ranges::stable_partition(suggestions,
                                [](const Json& s) { return !s.at("done").get<bool>(); });
  return {{"suggestions", suggestions}, {"facts", facts}};
}

}  // namespace voxelsieve
