#include "voxelsieve/gui.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "detail/material_volume.hpp"
#include "detail/png.hpp"
#include "voxelsieve/compare.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/dicom.hpp"
#include "voxelsieve/gzipped_xml.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/render.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

constexpr const char* kRunPrefix = "run_";
// Long enough to save calls, short enough for clients that give up on a call after a minute.
constexpr double kDefaultWaitSeconds = 30.0;
constexpr double kMaxWaitSeconds = 50.0;
constexpr std::uintmax_t kMaxReadBytes = 1U << 20U;
constexpr std::uintmax_t kMaxImageBytes = 8U << 20U;
constexpr std::size_t kMaxListedFiles = 1000;

Json objectSchema(Json properties, Json required = Json::array()) {
  Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
  if (!required.empty()) {
    schema["required"] = std::move(required);
  }
  return schema;
}

const Json& artifactProperties() {
  static const Json properties = {
      {"step", {{"type", "integer"}, {"description", "Step id; default: the latest step"}}},
      {"output", {{"type", "string"}, {"description", "Output name; default: the first output"}}}};
  return properties;
}

/// Picture of a histogram for the histogram method: bars on a logarithmic scale, grey below the
/// air threshold and in the colour of each suggested material above it, the air threshold as a
/// red line. RGB PNG.
std::vector<std::uint8_t> histogramPng(const std::vector<std::uint64_t>& counts, double first,
                                       double bin_width, float threshold,
                                       const std::vector<float>& material_starts) {
  constexpr int kWidth = 768;
  constexpr int kHeight = 200;
  constexpr std::uint8_t kBackground = 24;
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(kWidth * kHeight) * 3, kBackground);
  const auto set = [&rgb](int x, int y, const std::array<std::uint8_t, 3>& color) {
    std::copy(color.begin(), color.end(),
              rgb.begin() + (static_cast<std::ptrdiff_t>(x) + std::ptrdiff_t{kWidth} * y) * 3);
  };
  const std::uint64_t highest = counts.empty() ? 0 : *std::ranges::max_element(counts);
  for (int x = 0; x < kWidth && highest > 0; ++x) {
    const auto bin = static_cast<std::size_t>(x) * counts.size() / kWidth;
    const double grey = first + (static_cast<double>(bin) + 0.5) * bin_width;
    std::array<std::uint8_t, 3> color{150, 150, 150};
    for (std::size_t m = 0; m < material_starts.size(); ++m) {
      if (grey >= material_starts[m]) {
        color = detail::materialColor(static_cast<int>(m) + 1);
      }
    }
    const double fraction =
        std::log1p(static_cast<double>(counts[bin])) / std::log1p(static_cast<double>(highest));
    const int bar = static_cast<int>(std::lround(fraction * kHeight));
    for (int y = kHeight - bar; y < kHeight; ++y) {
      set(x, y, color);
    }
  }
  const double span = bin_width * static_cast<double>(counts.size());
  if (span > 0.0 && threshold >= first && threshold < first + span) {
    const int x = std::min(kWidth - 1, static_cast<int>((threshold - first) / span * kWidth));
    for (int y = 0; y < kHeight; ++y) {
      set(x, y, {230, 60, 50});
    }
  }
  return detail::encodePng(kWidth, kHeight, 3, rgb);
}

/// Removes the part of `mesh` (and its vertex colours) on one side of the plane where coordinate
/// `axis` is `position`: what lies above it when `keep_below`, else below. Triangles across the
/// plane are clipped along it.
void cutMesh(IndexedMesh& mesh, std::vector<std::array<std::uint8_t, 3>>& colors, std::size_t axis,
             float position, bool keep_below) {
  const auto inside = [&](const std::array<float, 3>& p) {
    return keep_below ? p[axis] <= position : p[axis] >= position;
  };
  IndexedMesh out;
  std::vector<std::array<std::uint8_t, 3>> out_colors;
  out.points = mesh.points;
  out_colors = colors;
  for (const auto& t : mesh.triangles) {
    // Sutherland-Hodgman against one plane: at most four corners, so one or two triangles.
    std::vector<std::uint32_t> polygon;
    for (std::size_t k = 0; k < 3; ++k) {
      const std::uint32_t a = t[k];
      const std::uint32_t b = t[(k + 1) % 3];
      const bool a_in = inside(out.points[a]);
      const bool b_in = inside(out.points[b]);
      if (a_in) {
        polygon.push_back(a);
      }
      if (a_in != b_in) {
        const auto& p = out.points[a];
        const auto& q = out.points[b];
        const float f = (position - p[axis]) / (q[axis] - p[axis]);
        out.points.push_back(
            {p[0] + f * (q[0] - p[0]), p[1] + f * (q[1] - p[1]), p[2] + f * (q[2] - p[2])});
        out_colors.push_back(colors[a]);
        polygon.push_back(static_cast<std::uint32_t>(out.points.size() - 1));
      }
    }
    for (std::size_t k = 2; k < polygon.size(); ++k) {
      out.triangles.push_back({polygon[0], polygon[k - 1], polygon[k]});
    }
  }
  mesh = std::move(out);
  colors = std::move(out_colors);
}

/// The files of a step output (a directory or a single file) with their sizes, at most
/// kMaxListedFiles; empty when the output is gone.
Json outputFiles(const std::filesystem::path& root) {
  Json files = Json::array();
  std::error_code error;
  if (!std::filesystem::is_directory(root, error)) {
    const std::uintmax_t size = std::filesystem::file_size(root, error);
    if (!error) {
      files.push_back({{"file", root.filename().string()}, {"size_bytes", size}});
    }
    return files;
  }
  for (auto it = std::filesystem::recursive_directory_iterator(root, error);
       !error && it != std::filesystem::recursive_directory_iterator() &&
       files.size() < kMaxListedFiles;
       it.increment(error)) {
    if (it->is_regular_file(error)) {
      files.push_back({{"file", it->path().lexically_relative(root).generic_string()},
                       {"size_bytes", it->file_size(error)}});
    }
  }
  return files;
}

std::uintmax_t directorySize(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_directory(path, error)) {
    return std::filesystem::is_regular_file(path, error) ? std::filesystem::file_size(path, error)
                                                         : 0;
  }
  std::uintmax_t size = 0;
  for (auto it = std::filesystem::recursive_directory_iterator(path, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    if (it->is_regular_file(error)) {
      size += it->file_size(error);
    }
  }
  return size;
}

Json datasetInfoJson(const DatasetInfo& info) {
  Json levels = Json::array();
  for (const LevelInfo& level : info.levels) {
    levels.push_back({{"level", level.level},
                      {"dims", level.dims},
                      {"voxel_size_mm", level.voxel_size},
                      {"bricks", level.bricks.size()}});
  }
  Json json = {{"dims", info.dims}};
  writeVoxelSize(json, info.voxel_size);
  json.update({{"brick_size", info.brick_size},
               {"threshold", info.threshold},
               {"air_level", info.air_level},
               {"margin_voxels", info.margin_voxels},
               {"min_material_voxels", info.min_material_voxels},
               {"active_voxels", info.active_voxel_count},
               {"levels", levels}});
  return json;
}

/// `file` relative to `root`, refusing paths that leave it.
std::filesystem::path insideOf(const std::filesystem::path& root, const std::string& file) {
  const auto base = std::filesystem::weakly_canonical(root);
  const auto path = std::filesystem::weakly_canonical(base / file);
  const auto relative = path.lexically_relative(base);
  if (relative.empty() || *relative.begin() == "..") {
    throw std::invalid_argument("File '" + file + "' is outside the artifact");
  }
  return path;
}

constexpr std::size_t kMaxBrowsedEntries = 5000;
constexpr std::size_t kOpenDatasets = 2;
// Open datasets share one brick cache, so keeping more of them open costs only their index.
constexpr std::size_t kOpenVolumes = 8;
constexpr std::size_t kViewCacheBytes = std::size_t{1} << 30U;
// Triangle budget of a volume's surface for its cut lines in slices.
constexpr std::size_t kCutTriangles = 500000;
// Colours of the objects in the order they were created, as OBJECT_COLORS in resources/ui/scene.js.
constexpr std::array<std::array<std::uint8_t, 3>, 6> kObjectColors = {{{196, 200, 207},
                                                                       {230, 150, 60},
                                                                       {90, 160, 230},
                                                                       {120, 190, 110},
                                                                       {200, 110, 170},
                                                                       {220, 200, 80}}};

/// The colour an object was given (#rrggbb), else the one of its place in the list.
std::array<std::uint8_t, 3> objectColor(const ProjectObject& object, std::size_t index) {
  std::array<std::uint8_t, 3> color = kObjectColors[index % kObjectColors.size()];
  if (object.color.size() == 7 && object.color[0] == '#') {
    for (std::size_t c = 0; c < 3; ++c) {
      color[c] =
          static_cast<std::uint8_t>(std::stoi(object.color.substr(1 + 2 * c, 2), nullptr, 16));
    }
  }
  return color;
}

std::string base64(std::span<const std::uint8_t> bytes) {
  static constexpr std::string_view kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string text;
  text.reserve((bytes.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const std::uint32_t chunk = (std::uint32_t{bytes[i]} << 16U) |
                                (i + 1 < bytes.size() ? std::uint32_t{bytes[i + 1]} << 8U : 0U) |
                                (i + 2 < bytes.size() ? std::uint32_t{bytes[i + 2]} : 0U);
    text.push_back(kAlphabet[(chunk >> 18U) & 63U]);
    text.push_back(kAlphabet[(chunk >> 12U) & 63U]);
    text.push_back(i + 1 < bytes.size() ? kAlphabet[(chunk >> 6U) & 63U] : '=');
    text.push_back(i + 2 < bytes.size() ? kAlphabet[chunk & 63U] : '=');
  }
  return text;
}

std::vector<std::uint8_t> decodeBase64(std::string_view text) {
  // Accepts a data URL ("data:image/png;base64,...") as the canvas produces it.
  if (const auto comma = text.find(','); text.starts_with("data:") && comma != text.npos) {
    text.remove_prefix(comma + 1);
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(text.size() / 4 * 3);
  std::uint32_t chunk = 0;
  int bits = 0;
  for (const char c : text) {
    std::uint32_t value = 0;
    if (c >= 'A' && c <= 'Z') {
      value = static_cast<std::uint32_t>(c - 'A');
    } else if (c >= 'a' && c <= 'z') {
      value = static_cast<std::uint32_t>(c - 'a' + 26);
    } else if (c >= '0' && c <= '9') {
      value = static_cast<std::uint32_t>(c - '0' + 52);
    } else if (c == '+') {
      value = 62;
    } else if (c == '/') {
      value = 63;
    } else if (c == '=' || c == '\n' || c == '\r') {
      continue;
    } else {
      throw std::invalid_argument("Invalid base64 data");
    }
    chunk = (chunk << 6U) | value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(static_cast<std::uint8_t>((chunk >> static_cast<unsigned>(bits)) & 0xFFU));
    }
  }
  return bytes;
}

Json savedViewJson(const SavedView& view) {
  return {{"id", view.id},
          {"name", view.name},
          {"created", view.created},
          {"state", view.state},
          {"has_image", view.has_image}};
}

int axisIndex(const std::string& axis) {
  if (axis == "x") {
    return 0;
  }
  return axis == "y" ? 1 : 2;
}

/// Most recently used entry of a small cache, opened with `open` when missing.
template <typename Value, typename Open>
std::shared_ptr<const Value> cached(
    std::vector<std::pair<std::filesystem::path, std::shared_ptr<const Value>>>& entries,
    const std::filesystem::path& key, std::size_t capacity, const Open& open) {
  for (auto it = entries.begin(); it != entries.end(); ++it) {
    if (it->first == key) {
      auto entry = *it;
      entries.erase(it);
      entries.push_back(entry);
      return entry.second;
    }
  }
  auto value = open();
  entries.emplace_back(key, value);
  if (entries.size() > capacity) {
    entries.erase(entries.begin());
  }
  return value;
}

std::string lowerExtension(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension;
}

bool isTiffFile(const std::filesystem::path& path) {
  const std::string extension = lowerExtension(path);
  return extension == ".tif" || extension == ".tiff";
}

/// Whether a directory holds TIFF slices, directly or one folder down (data/, target/). Looks at a
/// bounded number of entries, so that browsing stays fast on huge directories.
bool containsTiff(const std::filesystem::path& dir, int depth = 1) {
  constexpr int kLookedAt = 64;
  int looked_at = 0;
  std::error_code error;
  for (auto it = std::filesystem::directory_iterator(
           dir, std::filesystem::directory_options::skip_permission_denied, error);
       !error && it != std::filesystem::directory_iterator() && looked_at < kLookedAt;
       it.increment(error), ++looked_at) {
    std::error_code ignored;
    if (it->is_directory(ignored) ? depth > 0 && containsTiff(it->path(), depth - 1)
                                  : isTiffFile(it->path())) {
      return true;
    }
  }
  return false;
}

/// The modification time of a file as text, for cache keys. (Its clock's count may be a 128-bit
/// integer, as in libc++, which std::to_string does not take.)
std::string fileStamp(const std::filesystem::path& file) {
  return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::filesystem::last_write_time(file).time_since_epoch())
                            .count());
}

