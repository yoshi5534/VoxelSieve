#include "voxelsieve/studio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

#include "detail/png.hpp"
#include "voxelsieve/dataset.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

constexpr const char* kRunPrefix = "run_";
constexpr std::uintmax_t kMaxReadBytes = 1U << 20U;
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
                      {"voxel_size_mm", level.voxel_size_mm},
                      {"bricks", level.bricks.size()}});
  }
  return {{"dims", info.dims},
          {"voxel_size_mm", info.voxel_size_mm},
          {"brick_size", info.brick_size},
          {"threshold", info.threshold},
          {"air_level", info.air_level},
          {"margin_voxels", info.margin_voxels},
          {"active_voxels", info.active_voxel_count},
          {"levels", levels}};
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
constexpr std::size_t kViewCacheBytes = std::size_t{512} << 20U;

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
                                                                              : "dir";
    } else {
      auto sidecar = path;
      sidecar.replace_extension(".json");
      entry["kind"] =
          path.extension() != ".json" && std::filesystem::exists(sidecar, ignored) ? "raw" : "file";
      entry["size_bytes"] = it->file_size(ignored);
    }
    entries.push_back(std::move(entry));
    if (entries.size() == kMaxBrowsedEntries) {
      break;
    }
  }
  std::sort(entries.begin(), entries.end(), [](const Json& a, const Json& b) {
    const bool a_file = a.at("kind") == "file" || a.at("kind") == "raw";
    const bool b_file = b.at("kind") == "file" || b.at("kind") == "raw";
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
  while (std::getline(stream, dir, ':')) {
    if (!dir.empty()) {
      dirs.emplace_back(dir);
    }
  }
  return dirs;
}

Studio::Studio(const std::vector<std::filesystem::path>& plugin_dirs) {
  registerBuiltinOperations(registry_);
  for (const auto& dir : plugin_dirs) {
    for (std::string& message : registry_.loadPlugins(dir)) {
      plugin_messages_.push_back(std::move(message));
    }
  }
}

std::vector<StudioMethod> Studio::methods() const {
  const Json path = {{"type", "string"}, {"description", "Project directory"}};
  std::vector<StudioMethod> methods = {
      {"project_create",
       "Creates a new project in an empty or missing directory and opens it. Every step is saved "
       "immediately; there is no separate save.",
       objectSchema({{"path", path}, {"name", {{"type", "string"}}}}, {"path"})},
      {"project_open", "Opens an existing project.", objectSchema({{"path", path}}, {"path"})},
      {"project_status",
       "The open project: name, directory, cursor and the step protocol with parameters, inputs, "
       "outputs, messages and summaries. Steps with active=false are undone.",
       objectSchema(Json::object())},
      {"project_save_as", "Copies the project to a new directory and continues there.",
       objectSchema({{"path", path}}, {"path"})},
      {"undo", "Undoes the latest active step. Instant; outputs stay until a new step runs.",
       objectSchema(Json::object())},
      {"redo", "Redoes the next undone step.", objectSchema(Json::object())},
      {"list_operations",
       "All operations with inputs, outputs and parameter schema, including plugins.",
       objectSchema(Json::object())},
      {"dataset_info",
       "Dimensions, voxel size, threshold and resolution levels of a dataset output.",
       objectSchema(artifactProperties())},
      {"list_files", "Files of a step output (a directory or a single file) with sizes.",
       objectSchema(artifactProperties())},
      {"view_slice",
       "Renders a slice through a dataset as a PNG image, with pores in red and loosened zones in "
       "yellow when a porosity analysis of the dataset exists. The whole slice is shown at the "
       "resolution level that fits max_pixels; the result says which axes run right and down.",
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
         properties.erase("output");
         return objectSchema(properties);
       }()},
      {"browse",
       "Lists a directory on the machine running VoxelSieve, to choose raw volumes, datasets "
       "(.vsieve), projects and inspection orders. Entries have a kind: dir, project, dataset, "
       "raw (a file with a JSON sidecar) or file.",
       objectSchema({{"path",
                      {{"type", "string"},
                       {"description",
                        "Default: the working "
                        "directory"}}}})},
      {"read_file",
       "Reads a text file of a step output, such as report.json or porosity.json (at most 1 MB).",
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
      inputs += (inputs.empty() ? "" : ", ") + port.name + " (" + port.type + ")";
    }
    if (!info.inputs.empty()) {
      schema["properties"]["inputs"] = {
          {"type", "object"},
          {"description",
           "Optional: {input: {step, output}}. Default: the latest active output "
           "of the required type"}};
    }
    std::string description = info.title + ". " + info.description;
    if (!inputs.empty()) {
      description += " Inputs: " + inputs + ".";
    }
    description += " Runs as a new step of the open project.";
    methods.push_back({kRunPrefix + info.id, description, schema});
  }
  return methods;
}

