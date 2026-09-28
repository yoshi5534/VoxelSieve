#include "voxelsieve/project.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <stdexcept>

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

constexpr int kProjectFormat = 1;
constexpr std::size_t kMaxViewNameBytes = 200;
constexpr std::size_t kMaxViewStateBytes = std::size_t{256} << 10U;

std::string nowUtc() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&now, &tm);
  std::array<char, 32> buffer{};
  (void)std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer.data();
}

Json stepToJson(const Step& step) {
  Json inputs = Json::object();
  for (const auto& [name, ref] : step.inputs) {
    inputs[name] = {{"step", ref.step}, {"output", ref.output}};
  }
  Json outputs = Json::object();
  for (const auto& [name, path] : step.outputs) {
    outputs[name] = {{"path", path.string()}, {"type", step.output_types.at(name)}};
  }
  return {{"id", step.id},
          {"operation", step.operation},
          {"title", step.title},
          {"params", step.params},
          {"inputs", inputs},
          {"outputs", outputs},
          {"status", step.status},
          {"started", step.started},
          {"finished", step.finished},
          {"messages", step.messages},
          {"summary", step.summary}};
}

Step stepFromJson(const Json& json) {
  Step step;
  step.id = json.at("id").get<int>();
  step.operation = json.at("operation").get<std::string>();
  step.title = json.value("title", step.operation);
  step.params = json.value("params", Json::object());
  // Keep the objects alive: items() only refers to them.
  const Json inputs = json.value("inputs", Json::object());
  const Json outputs = json.value("outputs", Json::object());
  for (const auto& [name, ref] : inputs.items()) {
    step.inputs[name] = {ref.at("step").get<int>(), ref.at("output").get<std::string>()};
  }
  for (const auto& [name, output] : outputs.items()) {
    step.outputs[name] = output.at("path").get<std::string>();
    step.output_types[name] = output.at("type").get<std::string>();
  }
  step.status = json.value("status", "done");
  step.started = json.value("started", "");
  step.finished = json.value("finished", "");
  step.messages = json.value("messages", std::vector<std::string>{});
  step.summary = json.value("summary", Json::object());
  return step;
}

std::string directoryName(const Step& step) {
  return std::to_string(step.id) + "-" + step.operation;
}

}  // namespace

Project Project::create(const std::filesystem::path& dir, const std::string& name) {
  if (std::filesystem::exists(dir) && !std::filesystem::is_empty(dir)) {
    throw std::invalid_argument("Project directory is not empty: " + dir.string());
  }
  std::filesystem::create_directories(dir / "steps");
  Project project;
  project.dir_ = std::filesystem::absolute(dir);
  project.name_ = name;
  project.created_ = nowUtc();
  project.save();
  return project;
}

Project Project::open(const std::filesystem::path& dir) {
  std::ifstream in(dir / "project.json");
  if (!in) {
    throw std::invalid_argument("Not a VoxelSieve project: " + dir.string());
  }
  const Json json = Json::parse(in);
  if (json.value("format", 0) > kProjectFormat) {
    throw std::invalid_argument("Project was written by a newer VoxelSieve: " + dir.string());
  }
  Project project;
  project.dir_ = std::filesystem::absolute(dir);
  project.name_ = json.value("name", "");
  project.created_ = json.value("created", "");
  bool changed = false;
  for (const Json& item : json.value("steps", Json::array())) {
    Step step = stepFromJson(item);
    if (step.status == "running") {
      step.status = "failed";
      step.messages.emplace_back("Interrupted: the application ended while the step was running");
      changed = true;
    }
    project.next_id_ = std::max(project.next_id_, step.id + 1);
    project.steps_.push_back(std::move(step));
  }
  project.cursor_ = std::min(json.value("cursor", project.steps_.size()), project.steps_.size());
  project.view_ = json.value("view", Json::object());
  for (const Json& item : json.value("saved_views", Json::array())) {
    SavedView view;
    view.id = item.at("id").get<int>();
    view.name = item.value("name", "");
    view.created = item.value("created", "");
    view.state = item.value("state", Json::object());
    view.has_image = item.value("has_image", false);
    project.next_view_id_ = std::max(project.next_view_id_, view.id + 1);
    project.saved_views_.push_back(std::move(view));
  }
  if (changed) {
    project.save();
  }
  return project;
}

std::filesystem::path Project::stepDir(const Step& step) const {
  return dir_ / "steps" / directoryName(step);
}