Json browse(const std::filesystem::path& requested) {
  const auto dir = std::filesystem::weakly_canonical(std::filesystem::absolute(requested));
  if (!std::filesystem::is_directory(dir)) {
    throw std::invalid_argument("Not a directory: " + dir.string());
  }
  Json entries = Json::array();
  std::error_code error;
  for (auto it = std::filesystem::directory_iterator(
           dir, std::filesystem::directory_options::skip_permission_denied, error);
       !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
    const auto& path = it->path();
    const std::string name = path.filename().string();
    if (name.starts_with(".")) {
      continue;
    }
    Json entry = {{"name", name}, {"path", path.string()}};
    std::error_code ignored;
    if (it->is_directory(ignored)) {
      entry["kind"] = std::filesystem::exists(path / "project.json", ignored) ? "project"
                      : std::filesystem::exists(path / "index.json", ignored) ? "dataset"
                      : containsTiff(path)                                    ? "tiff"
                      : containsDicom(path)                                   ? "dicom"
                                                                              : "dir";
      entry["directory"] = true;
    } else {
      const auto sidecar = rawSidecarPath(path);
      entry["kind"] = isDicomArchive(path)                                 ? "dicom"
                      : isTiffFile(path) || lowerExtension(path) == ".zip" ? "tiff"
                      : isGzippedXmlFile(path)                             ? "gzipped_xml"
                      : isDicomFile(path)                                  ? "dicom"
                      : path.extension() != ".json" && std::filesystem::exists(sidecar, ignored)
                          ? "raw"
                          : "file";
      entry["size_bytes"] = it->file_size(ignored);
    }
    entries.push_back(std::move(entry));
    if (entries.size() == kMaxBrowsedEntries) {
      break;
    }
  }
  std::sort(entries.begin(), entries.end(), [](const Json& a, const Json& b) {
    const bool a_file = !a.contains("directory");
    const bool b_file = !b.contains("directory");
    return a_file != b_file ? b_file : a.at("name") < b.at("name");
  });
  return {{"path", dir.string()}, {"parent", dir.parent_path().string()}, {"entries", entries}};
}

}  // namespace

std::vector<std::filesystem::path> pluginPathFromEnvironment() {
  std::vector<std::filesystem::path> dirs;
  const char* value = std::getenv("VOXELSIEVE_PLUGIN_PATH");  // NOLINT(concurrency-mt-unsafe)
  if (value == nullptr) {
    return dirs;
  }
  std::stringstream stream(value);
  std::string dir;
  // Separated like PATH: ';' on Windows, where ':' follows drive letters.
#ifdef _WIN32
  constexpr char kSeparator = ';';
#else
  constexpr char kSeparator = ':';
#endif
  while (std::getline(stream, dir, kSeparator)) {
    if (!dir.empty()) {
      dirs.emplace_back(dir);
    }
  }
  return dirs;
}

Gui::Gui(const std::vector<std::filesystem::path>& plugin_dirs)
    : view_cache_(std::make_shared<BrickCache>(kViewCacheBytes)) {
  registerBuiltinOperations(registry_);
  for (const auto& dir : plugin_dirs) {
    for (std::string& message : registry_.loadPlugins(dir)) {
      plugin_messages_.push_back(std::move(message));
    }
  }
}

Gui::~Gui() {
  cancel_ = true;
  if (background_.joinable()) {
    background_.join();
  }
}

