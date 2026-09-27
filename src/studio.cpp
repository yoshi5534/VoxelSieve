#include "voxelsieve/studio.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

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
  if (!project_) {
    return {{"open", false}};
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

Json Studio::runOperation(const std::string& operation, Json params,
                          const std::function<void(double)>& progress) {
  std::map<std::string, ArtifactRef> inputs;
  if (params.contains("inputs")) {
    for (const auto& [name, ref] : params.at("inputs").items()) {
      if (!ref.is_object() || !ref.contains("step")) {
        throw std::invalid_argument("Input '" + name + "' needs {step, output}");
      }
      inputs[name] = artifactRef(ref, "");
      if (ref.contains("output")) {
        inputs[name].output = ref.at("output").get<std::string>();
      }
    }
    params.erase("inputs");
  }
  cancel_ = false;
  const Step& step = project().run(registry_, operation, params, inputs, progress, &cancel_);
  Json json = project().toJson().at("steps").back();
  json["active"] = true;
  json["size_bytes"] = directorySize(project().stepDir(step));
  return json;
}

Json Studio::call(const std::string& method, const Json& arguments,
                  const std::function<void(double)>& progress) {
  const std::scoped_lock lock(mutex_);
  const Json params = arguments.is_null() ? Json::object() : arguments;  // `{}` in C++ is null
  if (!params.is_object()) {
    throw std::invalid_argument("Parameters must be an object");
  }
  for (const StudioMethod& known : methods()) {
    if (known.name != method) {
      continue;
    }
    if (method.starts_with(kRunPrefix)) {
      return runOperation(method.substr(std::string(kRunPrefix).size()), params, progress);
    }
    const Json checked = validateParameters(known.parameters, params);
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