Project& Studio::project() {
  if (!project_) {
    throw std::invalid_argument("No project open: call project_create or project_open first");
  }
  return *project_;
}

const Project& Studio::project() const {
  if (!project_) {
    throw std::invalid_argument("No project open: call project_create or project_open first");
  }
  return *project_;
}

Json Studio::status() const {
  const Json running =
      running_ ? Json{{"operation", running_operation_}, {"progress", progress_.load()}} : Json();
  if (!project_) {
    return {{"open", false}, {"running", running}};
  }
  Json json = project_->toJson();
  for (std::size_t i = 0; i < project_->steps().size(); ++i) {
    const Step& step = project_->steps()[i];
    json["steps"][i]["active"] = i < project_->cursor();
    json["steps"][i]["size_bytes"] = directorySize(project_->stepDir(step));
  }
  json["open"] = true;
  json["dir"] = project_->dir().string();
  json["can_undo"] = project_->canUndo();
  json["can_redo"] = project_->canRedo();
  json["running"] = running;
  return json;
}

ArtifactRef Studio::artifactRef(const Json& params, const std::string& type) const {
  if (!params.contains("step")) {
    if (!type.empty()) {
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

std::filesystem::path Studio::artifactPath(const Json& params) const {
  return project().resolve(artifactRef(params, ""));
}

std::shared_ptr<const Dataset> Studio::openDataset(const std::filesystem::path& dir) const {
  const std::scoped_lock lock(view_mutex_);
  return cached(datasets_, dir, kOpenDatasets, [&dir] {
    return std::make_shared<const Dataset>(
        Dataset::open(dir, kViewCacheBytes, BrickLoading::kOnAccess));
  });
}

std::shared_ptr<const PorosityResult> Studio::openPorosity(const std::filesystem::path& dir) const {
  const std::scoped_lock lock(view_mutex_);
  return cached(porosity_results_, dir, kOpenDatasets,
                [&dir] { return std::make_shared<const PorosityResult>(loadPorosityResult(dir)); });
}

SliceImage Studio::sliceTile(std::optional<int> dataset_step, std::optional<int> porosity_step,
                             const SliceRequest& request) const {
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
  const auto dataset = openDataset(dataset_dir);
  const auto porosity = porosity_dir.empty() ? nullptr : openPorosity(porosity_dir);
  return readSlice(*dataset, request, porosity.get());
}

Json Studio::viewSlice(const Json& params) const {
  const ArtifactRef dataset_ref = artifactRef(params, artifact::kDataset);
  const auto dataset_dir = project().resolve(dataset_ref);
  const auto dataset = openDataset(dataset_dir);
  const DatasetInfo& info = dataset->info();

  // Overlay: the given porosity step, or the latest one computed from this dataset.
  std::shared_ptr<const PorosityResult> porosity;
  std::optional<int> porosity_step;
  if (params.at("overlay").get<bool>()) {
    if (params.contains("porosity_step")) {
      porosity_step = params.at("porosity_step").get<int>();
    } else {
      for (std::size_t i = project().cursor(); i > 0 && !porosity_step; --i) {
        const Step& step = project().steps()[i - 1];
        const auto input = step.inputs.find("dataset");
        if (step.status == "done" && step.operation == "porosity" && input != step.inputs.end() &&
            project().resolve(input->second) == dataset_dir) {
          porosity_step = step.id;
        }
      }
    }
    if (porosity_step) {
      porosity = openPorosity(
          project().resolve(artifactRef({{"step", *porosity_step}}, artifact::kPorosity)));
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
  const SliceImage image = readSlice(*dataset, request, porosity.get());

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
    }
    std::copy(color.begin(), color.end(), rgb.begin() + static_cast<std::ptrdiff_t>(i * 3));
  }
  const auto png = detail::encodePng(static_cast<std::uint32_t>(image.width),
                                     static_cast<std::uint32_t>(image.height), 3, rgb);
  constexpr std::array<const char*, 3> kNames = {"x", "y", "z"};
  Json result = {{"step", dataset_ref.step},
                 {"axis", params.at("axis")},
                 {"index", request.index},
                 {"level", request.level},
                 {"width", image.width},
                 {"height", image.height},
                 {"right", kNames[static_cast<std::size_t>(u)]},
                 {"down", kNames[static_cast<std::size_t>(v)]},
                 {"pixel_size_mm", levels[static_cast<std::size_t>(request.level)].voxel_size_mm},
                 {"window", {low, high}},
                 {"image", {{"mime_type", "image/png"}, {"base64", base64(png)}}}};
  result["porosity_step"] = porosity_step ? Json(*porosity_step) : Json();
  return result;
}

std::filesystem::path Studio::outputFile(int step, const std::string& output,
                                         const std::string& file) const {
  const std::scoped_lock lock(mutex_);
  const auto root = project().resolve({step, output});
  const auto path = file.empty() ? root : insideOf(root, file);
  if (!std::filesystem::is_regular_file(path)) {
    throw std::invalid_argument("Not a file: " + path.string());
  }
  return path;
}

Json Studio::runOperation(const std::string& operation, Json params,
                          const std::function<void(double)>& progress) {
  // The operation runs on a copy of the project without holding the lock, so status, file and
  // view requests are answered while it runs. Changes to the project wait until it is done.
  std::optional<Project> working;
  std::map<std::string, ArtifactRef> inputs;
  {
    const std::scoped_lock lock(mutex_);
    if (running_) {
      throw std::invalid_argument("Operation '" + running_operation_ + "' is still running");
    }
    if (params.contains("inputs")) {
      if (!params.at("inputs").is_object()) {
        throw std::invalid_argument("'inputs' must be an object");
      }
      for (const auto& [name, ref] : params.at("inputs").items()) {
        if (!ref.is_object() || !ref.contains("step") || !ref.at("step").is_number_integer()) {
          throw std::invalid_argument("Input '" + name + "' needs {step, output}");
        }
        inputs[name] = artifactRef(ref, "");
      }
      params.erase("inputs");
    }
    working = project();
    running_ = true;
    running_operation_ = operation;
    progress_ = 0.0;
    cancel_ = false;
  }
  const auto report = [this, &progress](double fraction) {
    progress_ = fraction;
    if (progress) {
      progress(fraction);
    }
  };
  const auto finish = [this, &working] {
    const std::scoped_lock lock(mutex_);
    project_ = std::move(working);
    running_ = false;
    running_operation_.clear();
  };
  try {
    const Step& step = working->run(registry_, operation, params, inputs, report, &cancel_);
    Json json = working->toJson().at("steps").back();
    json["active"] = true;
    json["size_bytes"] = directorySize(working->stepDir(step));
    finish();
    return json;
  } catch (...) {
    finish();  // the failed step is part of the protocol
    throw;
  }
}

Json Studio::call(const std::string& method, const Json& arguments,
                  const std::function<void(double)>& progress) {
  const Json params = arguments.is_null() ? Json::object() : arguments;  // `{}` in C++ is null
  if (!params.is_object()) {
    throw std::invalid_argument("Parameters must be an object");
  }
  if (method.starts_with(kRunPrefix)) {
    const std::string operation = method.substr(std::string(kRunPrefix).size());
    if (registry_.find(operation)) {
      return runOperation(operation, params, progress);
    }
  }
  const std::scoped_lock lock(mutex_);
  for (const StudioMethod& known : methods()) {
    if (known.name != method) {
      continue;
    }
    const Json checked = validateParameters(known.parameters, params);
    const bool changes_project = method == "project_create" || method == "project_open" ||
                                 method == "project_save_as" || method == "undo" ||
                                 method == "redo";
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
    if (method == "list_operations") {
      Json operations = Json::array();
      for (const auto& operation : registry_.all()) {
        const OperationInfo& info = operation->info();
        Json inputs = Json::array();
        for (const PortInfo& port : info.inputs) {
          inputs.push_back(
              {{"name", port.name}, {"type", port.type}, {"description", port.description}});
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
    if (method == "dataset_info") {
      const ArtifactRef ref = artifactRef(checked, artifact::kDataset);
      Json json = datasetInfoJson(readDatasetInfo(project().resolve(ref)));
      json["step"] = ref.step;
      json["output"] = ref.output;
      return json;
    }
    if (method == "list_files") {
      const auto root = artifactPath(checked);
      Json files = Json::array();
      if (std::filesystem::is_directory(root)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
          if (!entry.is_regular_file()) {
            continue;
          }
          if (files.size() == kMaxListedFiles) {
            break;
          }
          files.push_back({{"file", entry.path().lexically_relative(root).string()},
                           {"size_bytes", entry.file_size()}});
        }
      } else {
        files.push_back(
            {{"file", root.filename().string()}, {"size_bytes", std::filesystem::file_size(root)}});
      }
      return {{"path", root.string()}, {"files", files}};
    }
    if (method == "view_slice") {
      return viewSlice(checked);
    }
    if (method == "browse") {
      return browse(checked.contains("path")
                        ? std::filesystem::path(checked.at("path").get<std::string>())
                        : std::filesystem::current_path());
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