std::vector<GuiMethod> Gui::methods() const {
  const Json path = {{"type", "string"}, {"format", "path"}, {"description", "Project directory"}};
  std::vector<GuiMethod> methods = {
      {"project_create",
       "Creates a new project in an empty or missing directory and opens it. Every step is saved "
       "immediately; there is no separate save.",
       objectSchema({{"path", path}, {"name", {{"type", "string"}}}}, {"path"})},
      {"project_open", "Opens an existing project.", objectSchema({{"path", path}}, {"path"})},
      {"project_status",
       "The open project: name, directory, cursor and the step protocol with parameters, inputs, "
       "outputs, messages and summaries, the objects and the analysis results (see results, "
       "without files). Steps with active=false are undone.",
       objectSchema(Json::object())},
      {"project_save_as", "Copies the project to a new directory and continues there.",
       objectSchema({{"path", path}}, {"path"})},
      {"undo", "Undoes the latest active step. Instant; outputs stay until a new step runs.",
       objectSchema(Json::object())},
      {"redo", "Redoes the next undone step.", objectSchema(Json::object())},
      {"objects",
       "The objects of the project, volumes and meshes, in the order they were created: id, "
       "name, kind, the output holding their data (source), the step that created them and "
       "the steps that moved them, and their pose in the global coordinate system (a row-major "
       "4x4 matrix from object to global coordinates in mm). Also the active object, whose "
       "outputs are the default inputs of new steps.",
       objectSchema(Json::object())},
      {"results",
       "The analysis results of the project per object (ADR 0023), in the order they were made: "
       "every output of an active step that is not an object's data or a motion, such as pores "
       "(porosity), materials, surfaces, comparisons with CAD, reports and plugin outputs. Each "
       "names its step and output, type, operation, object, the dataset or mesh it was computed "
       "from (source), the results it builds on, its summary and files, and whether it is the "
       "current one of its type for its object or replaced by a newer one.",
       objectSchema({{"object",
                      {{"type", "string"},
                       {"description", "Object id; default: the results of all objects"}}},
                     {"current_only",
                      {{"type", "boolean"},
                       {"default", false},
                       {"description", "Leave out results replaced by newer ones"}}}})},
      {"object_add",
       "Adds a dataset (a .vsieve directory) or a mesh such as a CAD model (STL in mm) as a new "
       "object, as a step. Raw volumes, TIFF stacks, DICOM stacks and gzipped XML projects become "
       "objects through run_import_raw, run_import_tiff, run_import_dicom and "
       "run_import_gzipped_xml. Objects are placed with run_move.",
       objectSchema({{"path",
                      {{"type", "string"},
                       {"format", "path"},
                       {"description", "Dataset directory or STL file"}}},
                     {"name", {{"type", "string"}, {"description", "Default: the file name"}}}},
                    {"path"})},
      {"object_select",
       "Chooses the active object: steps run without explicit inputs take them from its outputs "
       "first. An empty id clears the choice. Not a step.",
       objectSchema({{"id", {{"type", "string"}}}}, {"id"})},
      {"view_objects",
       "Renders objects where they lie in global coordinates as a PNG image, each in its own "
       "colour (listed in the result): meshes as their triangles, volumes as their extracted "
       "surface or, before a surface step, as the box they fill. With materials, volumes with a "
       "material segmentation show the surface of each material in its colour instead; choose "
       "material_ids to look at inner materials alone, and cut_axis to cut the scene open. Shows "
       "whether an alignment worked; z is up in the image.",
       objectSchema(
           {{"objects",
             {{"type", "array"},
              {"items", {{"type", "string"}}},
              {"description", "Object ids; default: all objects"}}},
            {"azimuth_deg",
             {{"type", "number"},
              {"default", -60.0},
              {"description", "Direction towards the camera, about z from +x"}}},
            {"elevation_deg",
             {{"type", "number"}, {"minimum", -90}, {"maximum", 90}, {"default", 25.0}}},
            {"width", {{"type", "integer"}, {"minimum", 64}, {"maximum", 2048}, {"default", 900}}},
            {"height", {{"type", "integer"}, {"minimum", 64}, {"maximum", 2048}, {"default", 700}}},
            {"max_triangles",
             {{"type", "integer"},
              {"minimum", 1000},
              {"maximum", 5000000},
              {"default", 300000},
              {"description", "Triangle budget of each volume's surface"}}},
            {"materials",
             {{"type", "boolean"},
              {"default", false},
              {"description", "Volumes in the colours of their latest material segmentation"}}},
            {"material_ids",
             {{"type", "array"},
              {"items", {{"type", "integer"}, {"minimum", 1}}},
              {"description", "With materials: the materials to show; default: all"}}},
            {"cut_axis",
             {{"type", "string"},
              {"enum", {"x", "y", "z"}},
              {"description",
               "Cuts the scene open with a plane normal to this global axis, to look inside"}}},
            {"cut_mm",
             {{"type", "number"},
              {"description", "With cut_axis: where the plane lies; default: the scene's middle"}}},
            {"cut_keep",
             {{"type", "string"},
              {"enum", {"below", "above"}},
              {"default", "below"},
              {"description", "With cut_axis: the side of the plane that stays"}}}})},
      {"step_telemetry",
       "Time and resources a step used: wall time, CPU time and busy cores, peak memory (own "
       "and mapped file pages), bytes read and written and page faults, per phase, with hints "
       "and a timeline sampled every second. Recorded locally for every operation.",
       objectSchema(
           {{"step",
             {{"type", "integer"}, {"description", "Step id; default: the latest step"}}}})},
      {"list_operations",
       "All operations with inputs, outputs and parameter schema, including plugins.",
       objectSchema(Json::object())},
      {"dataset_info",
       "Dimensions, voxel size, threshold and resolution levels of a dataset output.",
       objectSchema(artifactProperties())},
      {"histogram",
       "Grey-value histogram of a dataset (its active voxels: the part, its voids and a thin "
       "margin of air) with the air threshold and a suggested number of materials: one per "
       "clear peak above the threshold, with the thresholds in the valleys between them, or "
       "more when a Gaussian mixture fitted to the histogram also finds classes that show only "
       "as a shoulder or a tail (clear_peaks counts the peaks alone, mixture lists the classes "
       "with grey value, spread and share). ambiguous_fraction is the share of the voxels whose "
       "class is uncertain (below 90 % probability): the voxels at class boundaries, a measure "
       "of how much a segmentation depends on noise. Comes with a picture (logarithmic scale, "
       "threshold in red, the suggested materials in their colours). To preview the classes "
       "before segmenting, pass material_thresholds as preview_thresholds to view_slice. "
       "run_segment_materials with materials 0 uses the same suggestion, counted at full "
       "resolution; to choose differently, pass materials and material_thresholds.",
       [] {
         Json properties = artifactProperties();
         properties["level"] = {
             {"type", "integer"},
             {"minimum", 0},
             {"description",
              "Resolution level to count; default: the finest with at most 2^27 voxels"}};
         properties["bins"] = {
             {"type", "integer"}, {"minimum", 8}, {"maximum", 1024}, {"default", 256}};
         properties["max_materials"] = {
             {"type", "integer"}, {"minimum", 1}, {"maximum", 8}, {"default", 8}};
         return objectSchema(properties);
       }()},
      {"list_files", "Files of a step output (a directory or a single file) with sizes.",
       objectSchema(artifactProperties())},
      {"view_slice",
       "Renders a slice through a dataset as a PNG image, with pores in red and loosened zones in "
       "yellow when a porosity analysis of the dataset exists, and materials in their colours "
       "when a material segmentation exists. The whole slice is shown at the "
       "resolution level that fits max_pixels; the result says which axes run right and down. "
       "One pixel per voxel: pixel_size_mm gives its width and height, which differ when the "
       "voxels are not cubes.",
       [] {
         Json properties = artifactProperties();
         properties["axis"] = {{"type", "string"},
                               {"enum", {"x", "y", "z"}},
                               {"default", "z"},
                               {"description", "Axis normal to the slice"}};
         properties["index"] = {{"type", "integer"},
                                {"minimum", 0},
                                {"description", "Slice position in voxels; default: the middle"}};
         properties["max_pixels"] = {
             {"type", "integer"}, {"minimum", 64}, {"maximum", 2048}, {"default", 768}};
         properties["window_min"] = {{"type", "number"},
                                     {"description", "Grey value shown black; default: automatic"}};
         properties["window_max"] = {{"type", "number"},
                                     {"description", "Grey value shown white; default: automatic"}};
         properties["overlay"] = {{"type", "boolean"}, {"default", true}};
         properties["porosity_step"] = {
             {"type", "integer"},
             {"description", "Porosity step for the overlay; default: the latest of this dataset"}};
         properties["materials_step"] = {
             {"type", "integer"},
             {"description",
              "Material segmentation for the overlay; default: the latest of this dataset"}};
         properties["preview_thresholds"] = {
             {"type", "array"},
             {"items", {{"type", "number"}}},
             {"description",
              "Preview of a material segmentation without running it: voxels above the air "
              "threshold are coloured by the class these ascending grey values give (material 1 "
              "below the first, 2 up to the second, ...), for example histogram's "
              "material_thresholds. Replaces the materials overlay"}};
         properties["objects"] = {
             {"type", "array"},
             {"items", {{"type", "string"}}},
             {"description",
              "Other objects to show where they lie (ADR 0018), each in its colour as in "
              "view_objects: volumes blended over the slice, and the outline where meshes, "
              "surfaces and volume boxes cut it"}};
         properties.erase("output");
         return objectSchema(properties);
       }()},
      {"view_set",
       "Stores how the project is shown (the UI sends its viewer state), so opening the project "
       "shows it the same way. Not a step.",
       objectSchema({{"state", {{"type", "object"}}}}, {"state"})},
      {"view_save",
       "Saves a view under a name: a view state (default: the current one) and optionally a PNG "
       "picture of it. Saved views are listed in project_status and are not steps.",
       objectSchema({{"name", {{"type", "string"}}},
                     {"state", {{"type", "object"}}},
                     {"image_base64", {{"type", "string"}, {"description", "PNG, base64"}}}},
                    {"name"})},
      {"view_list", "The saved views of the project with name, state and whether a picture exists.",
       objectSchema(Json::object())},
      {"import_preview",
       "A first look at the input of the import that is running, available after a few slices, "
       "long before the import is done; or the preview kept with import step `step` (default "
       "when nothing runs: the latest). Gives the slices read, the threshold estimate, the grey "
       "value window, a histogram and a picture of the central sections normal to z, y and x "
       "with the histogram below (threshold in red). The import goes on without reading these "
       "slices again; a staged import (stacks, files on a share) replaces the preview with a "
       "sharper one as it copies more slices, until `complete` (every slice read).",
       objectSchema({{"step", {{"type", "integer"}}}})},
      {"view_image", "The picture of a saved view as a PNG image.",
       objectSchema({{"id", {{"type", "integer"}}}}, {"id"})},
      {"view_rename", "Renames a saved view.",
       objectSchema({{"id", {{"type", "integer"}}}, {"name", {{"type", "string"}}}},
                    Json::array({"id", "name"}))},
      {"view_delete", "Deletes a saved view and its picture.",
       objectSchema({{"id", {{"type", "integer"}}}}, {"id"})},
      {"wait",
       "Waits until the running operation is done, at most `seconds` (default 30). Gives done, "
       "the progress while it still runs and, as result, the step of the latest operation "
       "started with background: true (or its error). Call it again while done is false.",
       objectSchema({{"seconds",
                      {{"type", "number"},
                       {"minimum", 0},
                       {"maximum", kMaxWaitSeconds},
                       {"default", kDefaultWaitSeconds}}}})},
      {"pipeline",
       "Runs several operations one after the other in one call, such as import, isolate_part, "
       "segment_materials and material_meshes, so nothing waits between them. Each step names "
       "an operation (as run_<operation> without the prefix) and its parameters as that method "
       "takes them; inputs not given come from the active object, which is the object the "
       "previous step made or worked on. Stops at the first failure and gives the steps done "
       "until then with the error. With background: true it returns at once; wait gives the "
       "result and cancel stops it.",
       objectSchema({{"steps",
                      {{"type", "array"},
                       {"minItems", 1},
                       {"items", objectSchema({{"operation", {{"type", "string"}}},
                                               {"params",
                                                {{"type", "object"}, {"default", Json::object()}}}},
                                              {"operation"})}}},
                     {"background", {{"type", "boolean"}, {"default", false}}}},
                    {"steps"})},
      {"cancel",
       "Stops the running operation at its next progress report; its step is kept as failed. "
       "Gives the operation that was asked to stop, or null when none was running.",
       objectSchema(Json::object())},
      {"browse",
       "Lists a directory on the machine running VoxelSieve, to choose raw volumes, TIFF stacks, "
       "DICOM stacks, gzipped XML projects, datasets (.vsieve), projects and inspection orders. "
       "Entries have a kind: dir, project, dataset, raw (a file with a JSON sidecar, also "
       "name.raw.gz with name.json), tiff (a "
       "TIFF file, a ZIP archive or a directory with TIFF slices, for import_tiff), dicom (a "
       "DICOM file or a directory with DICOM slices, for import_dicom), gzipped_xml (a gzipped XML "
       "project, for import_gzipped_xml) or file; directories carry directory: true.",
       objectSchema({{"path",
                      {{"type", "string"},
                       {"format", "path"},
                       {"description",
                        "Default: the first allowed directory, else the working directory"}}}})},
      {"read_file",
       "Reads a text file of a step output, such as report.json or porosity.json (at most 1 MB), "
       "or a PNG picture such as projection_z.png of a porosity step (at most 8 MB).",
       [] {
         Json properties = artifactProperties();
         properties["file"] = {{"type", "string"},
                               {"description", "File inside the output directory"}};
         return objectSchema(properties);
       }()},
  };
  for (const auto& operation : registry_.all()) {
    const OperationInfo& info = operation->info();
    Json schema = info.parameters;
    std::string inputs;
    for (const PortInfo& port : info.inputs) {
      inputs += (inputs.empty() ? "" : ", ") + port.name + " (" + port.type +
                (port.optional ? ", optional" : "") + ")";
    }
    if (!info.inputs.empty()) {
      schema["properties"]["inputs"] = {
          {"type", "object"},
          {"description",
           "Optional: {input: {step, output}}. Default: the latest active output "
           "of the required type, of the active object first"}};
    }
    schema["properties"]["object"] = {
        {"type", "string"},
        {"description",
         "Optional: id of the object to work on; inputs not given are taken from its outputs"}};
    if (std::ranges::any_of(info.outputs, [](const PortInfo& port) {
          return port.type == artifact::kDataset || port.type == artifact::kMesh;
        })) {
      schema["properties"]["object_name"] = {
          {"type", "string"},
          {"description",
           "Optional: name of the object this step creates; default: the file name"}};
    }
    std::string description = info.title + ". " + info.description;
    if (!inputs.empty()) {
      description += " Inputs: " + inputs + ".";
    }
    description += " Runs as a new step of the open project.";
    schema["properties"]["background"] = {
        {"type", "boolean"},
        {"description",
         "Optional: return at once and run in the background; then call wait until done"}};
    methods.push_back({kRunPrefix + info.id, description, schema});
  }
  return methods;
}

Project& Gui::project() {
  if (!project_) {
    throw std::invalid_argument("No project open: call project_create or project_open first");
  }
  return *project_;
}

const Project& Gui::project() const {
  if (!project_) {
    throw std::invalid_argument("No project open: call project_create or project_open first");
  }
  return *project_;
}

Json Gui::status() const {
  Json running =
      running_ ? Json{{"operation", running_operation_}, {"progress", progress_.load()}} : Json();
  if (running_ && !running_preview_.is_null()) {
    running["preview"] = running_preview_;
  }
  if (!project_) {
    return {{"open", false}, {"running", running}};
  }
  Json json = project_->toJson();
  for (std::size_t i = 0; i < project_->steps().size(); ++i) {
    const Step& step = project_->steps()[i];
    json["steps"][i]["active"] = i < project_->cursor();
    json["steps"][i]["size_bytes"] = directorySize(project_->stepDir(step));
    // Older projects stored German titles; show the operation's current one.
    if (const auto operation = registry_.find(step.operation)) {
      json["steps"][i]["title"] = operation->info().title;
    }
  }
  json["objects"] = Json::array();
  for (const ProjectObject& object : project_->objects()) {
    json["objects"].push_back(toJson(object));
  }
  json["results"] = Json::array();
  for (const AnalysisResult& result : project_->results()) {
    json["results"].push_back(toJson(result));
  }
  json["active_object"] = project_->activeObject();
  json["open"] = true;
  json["dir"] = project_->dir().string();
  json["can_undo"] = project_->canUndo();
  json["can_redo"] = project_->canRedo();
  json["running"] = running;
  return json;
}

ArtifactRef Gui::artifactRef(const Json& params, const std::string& type) const {
  if (!params.contains("step")) {
    if (!type.empty()) {
      // The data of the active object first, as new steps take it.
      const std::string active = project().activeObject();
      if (const auto ref = active.empty() ? std::nullopt : project().latest(type, active)) {
        return *ref;
      }
      if (const auto ref = project().latest(type)) {
        return *ref;
      }
      throw std::invalid_argument("The project has no active " + type);
    }
    for (std::size_t i = project().cursor(); i > 0; --i) {
      const Step& step = project().steps()[i - 1];
      if (step.status == "done" && !step.outputs.empty()) {
        return {step.id, step.outputs.begin()->first};
      }
    }
    throw std::invalid_argument("The project has no active outputs");
  }
  const int id = params.at("step").get<int>();
  for (const Step& step : project().steps()) {
    if (step.id != id) {
      continue;
    }
    if (params.contains("output")) {
      return {id, params.at("output").get<std::string>()};
    }
    for (const auto& [name, output_type] : step.output_types) {
      if (type.empty() || output_type == type) {
        return {id, name};
      }
    }
    throw std::invalid_argument("Step " + std::to_string(id) + " has no " +
                                (type.empty() ? std::string("outputs") : type));
  }
  throw std::invalid_argument("No step " + std::to_string(id));
}