const Step& Project::stepById(int id) const {
  for (const Step& step : steps_) {
    if (step.id == id) {
      return step;
    }
  }
  throw std::invalid_argument("No step " + std::to_string(id));
}

std::filesystem::path Project::resolve(const ArtifactRef& ref) const {
  const Step& step = stepById(ref.step);
  const auto it = step.outputs.find(ref.output);
  if (it == step.outputs.end()) {
    throw std::invalid_argument("Step " + std::to_string(ref.step) + " has no output '" +
                                ref.output + "'");
  }
  return it->second.is_absolute() ? it->second : stepDir(step) / it->second;
}

bool Project::isActive(const Step& step) const {
  for (std::size_t i = 0; i < cursor_; ++i) {
    if (steps_[i].id == step.id) {
      return step.status == "done";
    }
  }
  return false;
}

std::optional<ArtifactRef> Project::latest(const std::string& type) const {
  for (std::size_t i = cursor_; i > 0; --i) {
    const Step& step = steps_[i - 1];
    if (step.status != "done") {
      continue;
    }
    for (const auto& [name, output_type] : step.output_types) {
      if (output_type == type) {
        return ArtifactRef{step.id, name};
      }
    }
  }
  return std::nullopt;
}

const Step& Project::run(const OperationRegistry& registry, const std::string& operation_id,
                         const Json& params, const std::map<std::string, ArtifactRef>& inputs,
                         const std::function<void(double)>& progress,
                         const std::atomic<bool>* cancel) {
  const auto operation = registry.find(operation_id);
  if (!operation) {
    throw std::invalid_argument("Unknown operation '" + operation_id + "'");
  }
  const OperationInfo& info = operation->info();
  Step step;
  step.operation = info.id;
  step.title = info.title;
  step.params = validateParameters(info.parameters, params);

  // Wire inputs before anything changes, so a missing input leaves the project untouched.
  for (const PortInfo& port : info.inputs) {
    if (const auto given = inputs.find(port.name); given != inputs.end()) {
      const Step& source = stepById(given->second.step);
      if (!isActive(source)) {
        throw std::invalid_argument("Input '" + port.name + "' refers to undone or failed step " +
                                    std::to_string(source.id));
      }
      const auto type = source.output_types.find(given->second.output);
      if (type == source.output_types.end() || type->second != port.type) {
        throw std::invalid_argument("Input '" + port.name + "' needs a " + port.type);
      }
      step.inputs[port.name] = given->second;
    } else if (const auto found = latest(port.type)) {
      step.inputs[port.name] = *found;
    } else if (!port.optional) {
      throw std::invalid_argument("No " + port.type + " available for input '" + port.name +
                                  "' of " + info.id);
    }
  }
  for (const auto& [name, ref] : inputs) {
    if (!step.inputs.contains(name)) {
      throw std::invalid_argument(info.id + " has no input '" + name + "'");
    }
  }

  // Discard undone steps: a new step starts a new history.
  while (steps_.size() > cursor_) {
    std::filesystem::remove_all(stepDir(steps_.back()));
    steps_.pop_back();
  }

  step.id = next_id_++;
  step.status = "running";
  step.started = nowUtc();
  steps_.push_back(step);
  cursor_ = steps_.size();
  save();

  const auto dir = stepDir(step);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  Step& stored = steps_.back();
  OperationContext context;
  context.params = step.params;
  for (const auto& [name, ref] : step.inputs) {
    context.inputs[name] = resolve(ref);
  }
  context.output_dir = dir;
  if (progress) {
    context.progress = progress;
  }
  context.log = [&stored](const std::string& message) { stored.messages.push_back(message); };
  context.cancel = cancel;
  try {
    OperationResult result = operation->run(context);
    for (const PortInfo& port : info.outputs) {
      const auto it = result.outputs.find(port.name);
      if (it == result.outputs.end()) {
        throw std::runtime_error(info.id + " did not produce output '" + port.name + "'");
      }
      stored.outputs[port.name] = it->second;
      stored.output_types[port.name] = port.type;
    }
    stored.summary = std::move(result.summary);
    stored.status = "done";
  } catch (const std::exception& error) {
    stored.status = "failed";
    stored.messages.emplace_back(error.what());
    stored.outputs.clear();
    stored.output_types.clear();
    std::filesystem::remove_all(dir);
    stored.finished = nowUtc();
    save();
    throw;
  }
  stored.finished = nowUtc();
  save();
  return stored;
}