std::filesystem::path Gui::artifactPath(const Json& params) const {
  return project().resolve(artifactRef(params, ""));
}

std::shared_ptr<const Dataset> Gui::openDataset(const std::filesystem::path& dir) const {
  const std::scoped_lock lock(view_mutex_);
  return cached(datasets_, dir, kOpenVolumes, [this, &dir] {
    return std::make_shared<const Dataset>(
        Dataset::open(dir, view_cache_, BrickLoading::kOnAccess));
  });
}

std::shared_ptr<const PorosityResult> Gui::openPorosity(const std::filesystem::path& dir) const {
  const std::scoped_lock lock(view_mutex_);
  return cached(porosity_results_, dir, kOpenDatasets,
                [&dir] { return std::make_shared<const PorosityResult>(loadPorosityResult(dir)); });
}

std::shared_ptr<const MaterialVolume> Gui::openMaterials(std::optional<int> materials_step) const {
  if (!materials_step) {
    return nullptr;
  }
  std::filesystem::path dir;
  {
    const std::scoped_lock lock(mutex_);
    dir = project().resolve(artifactRef({{"step", *materials_step}}, artifact::kMaterials));
  }
  return openMaterials(dir);
}

std::shared_ptr<const MaterialVolume> Gui::openMaterials(const std::filesystem::path& dir) const {
  const std::scoped_lock lock(view_mutex_);
  return cached(material_volumes_, dir, kOpenDatasets, [&dir] {
    return std::make_shared<const MaterialVolume>(MaterialVolume::open(dir));
  });
}

SliceImage Gui::sliceTile(std::optional<int> dataset_step, std::optional<int> porosity_step,
                          const SliceRequest& request, std::optional<int> materials_step) const {
  const auto [dataset, porosity] = openView(dataset_step, porosity_step);
  const auto materials = openMaterials(materials_step);
  return readSlice(*dataset, request, porosity.get(), materials.get());
}

VolumePreview Gui::volumePreview(std::optional<int> dataset_step, std::optional<int> porosity_step,
                                 const VolumeRequest& request,
                                 std::optional<int> materials_step) const {
  const auto [dataset, porosity] = openView(dataset_step, porosity_step);
  const auto materials = openMaterials(materials_step);
  return readVolumePreview(*dataset, request, porosity.get(), materials.get());
}

std::shared_ptr<const IndexedMesh> Gui::surfaceMesh(std::optional<int> surface_step,
                                                    std::size_t max_triangles) const {
  std::filesystem::path file;
  {
    const std::scoped_lock lock(mutex_);
    const Json params = surface_step ? Json{{"step", *surface_step}} : Json::object();
    file = project().resolve(artifactRef(params, artifact::kSurface)) / "surface.vss";
  }
  return surfaceMeshOf(file, max_triangles);
}

std::shared_ptr<const IndexedMesh> Gui::surfaceMeshOf(const std::filesystem::path& file,
                                                      std::size_t max_triangles) const {
  // The file time is part of the key: an undone step and its successor share the directory.
  const std::filesystem::path key =
      file.string() + "#" + std::to_string(max_triangles) + "#" + fileStamp(file);
  {
    const std::scoped_lock lock(surface_mutex_);
    for (const auto& [path, mesh] : surface_meshes_) {
      if (path == key) {
        return mesh;
      }
    }
  }
  // Built without a lock: meshing runs in parallel with TBB.
  auto mesh = std::make_shared<const IndexedMesh>(
      surfaceDisplayMesh(SurfaceMask::open(file), max_triangles));
  const std::scoped_lock lock(surface_mutex_);
  surface_meshes_.emplace_back(key, mesh);
  // One per object of a scene: the 3D view shows all of them.
  if (surface_meshes_.size() > kOpenVolumes) {
    surface_meshes_.erase(surface_meshes_.begin());
  }
  return mesh;
}

std::shared_ptr<const Gui::DeviationView> Gui::deviationMesh(
    std::optional<int> comparison_step) const {
  std::filesystem::path dir;
  {
    const std::scoped_lock lock(mutex_);
    const Json params = comparison_step ? Json{{"step", *comparison_step}} : Json::object();
    dir = project().resolve(artifactRef(params, artifact::kComparison));
  }
  const std::filesystem::path key = dir.string() + "#" + fileStamp(dir / "deviation.ply");
  {
    const std::scoped_lock lock(surface_mutex_);
    for (const auto& [path, view] : deviation_meshes_) {
      if (path == key) {
        return view;
      }
    }
  }
  std::ifstream in(dir / "compare.json");
  const Json info = Json::parse(in);
  DeviationMesh read = readDeviationPly(dir / "deviation.ply");
  const VoxelSize v = readVoxelSize(info);
  for (auto& p : read.mesh.points) {
    for (std::size_t k = 0; k < 3; ++k) {
      p[k] = static_cast<float>(p[k] / v[k]);
    }
  }
  auto view = std::make_shared<DeviationView>();
  view->mesh = std::move(read.mesh);
  view->deviation_mm = std::move(read.deviation_mm);
  view->tolerance_mm = info.at("tolerance_mm").get<double>();
  view->range_mm = info.at("deviation").at("range_mm").get<double>();
  if (const auto deformation = info.find("deformation"); deformation != info.end()) {
    view->displacement_mm = std::move(read.displacement_mm);
    view->displacement_range_mm = deformation->at("displacement").at("range_mm").get<double>();
  }
  const std::scoped_lock lock(surface_mutex_);
  deviation_meshes_.emplace_back(key, view);
  if (deviation_meshes_.size() > kOpenDatasets) {
    deviation_meshes_.erase(deviation_meshes_.begin());
  }
  return view;
}

std::shared_ptr<const IndexedMesh> Gui::stlMesh(const std::filesystem::path& file) const {
  const std::filesystem::path key = file.string() + "#stl#" + fileStamp(file);
  {
    const std::scoped_lock lock(surface_mutex_);
    for (const auto& [path, mesh] : surface_meshes_) {
      if (path == key) {
        return mesh;
      }
    }
  }
  auto mesh = std::make_shared<const IndexedMesh>(indexedMesh(readStl(file)));
  const std::scoped_lock lock(surface_mutex_);
  surface_meshes_.emplace_back(key, mesh);
  if (surface_meshes_.size() > kOpenVolumes) {
    surface_meshes_.erase(surface_meshes_.begin());
  }
  return mesh;
}

Gui::ObjectSource Gui::objectSource(const std::string& id) const {
  ObjectSource source;
  source.object = project().object(id);
  source.source = project().resolve(source.object.source);
  if (source.object.kind == kVolumeObject) {
    if (const auto surface = project().latest(artifact::kSurface, id)) {
      source.surface_file = project().resolve(*surface) / "surface.vss";
    }
    if (const auto materials = project().latest(artifact::kMaterials, id)) {
      source.materials_dir = project().resolve(*materials);
    }
  }
  return source;
}

RigidTransform Gui::datasetPose(const ArtifactRef& dataset) const {
  for (const Step& step : project().steps()) {
    if (step.id == dataset.step) {
      return step.object.empty() ? RigidTransform() : project().object(step.object).pose;
    }
  }
  return {};
}

Gui::ObjectShape Gui::objectShape(const std::string& id, std::size_t max_triangles) const {
  ObjectSource source;
  {
    const std::scoped_lock lock(mutex_);
    source = objectSource(id);
  }
  return shapeOf(source, max_triangles);
}

Gui::ObjectShape Gui::shapeOf(const ObjectSource& source, std::size_t max_triangles) const {
  ObjectShape shape;
  shape.pose = source.object.pose;
  if (source.object.kind == kMeshObject) {
    shape.shape = "mesh";
    shape.mesh = stlMesh(source.source);
    return shape;
  }
  const DatasetInfo& info = openDataset(source.source)->info();
  for (std::size_t k = 0; k < 3; ++k) {
    shape.scale[k] = info.voxel_size[k];
  }
  if (!source.surface_file.empty()) {
    shape.shape = "surface";
    shape.mesh = surfaceMeshOf(source.surface_file, max_triangles);
    return shape;
  }
  // The box the voxels fill: voxel centres lie on whole indices.
  std::array<double, 3> size{};
  std::array<double, 3> center{};
  for (std::size_t k = 0; k < 3; ++k) {
    size[k] = static_cast<double>(info.dims[k]);
    center[k] = 0.5 * (size[k] - 1.0);
  }
  shape.shape = "box";
  shape.mesh = std::make_shared<const IndexedMesh>(indexedMesh(boxMesh(size, center)));
  return shape;
}

PlaneImage Gui::objectSliceTile(std::optional<int> dataset_step, const SliceRequest& request,
                                const std::string& object) const {
  std::filesystem::path base_dir;
  RigidTransform base_pose;
  ObjectSource source;
  {
    const std::scoped_lock lock(mutex_);
    const Json params = dataset_step ? Json{{"step", *dataset_step}} : Json::object();
    const ArtifactRef ref = artifactRef(params, artifact::kDataset);
    base_dir = project().resolve(ref);
    base_pose = datasetPose(ref);
    source = objectSource(object);
  }
  return objectPlane(*openDataset(base_dir), base_pose, request, source);
}

std::vector<float> Gui::objectCutLines(std::optional<int> dataset_step, int axis,
                                       std::int64_t index, const std::string& object) const {
  std::filesystem::path base_dir;
  RigidTransform base_pose;
  ObjectSource source;
  {
    const std::scoped_lock lock(mutex_);
    const Json params = dataset_step ? Json{{"step", *dataset_step}} : Json::object();
    const ArtifactRef ref = artifactRef(params, artifact::kDataset);
    base_dir = project().resolve(ref);
    base_pose = datasetPose(ref);
    source = objectSource(object);
  }
  return objectCuts(*openDataset(base_dir), base_pose, axis, index, source);
}

PlaneImage Gui::objectPlane(const Dataset& base, const RigidTransform& base_pose,
                            const SliceRequest& request, const ObjectSource& object) const {
  if (object.object.kind != kVolumeObject) {
    throw std::invalid_argument("Object '" + object.object.name +
                                "' is not a volume; meshes show as cut lines");
  }
  const DatasetInfo& info = base.info();
  const auto [u, v] = sliceAxes(request.axis);
  const auto n = static_cast<std::size_t>(request.axis);
  const auto ua = static_cast<std::size_t>(u);
  const auto va = static_cast<std::size_t>(v);
  if (request.level < 0 || static_cast<std::size_t>(request.level) >= info.levels.size()) {
    throw std::invalid_argument("Level " + std::to_string(request.level) + " does not exist");
  }
  const auto other = openDataset(object.source);
  // Base object coordinates (level-0 index times pitch) to those of the other object.
  const RigidTransform to_other = object.object.pose.inverse().after(base_pose);
  const double scale = std::ldexp(1.0, request.level);
  const VoxelSize& pitch = info.voxel_size;
  // Level voxel i covers level-0 indices [i * 2^level, (i + 1) * 2^level): its centre lies at
  // (i + 0.5) * 2^level - 0.5.
  std::array<double, 3> first{};
  first[n] = static_cast<double>(request.index) * pitch[n];
  first[ua] = ((static_cast<double>(request.origin[0]) + 0.5) * scale - 0.5) * pitch[ua];
  first[va] = ((static_cast<double>(request.origin[1]) + 0.5) * scale - 0.5) * pitch[va];
  std::array<double, 3> step_u{};
  std::array<double, 3> step_v{};
  step_u[ua] = scale * pitch[ua];
  step_v[va] = scale * pitch[va];
  PlaneRequest plane;
  plane.origin_mm = to_other.apply(first);
  plane.du_mm = to_other.rotate(step_u);
  plane.dv_mm = to_other.rotate(step_v);
  plane.width = request.size[0];
  plane.height = request.size[1];
  plane.level = levelForPixel(other->info(), scale * std::min(pitch[ua], pitch[va]));
  plane.linear = true;
  return samplePlane(*other, plane);
}

std::vector<float> Gui::objectCuts(const Dataset& base, const RigidTransform& base_pose, int axis,
                                   std::int64_t index, const ObjectSource& object) const {
  const auto shape = shapeOf(object, kCutTriangles);
  const RigidTransform to_base = base_pose.inverse().after(shape.pose);
  const VoxelSize& pitch = base.info().voxel_size;
  std::vector<std::array<float, 3>> points;
  points.reserve(shape.mesh->points.size());
  for (const auto& p : shape.mesh->points) {
    const auto q =
        to_base.apply({p[0] * shape.scale[0], p[1] * shape.scale[1], p[2] * shape.scale[2]});
    points.push_back({static_cast<float>(q[0] / pitch[0]), static_cast<float>(q[1] / pitch[1]),
                      static_cast<float>(q[2] / pitch[2])});
  }
  return cutMesh(points, shape.mesh->triangles, axis, static_cast<double>(index));
}

Json Gui::histogram(const Json& params) const {
  ArtifactRef ref;
  std::filesystem::path dir;
  {
    const std::scoped_lock lock(mutex_);
    ref = artifactRef(params, artifact::kDataset);
    dir = project().resolve(ref);
  }
  // Counted without the lock, like view_objects: a full level of a large scan takes a while.
  const auto dataset = openDataset(dir);
  const DatasetInfo& info = dataset->info();
  constexpr std::int64_t kDefaultVoxels = std::int64_t{1} << 27;
  int level = 0;
  if (params.contains("level")) {
    level = params.at("level").get<int>();
    if (level >= static_cast<int>(info.levels.size())) {
      throw std::invalid_argument("The dataset has levels 0 to " +
                                  std::to_string(info.levels.size() - 1));
    }
  } else {
    const auto voxels = [&](int l) {
      const auto& d = info.levels[static_cast<std::size_t>(l)].dims;
      return d[0] * d[1] * d[2];
    };
    while (level + 1 < static_cast<int>(info.levels.size()) && voxels(level) > kDefaultVoxels) {
      ++level;
    }
  }
  const std::vector<std::uint64_t> full = datasetHistogram(*dataset, level);
  const MaterialSuggestion suggestion =
      suggestMaterials(full, info.threshold, params.at("max_materials").get<int>());
  std::size_t low = 0;
  while (low + 1 < full.size() && full[low] == 0) {
    ++low;
  }
  std::size_t high = full.size() - 1;
  while (high > low && full[high] == 0) {
    --high;
  }
  const auto bins = params.at("bins").get<std::size_t>();
  const double bin_width = static_cast<double>(high + 1 - low) / static_cast<double>(bins);
  std::vector<std::uint64_t> counts(bins, 0);
  std::uint64_t total = 0;
  for (std::size_t v = low; v <= high; ++v) {
    counts[std::min(bins - 1,
                    static_cast<std::size_t>(static_cast<double>(v - low) / bin_width))] += full[v];
    total += full[v];
  }
  std::vector<float> starts{info.threshold};
  starts.insert(starts.end(), suggestion.thresholds.begin(), suggestion.thresholds.end());
  Json mixture = Json::array();
  for (const MixtureComponent& component : suggestion.components) {
    mixture.push_back(
        {{"mean", component.mean}, {"sigma", component.sigma}, {"share", component.weight}});
  }
  const std::vector<std::uint8_t> png =
      histogramPng(counts, static_cast<double>(low), bin_width, info.threshold, starts);
  Json json = {{"step", ref.step},
               {"output", ref.output},
               {"level", level},
               {"voxels", total},
               {"threshold", info.threshold},
               {"air_level", info.air_level},
               {"first", low},
               {"bin_width", bin_width},
               {"counts", counts},
               {"suggested_materials", std::max<std::size_t>(1, suggestion.peaks.size())},
               {"clear_peaks", suggestion.clear_peaks},
               {"material_peaks", suggestion.peaks},
               {"material_thresholds", suggestion.thresholds},
               {"mixture", mixture},
               {"ambiguous_fraction", suggestion.ambiguous_fraction},
               {"image", {{"mime_type", "image/png"}, {"base64", base64(png)}}}};
  if (!info.value_mapping.isIdentity()) {
    json["value_mapping"] = {{"offset", info.value_mapping.offset},
                             {"scale", info.value_mapping.scale}};
  }
  return json;
}

Json Gui::viewObjects(const Json& params) const {
  std::vector<std::string> all;
  {
    const std::scoped_lock lock(mutex_);
    for (const ProjectObject& object : project().objects()) {
      all.push_back(object.id);
    }
  }
  const auto ids =
      params.contains("objects") ? params.at("objects").get<std::vector<std::string>>() : all;
  if (ids.empty()) {
    throw std::invalid_argument("The project has no objects");
  }
  const auto budget = params.at("max_triangles").get<std::size_t>();
  IndexedMesh scene;
  std::vector<std::array<std::uint8_t, 3>> colors;
  constexpr double kInf = std::numeric_limits<double>::infinity();
  std::array<double, 3> low{kInf, kInf, kInf};
  std::array<double, 3> high{-kInf, -kInf, -kInf};
  Json listed = Json::array();
  const auto add = [&](const IndexedMesh& mesh, const ObjectShape& shape,
                       const std::array<std::uint8_t, 3>& color) {
    const auto offset = static_cast<std::uint32_t>(scene.points.size());
    for (const auto& p : mesh.points) {
      const auto q =
          shape.pose.apply({p[0] * shape.scale[0], p[1] * shape.scale[1], p[2] * shape.scale[2]});
      scene.points.push_back(
          {static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2])});
      for (std::size_t k = 0; k < 3; ++k) {
        low[k] = std::min(low[k], q[k]);
        high[k] = std::max(high[k], q[k]);
      }
    }
    colors.insert(colors.end(), mesh.points.size(), color);
    for (const auto& t : mesh.triangles) {
      scene.triangles.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
    }
  };
  const bool by_material = params.at("materials").get<bool>();
  const auto chosen = params.contains("material_ids")
                          ? params.at("material_ids").get<std::vector<int>>()
                          : std::vector<int>{};
  for (const std::string& id : ids) {
    ObjectSource source;
    {
      const std::scoped_lock lock(mutex_);
      source = objectSource(id);
    }
    if (by_material && !source.materials_dir.empty()) {
      // The surface of each material, in its colour.
      const auto volume = openMaterials(source.materials_dir);
      std::vector<const Material*> shown;
      for (const Material& material : volume->info().materials) {
        if (chosen.empty() || std::ranges::find(chosen, material.id) != chosen.end()) {
          shown.push_back(&material);
        }
      }
      ObjectShape shape;
      shape.pose = source.object.pose;
      for (std::size_t k = 0; k < 3; ++k) {
        shape.scale[k] = volume->info().voxel_size[k];
      }
      Json materials = Json::array();
      std::size_t triangles = 0;
      for (const Material* material : shown) {
        const IndexedMesh mesh =
            materialMesh(*volume, material->id, std::max<std::size_t>(1, budget / shown.size()));
        add(mesh, shape, material->color);
        triangles += mesh.triangles.size();
        materials.push_back({{"id", material->id},
                             {"name", material->name},
                             {"color", material->color},
                             {"triangles", mesh.triangles.size()}});
      }
      listed.push_back(
          {{"id", id}, {"shape", "materials"}, {"materials", materials}, {"triangles", triangles}});
      continue;
    }
    const ObjectShape shape = shapeOf(source, budget);
    const auto index = static_cast<std::size_t>(std::ranges::find(all, id) - all.begin());
    const auto color = objectColor(source.object, index);
    add(*shape.mesh, shape, color);
    listed.push_back({{"id", id},
                      {"shape", shape.shape},
                      {"color", color},
                      {"triangles", shape.mesh->triangles.size()}});
  }
  if (params.contains("cut_axis")) {
    const auto axis = static_cast<std::size_t>(params.at("cut_axis").get<std::string>()[0] - 'x');
    const double position = params.contains("cut_mm") ? params.at("cut_mm").get<double>()
                                                      : 0.5 * (low[axis] + high[axis]);
    cutMesh(scene, colors, axis, static_cast<float>(position),
            params.at("cut_keep").get<std::string>() == "below");
  }
  if (scene.triangles.empty()) {
    throw std::invalid_argument(
        "Nothing to show: the chosen materials have no voxels, or the cut leaves nothing");
  }
  RenderScene render_scene;
  render_scene.mesh = &scene;
  render_scene.vertex_colors = std::move(colors);
  RenderView view;
  view.azimuth_degrees = params.at("azimuth_deg").get<double>();
  view.elevation_degrees = params.at("elevation_deg").get<double>();
  view.width = params.at("width").get<int>();
  view.height = params.at("height").get<int>();
  view.supersampling = 2;
  const RenderImage image = render(render_scene, view);
  const auto png =
      detail::encodePng(static_cast<std::uint32_t>(image.width),
                        static_cast<std::uint32_t>(image.height), image.channels, image.pixels);
  return {{"objects", listed},
          {"triangles_shown", scene.triangles.size()},
          {"bounds_min_mm", low},
          {"bounds_max_mm", high},
          {"width", image.width},
          {"height", image.height},
          {"image", {{"mime_type", "image/png"}, {"base64", base64(png)}}}};
}

std::pair<std::shared_ptr<const Dataset>, std::shared_ptr<const PorosityResult>> Gui::openView(
    std::optional<int> dataset_step, std::optional<int> porosity_step) const {
  std::filesystem::path dataset_dir;
  std::filesystem::path porosity_dir;
  {
    const std::scoped_lock lock(mutex_);
    const Json dataset_params = dataset_step ? Json{{"step", *dataset_step}} : Json::object();
    dataset_dir = project().resolve(artifactRef(dataset_params, artifact::kDataset));
    if (porosity_step) {
      porosity_dir =
          project().resolve(artifactRef({{"step", *porosity_step}}, artifact::kPorosity));
    }
  }
  return {openDataset(dataset_dir), porosity_dir.empty() ? nullptr : openPorosity(porosity_dir)};
}