bool Project::undo() {
  if (!canUndo()) {
    return false;
  }
  --cursor_;
  save();
  return true;
}

bool Project::redo() {
  if (!canRedo()) {
    return false;
  }
  ++cursor_;
  save();
  return true;
}

void Project::saveAs(const std::filesystem::path& dir) {
  if (std::filesystem::exists(dir)) {
    throw std::invalid_argument("Target already exists: " + dir.string());
  }
  std::filesystem::copy(dir_, dir, std::filesystem::copy_options::recursive);
  dir_ = std::filesystem::absolute(dir);
  save();
}

namespace {

void checkViewName(const std::string& name) {
  if (name.empty() || name.size() > kMaxViewNameBytes) {
    throw std::invalid_argument("A view name needs 1 to 200 characters");
  }
}

void checkViewState(const Json& state) {
  if (!state.is_object()) {
    throw std::invalid_argument("A view state must be an object");
  }
  if (state.dump().size() > kMaxViewStateBytes) {
    throw std::invalid_argument("A view state must be smaller than 256 kB");
  }
}

}  // namespace

void Project::setView(const Json& state) {
  checkViewState(state);
  view_ = state;
  save();
}

const SavedView& Project::saveView(const std::string& name, const Json& state,
                                   std::span<const std::uint8_t> png) {
  checkViewName(name);
  checkViewState(state);
  constexpr std::array<std::uint8_t, 8> kPngSignature = {0x89, 'P',  'N',  'G',
                                                         '\r', '\n', 0x1A, '\n'};
  if (!png.empty() && (png.size() < kPngSignature.size() ||
                       !std::equal(kPngSignature.begin(), kPngSignature.end(), png.begin()))) {
    throw std::invalid_argument("The picture of a view must be a PNG");
  }
  SavedView view;
  view.id = next_view_id_;
  view.name = name;
  view.created = nowUtc();
  view.state = state;
  view.has_image = !png.empty();
  if (view.has_image) {
    std::filesystem::create_directories(dir_ / "views");
    const auto file = viewImage(view.id);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
      throw std::runtime_error("Cannot write " + file.string());
    }
  }
  ++next_view_id_;
  saved_views_.push_back(std::move(view));
  save();
  return saved_views_.back();
}

const SavedView& Project::savedView(int id) const {
  const auto found = std::find_if(saved_views_.begin(), saved_views_.end(),
                                  [id](const SavedView& view) { return view.id == id; });
  if (found == saved_views_.end()) {
    throw std::invalid_argument("No saved view " + std::to_string(id));
  }
  return *found;
}

void Project::renameView(int id, const std::string& name) {
  checkViewName(name);
  (void)savedView(id);  // throws for an unknown id
  for (SavedView& view : saved_views_) {
    if (view.id == id) {
      view.name = name;
    }
  }
  save();
}

void Project::deleteView(int id) {
  const bool had_image = savedView(id).has_image;
  std::erase_if(saved_views_, [id](const SavedView& view) { return view.id == id; });
  save();
  if (had_image) {
    std::filesystem::remove(viewImage(id));
  }
}

std::filesystem::path Project::viewImage(int id) const {
  return dir_ / "views" / (std::to_string(id) + ".png");
}

void Project::adoptViews(const Project& other) {
  view_ = other.view_;
  saved_views_ = other.saved_views_;
  next_view_id_ = other.next_view_id_;
  save();
}

Json Project::toJson() const {
  Json steps = Json::array();
  for (const Step& step : steps_) {
    steps.push_back(stepToJson(step));
  }
  Json views = Json::array();
  for (const SavedView& view : saved_views_) {
    views.push_back({{"id", view.id},
                     {"name", view.name},
                     {"created", view.created},
                     {"state", view.state},
                     {"has_image", view.has_image}});
  }
  return {{"format", kProjectFormat}, {"name", name_}, {"created", created_}, {"cursor", cursor_},
          {"steps", steps},           {"view", view_}, {"saved_views", views}};
}

void Project::save() const {
  const auto file = dir_ / "project.json";
  const auto temporary = dir_ / "project.json.tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << toJson().dump(2) << "\n";
    if (!out) {
      throw std::runtime_error("Cannot write " + temporary.string());
    }
  }
  std::filesystem::rename(temporary, file);  // atomic on POSIX: never a half-written project
}

}  // namespace voxelsieve