Json Gui::drawObjects(const Dataset& base, const ArtifactRef& base_ref, const SliceRequest& request,
                      const Json& ids, std::vector<std::uint8_t>& rgb) const {
  const auto width = request.size[0];
  const auto height = request.size[1];
  const RigidTransform base_pose = datasetPose(base_ref);
  std::vector<std::string> all;
  for (const ProjectObject& object : project().objects()) {
    all.push_back(object.id);
  }
  const double scale = std::ldexp(1.0, request.level);
  Json shown = Json::array();
  for (const std::string& id : ids.get<std::vector<std::string>>()) {
    const ObjectSource source = objectSource(id);
    const auto index = static_cast<std::size_t>(std::ranges::find(all, id) - all.begin());
    const auto color = objectColor(source.object, index);
    const auto blend = [&rgb, &color](std::size_t pixel, float weight, float shade) {
      for (std::size_t c = 0; c < 3; ++c) {
        auto& channel = rgb[pixel * 3 + c];
        channel =
            static_cast<std::uint8_t>(std::lround((1.0F - weight) * static_cast<float>(channel) +
                                                  weight * shade * static_cast<float>(color[c])));
      }
    };
    bool blended = false;
    if (source.object.kind == kVolumeObject) {
      // The material of the volume in the colour of its object, brighter where it is denser.
      const PlaneImage plane = objectPlane(base, base_pose, request, source);
      std::vector<float> values;
      for (std::size_t i = 0; i < plane.grey.size(); ++i) {
        if (plane.inside[i] == kPlaneMaterial) {
          values.push_back(plane.grey[i]);
        }
      }
      if (!values.empty()) {
        std::ranges::sort(values);
        const float low = values[values.size() / 200];
        const float high = std::max(values[values.size() - 1 - values.size() / 200], low + 1.0F);
        // The edge fades over a tenth of the window around the threshold, so a turned volume
        // shows its interpolated surface instead of a staircase of whole voxels.
        const float ramp = 0.1F * (high - low);
        for (std::size_t i = 0; i < plane.grey.size(); ++i) {
          if (plane.inside[i] != kPlaneOutside) {
            const float cover =
                std::clamp((plane.grey[i] - plane.threshold + ramp) / (2.0F * ramp), 0.0F, 1.0F);
            const float t = std::clamp((plane.grey[i] - low) / (high - low), 0.0F, 1.0F);
            if (cover > 0.0F) {
              blend(i, 0.5F * cover, 0.3F + 0.7F * t);
            }
          }
        }
        blended = true;
      }
    }
    const std::vector<float> cuts =
        objectCuts(base, base_pose, request.axis, request.index, source);
    // Level-0 index c lies at pixel (c + 0.5) / 2^level - 0.5 - origin.
    const auto pixel = [&](float c, std::int64_t origin) {
      return (static_cast<double>(c) + 0.5) / scale - 0.5 - static_cast<double>(origin);
    };
    for (std::size_t s = 0; s + 3 < cuts.size(); s += 4) {
      const double x0 = pixel(cuts[s], request.origin[0]);
      const double y0 = pixel(cuts[s + 1], request.origin[1]);
      const double x1 = pixel(cuts[s + 2], request.origin[0]);
      const double y1 = pixel(cuts[s + 3], request.origin[1]);
      const auto steps =
          static_cast<int>(std::ceil(std::max(std::abs(x1 - x0), std::abs(y1 - y0))));
      for (int k = 0; k <= steps; ++k) {
        const double t = steps == 0 ? 0.0 : static_cast<double>(k) / steps;
        const auto x = static_cast<std::int64_t>(std::lround(x0 + t * (x1 - x0)));
        const auto y = static_cast<std::int64_t>(std::lround(y0 + t * (y1 - y0)));
        if (x >= 0 && x < width && y >= 0 && y < height) {
          blend(static_cast<std::size_t>(y * width + x), 1.0F, 1.0F);
        }
      }
    }
    shown.push_back({{"id", id},
                     {"name", source.object.name},
                     {"color", color},
                     {"volume_blended", blended},
                     {"cut_segments", cuts.size() / 4}});
  }
  return shown;
}

Json Gui::importPreview(const Json& params) const {
  if (!params.contains("step") && running_) {
    if (running_preview_.is_null()) {
      throw std::invalid_argument("'" + running_operation_ + "' has no preview yet");
    }
    Json json = running_preview_;
    json["running"] = true;
    json["image"] = {{"mime_type", "image/png"}, {"base64", base64(running_preview_png_)}};
    return json;
  }
  const Project& open = project();
  const auto has_preview = [&open](const Step& step) {
    return std::filesystem::exists(open.stepDir(step) / "preview.json");
  };
  const Step* found = nullptr;
  if (params.contains("step")) {
    const int id = params.at("step").get<int>();
    for (const Step& step : open.steps()) {
      if (step.id == id) {
        found = &step;
      }
    }
    if (found == nullptr || !has_preview(*found)) {
      throw std::invalid_argument("Step " + std::to_string(id) + " has no preview");
    }
  } else {
    for (std::size_t i = open.cursor(); i-- > 0 && found == nullptr;) {
      if (has_preview(open.steps()[i])) {
        found = &open.steps()[i];
      }
    }
    if (found == nullptr) {
      throw std::invalid_argument("No import is running and no step has a preview");
    }
  }
  const auto dir = open.stepDir(*found);
  std::ifstream summary(dir / "preview.json");
  Json json = Json::parse(summary);
  std::ifstream in(dir / "preview.png", std::ios::binary);
  const std::vector<std::uint8_t> png{std::istreambuf_iterator<char>(in),
                                      std::istreambuf_iterator<char>()};
  json["step"] = found->id;
  json["running"] = false;
  json["image"] = {{"mime_type", "image/png"}, {"base64", base64(png)}};
  return json;
}

Json Gui::viewSlice(const Json& params) const {
  const ArtifactRef dataset_ref = artifactRef(params, artifact::kDataset);
  const auto dataset_dir = project().resolve(dataset_ref);
  const auto dataset = openDataset(dataset_dir);
  const DatasetInfo& info = dataset->info();

  // Overlay: the given porosity and segmentation steps, or the latest ones computed from this
  // dataset.
  std::shared_ptr<const PorosityResult> porosity;
  std::optional<int> porosity_step;
  std::shared_ptr<const MaterialVolume> materials;
  std::optional<int> materials_step;
  const auto latest_step_of =
      [&](const std::vector<std::string>& operations) -> std::optional<int> {
    for (std::size_t i = project().cursor(); i > 0; --i) {
      const Step& step = project().steps()[i - 1];
      const auto input = step.inputs.find("dataset");
      if (step.status == "done" &&
          std::find(operations.begin(), operations.end(), step.operation) != operations.end() &&
          input != step.inputs.end() && project().resolve(input->second) == dataset_dir) {
        return step.id;
      }
    }
    return std::nullopt;
  };
  if (params.at("overlay").get<bool>()) {
    if (params.contains("porosity_step")) {
      porosity_step = params.at("porosity_step").get<int>();
    } else {
      porosity_step = latest_step_of({"porosity"});
    }
    if (porosity_step) {
      porosity = openPorosity(
          project().resolve(artifactRef({{"step", *porosity_step}}, artifact::kPorosity)));
    }
    if (params.contains("materials_step")) {
      materials_step = params.at("materials_step").get<int>();
    } else {
      materials_step = latest_step_of({"segment_materials", "segment_model"});
    }
    if (materials_step) {
      materials = openMaterials(
          project().resolve(artifactRef({{"step", *materials_step}}, artifact::kMaterials)));
    }
  }

  SliceRequest request;
  request.axis = axisIndex(params.at("axis").get<std::string>());
  const auto normal = static_cast<std::size_t>(request.axis);
  request.index =
      params.contains("index") ? params.at("index").get<std::int64_t>() : info.dims[normal] / 2;
  const auto [u, v] = sliceAxes(request.axis);
  const std::int64_t max_pixels = params.at("max_pixels").get<std::int64_t>();
  const auto& levels = info.levels;
  request.level = static_cast<int>(levels.size()) - 1;
  for (std::size_t l = 0; l < levels.size(); ++l) {
    const auto& dims = levels[l].dims;
    if (std::max(dims[static_cast<std::size_t>(u)], dims[static_cast<std::size_t>(v)]) <=
        max_pixels) {
      request.level = static_cast<int>(l);
      break;
    }
  }
  const auto& dims = levels[static_cast<std::size_t>(request.level)].dims;
  request.size = {dims[static_cast<std::size_t>(u)], dims[static_cast<std::size_t>(v)]};
  std::vector<float> preview;
  if (params.contains("preview_thresholds")) {
    preview = params.at("preview_thresholds").get<std::vector<float>>();
    if (!std::ranges::is_sorted(preview)) {
      throw std::invalid_argument("preview_thresholds must ascend");
    }
    materials.reset();
    materials_step.reset();
  }
  const SliceImage image = readSlice(*dataset, request, porosity.get(), materials.get());

  float low = 0.0F;
  float high = 0.0F;
  {
    std::vector<float> sorted = image.grey;
    std::sort(sorted.begin(), sorted.end());
    const auto at = [&sorted](double fraction) {
      return sorted[static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1))];
    };
    low = params.contains("window_min") ? params.at("window_min").get<float>() : at(0.005);
    high = params.contains("window_max") ? params.at("window_max").get<float>() : at(0.995);
    if (high <= low) {
      high = low + 1.0F;
    }
  }
  std::vector<std::uint8_t> rgb(image.grey.size() * 3);
  for (std::size_t i = 0; i < image.grey.size(); ++i) {
    const float t = std::clamp((image.grey[i] - low) / (high - low), 0.0F, 1.0F);
    const auto grey = static_cast<std::uint8_t>(std::lround(t * 255.0F));
    std::array<std::uint8_t, 3> color{grey, grey, grey};
    // Tinted like in the browser view: pores red, zones yellow.
    const auto g = static_cast<float>(grey);
    const auto channel = [](float value) { return static_cast<std::uint8_t>(value); };
    if (image.overlay[i] == static_cast<std::uint8_t>(SliceOverlay::kPore)) {
      color = {channel(0.35F * g + 165.0F), channel(0.35F * g + 25.0F), channel(0.35F * g + 25.0F)};
    } else if (image.overlay[i] == static_cast<std::uint8_t>(SliceOverlay::kZone)) {
      color = {channel(0.5F * g + 125.0F), channel(0.5F * g + 100.0F), channel(0.4F * g)};
    } else if (!params.contains("preview_thresholds") || image.grey[i] <= info.threshold) {
      if (materials && image.overlay[i] > static_cast<std::uint8_t>(SliceOverlay::kMaterial)) {
        // Materials in their colour, half covering the grey value.
        const auto id = static_cast<std::size_t>(
            image.overlay[i] - static_cast<std::uint8_t>(SliceOverlay::kMaterial));
        const auto& tint = materials->info().materials.at(id - 1).color;
        for (std::size_t c = 0; c < 3; ++c) {
          color[c] = channel(0.5F * g + 0.5F * static_cast<float>(tint[c]));
        }
      }
    } else {
      // The class the preview thresholds give, in the colour a segmentation would use.
      const auto id = 1 + std::ranges::upper_bound(preview, image.grey[i]) - preview.begin();
      const auto tint = detail::materialColor(static_cast<int>(id));
      for (std::size_t c = 0; c < 3; ++c) {
        color[c] = channel(0.5F * g + 0.5F * static_cast<float>(tint[c]));
      }
    }
    std::copy(color.begin(), color.end(), rgb.begin() + static_cast<std::ptrdiff_t>(i * 3));
  }
  Json shown = Json::array();
  if (params.contains("objects")) {
    shown = drawObjects(*dataset, dataset_ref, request, params.at("objects"), rgb);
  }
  const auto png = detail::encodePng(static_cast<std::uint32_t>(image.width),
                                     static_cast<std::uint32_t>(image.height), 3, rgb);
  constexpr std::array<const char*, 3> kNames = {"x", "y", "z"};
  Json result = {
      {"step", dataset_ref.step},
      {"axis", params.at("axis")},
      {"index", request.index},
      {"level", request.level},
      {"width", image.width},
      {"height", image.height},
      {"right", kNames[static_cast<std::size_t>(u)]},
      {"down", kNames[static_cast<std::size_t>(v)]},
      {"pixel_size_mm",
       {levels[static_cast<std::size_t>(request.level)].voxel_size[static_cast<std::size_t>(u)],
        levels[static_cast<std::size_t>(request.level)].voxel_size[static_cast<std::size_t>(v)]}},
      {"window", {low, high}},
      {"image", {{"mime_type", "image/png"}, {"base64", base64(png)}}}};
  result["objects"] = shown;
  result["porosity_step"] = porosity_step ? Json(*porosity_step) : Json();
  result["materials_step"] = materials_step ? Json(*materials_step) : Json();
  if (materials) {
    Json legend = Json::array();
    for (const Material& material : materials->info().materials) {
      legend.push_back({{"id", material.id}, {"name", material.name}, {"color", material.color}});
    }
    result["materials"] = legend;
  }
  if (params.contains("preview_thresholds")) {
    Json legend = Json::array();
    for (std::size_t m = 0; m <= preview.size(); ++m) {
      const int id = static_cast<int>(m) + 1;
      legend.push_back({{"id", id},
                        {"lower", m == 0 ? info.threshold : preview[m - 1]},
                        {"color", detail::materialColor(id)}});
    }
    result["preview"] = legend;
  }
  return result;
}

std::filesystem::path Gui::outputFile(int step, const std::string& output,
                                      const std::string& file) const {
  const std::scoped_lock lock(mutex_);
  const auto root = project().resolve({step, output});
  const auto path = file.empty() ? root : insideOf(root, file);
  if (!std::filesystem::is_regular_file(path)) {
    throw std::invalid_argument("Not a file: " + path.string());
  }
  return path;
}

std::filesystem::path Gui::viewImage(int id) const {
  const std::scoped_lock lock(mutex_);
  if (!project().savedView(id).has_image) {
    throw std::invalid_argument("View " + std::to_string(id) + " has no picture");
  }
  return project().viewImage(id);
}

Json Gui::runOperation(const std::string& operation, Json params,
                       const std::function<void(double)>& progress) {
  bool background = false;
  if (params.contains("background")) {
    if (!params.at("background").is_boolean()) {
      throw std::invalid_argument("'background' must be true or false");
    }
    background = params.at("background").get<bool>();
    params.erase("background");
  }
  std::vector<RunRequest> requests;
  requests.push_back({operation, std::move(params)});
  return runSequence(std::move(requests), background, false, progress);
}

Gui::PreparedRun Gui::prepareRun(RunRequest request) {
  PreparedRun run;
  run.operation = std::move(request.operation);
  if (!registry_.find(run.operation)) {
    throw std::invalid_argument("Unknown operation '" + run.operation + "'");
  }
  Json params = std::move(request.params);
  if (!params.is_object()) {
    throw std::invalid_argument("The parameters of '" + run.operation + "' must be an object");
  }
  if (params.contains("inputs")) {
    if (!params.at("inputs").is_object()) {
      throw std::invalid_argument("'inputs' must be an object");
    }
    for (const auto& [name, ref] : params.at("inputs").items()) {
      if (!ref.is_object() || !ref.contains("step") || !ref.at("step").is_number_integer()) {
        throw std::invalid_argument("Input '" + name + "' needs {step, output}");
      }
      run.inputs[name] = artifactRef(ref, "");
    }
    params.erase("inputs");
  }
  for (const auto& [key, field] :
       {std::pair{"object", &StepTarget::object}, std::pair{"object_name", &StepTarget::name}}) {
    if (params.contains(key)) {
      if (!params.at(key).is_string()) {
        throw std::invalid_argument(std::string("'") + key + "' must be a string");
      }
      run.target.*field = params.at(key).get<std::string>();
      params.erase(key);
    }
  }
  run.params = std::move(params);
  run.working = project();
  return run;
}

Json Gui::runSequence(std::vector<RunRequest> requests, bool background, bool pipeline,
                      const std::function<void(double)>& progress) {
  // The operations run on a copy of the project without holding the lock, so status, file and
  // view requests are answered while they run. Changes to the project wait until each is done.
  if (requests.empty()) {
    throw std::invalid_argument("No operation to run");
  }
  PreparedRun first;
  std::thread finished;
  {
    const std::scoped_lock lock(mutex_);
    if (running_) {
      throw std::invalid_argument("Operation '" + running_operation_ +
                                  "' is still running; wait for it or cancel it");
    }
    first = prepareRun(std::move(requests.front()));
    requests.erase(requests.begin());
    running_ = true;
    running_operation_ = first.operation;
    running_preview_ = nullptr;
    running_preview_png_.clear();
    progress_ = 0.0;
    cancel_ = false;
    // The previous background thread has finished its operation; it is joined below, unlocked.
    finished = std::move(background_);
  }
  if (finished.joinable()) {
    finished.join();
  }
  const std::string operation = pipeline ? "pipeline" : first.operation;
  if (!background) {
    return executeSequence(std::move(first), std::move(requests), progress, pipeline);
  }
  // Under the lock, so the thread cannot finish before background_ holds it.
  const std::scoped_lock lock(mutex_);
  background_result_ = nullptr;
  background_ = std::thread([this, operation, pipeline, first = std::move(first),
                             requests = std::move(requests)]() mutable {
    Json result;
    try {
      result = executeSequence(std::move(first), std::move(requests), {}, pipeline);
    } catch (const std::exception& error) {
      result = {{"operation", operation}, {"status", "failed"}, {"error", error.what()}};
    }
    const std::scoped_lock result_lock(mutex_);
    background_result_ = std::move(result);
  });
  return {{"background", true},
          {"operation", operation},
          {"message", "Started in the background; call wait for its step, cancel to stop it"}};
}

Json Gui::executeSequence(PreparedRun first, std::vector<RunRequest> rest,
                          const std::function<void(double)>& progress, bool pipeline) {
  std::deque<RunRequest> queue(std::make_move_iterator(rest.begin()),
                               std::make_move_iterator(rest.end()));
  Json steps = Json::array();
  PreparedRun run = std::move(first);
  const auto failed = [&](const std::string& operation, const std::string& error) {
    return Json{
        {"status", "failed"}, {"failed_operation", operation}, {"error", error}, {"steps", steps}};
  };
  for (;;) {
    const auto total = static_cast<double>(steps.size() + 1 + queue.size());
    run.offset = static_cast<double>(steps.size()) / total;
    run.share = 1.0 / total;
    try {
      steps.push_back(executeRun(run, progress));
    } catch (const std::exception& error) {
      if (!pipeline || steps.empty()) {
        throw;
      }
      return failed(run.operation, error.what());
    }
    std::string next_error;
    std::string next_operation;
    bool more_to_run = false;
    {
      const std::scoped_lock lock(mutex_);
      std::vector<RunRequest> more = followUps(steps.back());
      queue.insert(queue.begin(), std::make_move_iterator(more.begin()),
                   std::make_move_iterator(more.end()));
      if (!queue.empty()) {
        next_operation = queue.front().operation;
        if (cancel_) {
          next_error = "Cancelled before '" + next_operation + "'";
        } else {
          try {
            run = prepareRun(std::move(queue.front()));
            queue.pop_front();
            running_operation_ = run.operation;
            running_preview_ = nullptr;
            running_preview_png_.clear();
            more_to_run = true;
          } catch (const std::exception& error) {
            next_error = "'" + next_operation + "': " + error.what();
          }
        }
      }
    }
    if (more_to_run) {
      continue;
    }
    if (next_error.empty()) {
      break;
    }
    releaseRun();
    if (!pipeline) {
      throw std::invalid_argument(next_error);
    }
    return failed(next_operation, next_error);
  }
  releaseRun();
  if (pipeline) {
    return {{"status", "done"}, {"steps", steps}};
  }
  Json result = steps.at(0);
  if (steps.size() > 1) {
    result["followed_by"] = Json(steps.begin() + 1, steps.end());
  }
  return result;
}
void Gui::releaseRun() {
  {
    const std::scoped_lock lock(mutex_);
    running_ = false;
    running_operation_.clear();
    running_preview_ = nullptr;
    running_preview_png_.clear();
  }
  idle_.notify_all();
}

std::vector<Gui::RunRequest> Gui::followUps(const Json& step) const {
  std::vector<RunRequest> requests;
  if (step.at("operation") != "material_meshes" || step.at("status") != "done" ||
      !step.at("params").value("add_objects", true)) {
    return requests;
  }
  // Each material mesh becomes an object at the pose of the segmented volume.
  const std::string& owner = step.value("object", "");
  const auto& objects = project().objects();
  const auto object = std::ranges::find(objects, owner, &ProjectObject::id);
  const std::filesystem::path dir = project().resolve({step.at("id").get<int>(), "meshes"});
  for (const Json& mesh : step.at("summary").at("meshes")) {
    Json params = {{"path", (dir / mesh.at("file").get<std::string>()).string()},
                   {"object_name", mesh.at("name")},
                   {"color", mesh.at("color")}};
    if (object != objects.end()) {
      params["pose"] = object->pose.matrix();
    }
    requests.push_back({"add_mesh", std::move(params)});
  }
  return requests;
}

Json Gui::executeRun(PreparedRun& run, const std::function<void(double)>& progress) {
  if (!run.working) {
    throw std::logic_error("Operation '" + run.operation + "' was not prepared");
  }
  Project& working = *run.working;
  const auto report = [this, &progress, &run](double fraction) {
    const double overall = run.offset + run.share * fraction;
    progress_ = overall;
    if (progress) {
      progress(overall);
    }
  };
  const auto install = [this, &working] {
    const std::scoped_lock lock(mutex_);
    // Views saved while the operation ran went to the project, not to the working copy.
    working.adoptViews(*project_);
    project_ = std::move(working);
  };
  const auto preview = [this](const Json& summary, const std::vector<std::uint8_t>& png) {
    const std::scoped_lock lock(mutex_);
    running_preview_ = summary;
    running_preview_png_ = png;
  };
  try {
    const Step& step = working.run(registry_, run.operation, run.params, run.inputs, report,
                                   &cancel_, run.target, preview);
    Json json = working.toJson().at("steps").back();
    json["active"] = true;
    json["size_bytes"] = directorySize(working.stepDir(step));
    install();
    run.working.reset();
    return json;
  } catch (...) {
    install();  // the failed step is part of the protocol
    run.working.reset();
    releaseRun();
    throw;
  }
}

void Gui::checkAllowedPaths(const std::string& method, const Json& params) const {
  if (allowed_roots_.empty() || !params.is_object()) {
    return;
  }
  for (const GuiMethod& known : methods()) {
    if (known.name != method || !known.parameters.contains("properties")) {
      continue;
    }
    for (const auto& [key, schema] : known.parameters.at("properties").items()) {
      if (schema.value("format", "") == "path" && params.contains(key) &&
          params.at(key).is_string()) {
        checkAllowedPath(params.at(key).get<std::string>());
      }
    }
  }
}

Json Gui::waitForOperation(double seconds) {
  std::unique_lock lock(mutex_);
  idle_.wait_for(lock, std::chrono::duration<double>(seconds), [this] { return !running_; });
  // The background thread stores its result right after it marks the operation as done.
  if (!running_ && background_.joinable()) {
    std::thread finished = std::move(background_);
    lock.unlock();
    finished.join();
    lock.lock();
  }
  Json json = {{"done", !running_}, {"result", background_result_}};
  if (running_) {
    json["running"] = {{"operation", running_operation_}, {"progress", progress_.load()}};
  }
  return json;
}

void Gui::setAllowedRoots(const std::vector<std::filesystem::path>& roots) {
  std::vector<std::filesystem::path> canonical;
  for (const auto& root : roots) {
    if (!std::filesystem::is_directory(root)) {
      throw std::invalid_argument("Not a directory: " + root.string());
    }
    canonical.push_back(std::filesystem::canonical(root));
  }
  allowed_roots_ = std::move(canonical);
}

void Gui::checkAllowedPath(const std::filesystem::path& path) const {
  if (allowed_roots_.empty()) {
    return;
  }
  // Symbolic links and ".." are resolved first, so neither leads out of a root.
  const auto resolved = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
  for (const auto& root : allowed_roots_) {
    const auto relative = resolved.lexically_relative(root);
    if (!relative.empty() && *relative.begin() != "..") {
      return;
    }
  }
  std::string roots;
  for (const auto& root : allowed_roots_) {
    roots += (roots.empty() ? "" : ", ") + root.string();
  }
  throw std::invalid_argument("Outside the allowed directories (" + roots +
                              "): " + resolved.string());
}

Json Gui::call(const std::string& method, const Json& arguments,
               const std::function<void(double)>& progress) {
  const Json params = arguments.is_null() ? Json::object() : arguments;  // `{}` in C++ is null
  if (!params.is_object()) {
    throw std::invalid_argument("Parameters must be an object");
  }
  checkAllowedPaths(method, params);
  if (method.starts_with(kRunPrefix)) {
    const std::string operation = method.substr(std::string(kRunPrefix).size());
    if (registry_.find(operation)) {
      return runOperation(operation, params, progress);
    }
  }
  if (method == "pipeline") {
    const std::vector<GuiMethod> known = methods();
    const Json checked = validateParameters(
        std::ranges::find(known, std::string("pipeline"), &GuiMethod::name)->parameters, params);
    std::vector<RunRequest> requests;
    for (const Json& step : checked.at("steps")) {
      RunRequest request{step.at("operation").get<std::string>(),
                         step.value("params", Json::object())};
      if (!registry_.find(request.operation)) {
        throw std::invalid_argument("Unknown operation '" + request.operation + "'");
      }
      checkAllowedPaths(std::string(kRunPrefix) + request.operation, request.params);
      requests.push_back(std::move(request));
    }
    return runSequence(std::move(requests), checked.at("background").get<bool>(), true, progress);
  }
  if (method == "object_add") {
    if (!params.contains("path") || !params.at("path").is_string()) {
      throw std::invalid_argument("Missing parameter 'path'");
    }
    const std::filesystem::path path = params.at("path").get<std::string>();
    Json run = {{"path", path.string()}};
    if (params.contains("name")) {
      run["object_name"] = params.at("name");
    }
    if (std::filesystem::is_directory(path) && std::filesystem::exists(path / "index.json")) {
      return runOperation("open_dataset", run, progress);
    }
    if (lowerExtension(path) == ".stl") {
      return runOperation("add_mesh", run, progress);
    }
    throw std::invalid_argument("Not a dataset or an STL file: " + path.string() +
                                "; import raw volumes, TIFF stacks, DICOM stacks and gzipped XML "
                                "projects with run_import_raw, run_import_tiff, run_import_dicom "
                                "and run_import_gzipped_xml");
  }
  if (method == "view_objects" || method == "wait" || method == "histogram") {
    // Not under the lock: building the meshes of the objects takes it only to find them, and
    // waiting releases it while it waits.
    for (const GuiMethod& known : methods()) {
      if (known.name != method) {
        continue;
      }
      const Json checked = validateParameters(known.parameters, params);
      if (method == "histogram") {
        return histogram(checked);
      }
      return method == "wait" ? waitForOperation(checked.at("seconds").get<double>())
                              : viewObjects(checked);
    }
  }
  const std::scoped_lock lock(mutex_);
  for (const GuiMethod& known : methods()) {
    if (known.name != method) {
      continue;
    }
    const Json checked = validateParameters(known.parameters, params);
    const bool changes_project = method == "project_create" || method == "project_open" ||
                                 method == "project_save_as" || method == "undo" ||
                                 method == "redo" || method == "object_select";
    if (running_ && changes_project) {
      throw std::invalid_argument("Operation '" + running_operation_ + "' is still running");
    }
    if (method == "project_create") {
      const std::filesystem::path path = checked.at("path").get<std::string>();
      project_ = Project::create(path, checked.value("name", path.filename().string()));
      return status();
    }
    if (method == "project_open") {
      project_ = Project::open(checked.at("path").get<std::string>());
      return status();
    }
    if (method == "project_status") {
      return status();
    }
    if (method == "cancel") {
      if (running_) {
        cancel_ = true;
      }
      return {{"cancelled", running_ ? Json(running_operation_) : Json()}};
    }
    if (method == "project_save_as") {
      project().saveAs(checked.at("path").get<std::string>());
      return status();
    }
    if (method == "undo" || method == "redo") {
      const bool changed = method == "undo" ? project().undo() : project().redo();
      Json json = status();
      json["changed"] = changed;
      return json;
    }
    if (method == "objects") {
      Json objects = Json::array();
      for (const ProjectObject& object : project().objects()) {
        objects.push_back(toJson(object));
      }
      return {{"objects", objects}, {"active_object", project().activeObject()}};
    }
    if (method == "results") {
      Json results = Json::array();
      const bool current_only = checked.value("current_only", false);
      for (const AnalysisResult& result : project().results(checked.value("object", ""))) {
        if (current_only && !result.current) {
          continue;
        }
        Json json = toJson(result);
        json["files"] = outputFiles(project().resolve(result.ref));
        results.push_back(std::move(json));
      }
      return {{"results", results}};
    }
    if (method == "object_select") {
      project().selectObject(checked.at("id").get<std::string>());
      return {{"active_object", project().activeObject()}};
    }
    if (method == "list_operations") {
      Json operations = Json::array();
      for (const auto& operation : registry_.all()) {
        const OperationInfo& info = operation->info();
        Json inputs = Json::array();
        for (const PortInfo& port : info.inputs) {
          inputs.push_back({{"name", port.name},
                            {"type", port.type},
                            {"description", port.description},
                            {"optional", port.optional}});
        }
        Json outputs = Json::array();
        for (const PortInfo& port : info.outputs) {
          outputs.push_back(
              {{"name", port.name}, {"type", port.type}, {"description", port.description}});
        }
        operations.push_back({{"id", info.id},
                              {"title", info.title},
                              {"description", info.description},
                              {"inputs", inputs},
                              {"outputs", outputs},
                              {"parameters", info.parameters}});
      }
      return {{"operations", operations}, {"plugin_messages", plugin_messages_}};
    }
    if (method == "step_telemetry") {
      const auto& steps = project().steps();
      if (steps.empty()) {
        throw std::invalid_argument("The project has no steps");
      }
      const int id = checked.value("step", steps.back().id);
      const auto step =
          std::find_if(steps.begin(), steps.end(), [id](const Step& s) { return s.id == id; });
      if (step == steps.end()) {
        throw std::invalid_argument("No step " + std::to_string(id));
      }
      std::ifstream file(project().stepDir(*step) / "telemetry.json");
      Json record = file ? Json::parse(file, nullptr, false) : Json();
      if (record.is_discarded() || !record.is_object()) {
        record = step->telemetry;  // failed steps keep only the summary
      }
      record["step"] = id;
      return record;
    }
    if (method == "dataset_info") {
      const ArtifactRef ref = artifactRef(checked, artifact::kDataset);
      Json json = datasetInfoJson(readDatasetInfo(project().resolve(ref)));
      json["step"] = ref.step;
      json["output"] = ref.output;
      return json;
    }
    if (method == "list_files") {
      const auto root = artifactPath(checked);
      return {{"path", root.string()}, {"files", outputFiles(root)}};
    }
    if (method == "view_slice") {
      return viewSlice(checked);
    }
    if (method == "view_set") {
      project().setView(checked.at("state"));
      return {{"view", project().view()}};
    }
    if (method == "view_save") {
      const std::vector<std::uint8_t> png =
          checked.contains("image_base64")
              ? decodeBase64(checked.at("image_base64").get<std::string>())
              : std::vector<std::uint8_t>{};
      const Json state = checked.contains("state") ? checked.at("state") : project().view();
      return savedViewJson(project().saveView(checked.at("name").get<std::string>(), state, png));
    }
    if (method == "view_list") {
      Json views = Json::array();
      for (const SavedView& view : project().savedViews()) {
        views.push_back(savedViewJson(view));
      }
      return {{"views", views}};
    }
    if (method == "import_preview") {
      return importPreview(checked);
    }
    if (method == "view_image") {
      const SavedView& view = project().savedView(checked.at("id").get<int>());
      if (!view.has_image) {
        throw std::invalid_argument("View '" + view.name + "' has no picture");
      }
      std::ifstream in(project().viewImage(view.id), std::ios::binary);
      const std::vector<std::uint8_t> png{std::istreambuf_iterator<char>(in),
                                          std::istreambuf_iterator<char>()};
      return {{"id", view.id},
              {"name", view.name},
              {"image", {{"mime_type", "image/png"}, {"base64", base64(png)}}}};
    }
    if (method == "view_rename") {
      project().renameView(checked.at("id").get<int>(), checked.at("name").get<std::string>());
      return savedViewJson(project().savedView(checked.at("id").get<int>()));
    }
    if (method == "view_delete") {
      project().deleteView(checked.at("id").get<int>());
      return {{"deleted", checked.at("id")}};
    }
    if (method == "browse") {
      if (checked.contains("path")) {
        return browse(checked.at("path").get<std::string>());
      }
      return browse(allowed_roots_.empty() ? std::filesystem::current_path()
                                           : allowed_roots_.front());
    }
    if (method == "read_file") {
      const auto root = artifactPath(checked);
      auto file = root;
      if (checked.contains("file")) {
        if (!std::filesystem::is_directory(root)) {
          throw std::invalid_argument("The output is a single file; omit 'file'");
        }
        file = insideOf(root, checked.at("file").get<std::string>());
      }
      if (!std::filesystem::is_regular_file(file)) {
        throw std::invalid_argument("Not a file: " + file.string());
      }
      if (file.extension() == ".png") {
        if (std::filesystem::file_size(file) > kMaxImageBytes) {
          throw std::invalid_argument("Picture is larger than 8 MB: " + file.string());
        }
        std::ifstream in(file, std::ios::binary);
        const std::vector<std::uint8_t> png{std::istreambuf_iterator<char>(in),
                                            std::istreambuf_iterator<char>()};
        return {{"path", file.string()},
                {"image", {{"mime_type", "image/png"}, {"base64", base64(png)}}}};
      }
      if (std::filesystem::file_size(file) > kMaxReadBytes) {
        throw std::invalid_argument("File is larger than 1 MB: " + file.string());
      }
      std::ifstream in(file, std::ios::binary);
      std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
      try {
        (void)Json(text).dump();  // throws on invalid UTF-8
      } catch (const Json::type_error&) {
        text.clear();
        text.push_back('\0');
      }
      if (text.find('\0') != std::string::npos) {
        throw std::invalid_argument("Not a UTF-8 text file: " + file.string());
      }
      return {{"path", file.string()}, {"text", text}};
    }
  }
  throw std::invalid_argument("Unknown method '" + method + "'");
}

}  // namespace voxelsieve
