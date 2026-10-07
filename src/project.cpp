#include "voxelsieve/project.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <stdexcept>

#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

// Format 2 records the object of every step (ADR 0018); format 1 projects are migrated on open.
constexpr int kProjectFormat = 2;
constexpr const char* kTelemetryFile = "telemetry.json";
constexpr std::size_t kMaxViewNameBytes = 200;
constexpr std::size_t kMaxViewStateBytes = std::size_t{256} << 10U;

std::string nowUtc() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
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
          {"summary", step.summary},
          {"telemetry", step.telemetry},
          {"object", step.object},
          {"object_name", step.object_name}};
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
  step.telemetry = json.value("telemetry", Json::object());
  step.object = json.value("object", "");
  step.object_name = json.value("object_name", "");
  return step;
}

/// The object kind a step output makes, or nullptr.
const char* objectKind(const std::string& output_type) {
  if (output_type == artifact::kDataset) {
    return kVolumeObject;
  }
  return output_type == artifact::kMesh ? kMeshObject : nullptr;
}

/// The name of a new object: the file or directory the step read, else "Object <n>".
std::string defaultObjectName(const Json& params, int number) {
  if (const auto path = params.find("path"); path != params.end() && path->is_string()) {
    std::filesystem::path file = std::filesystem::path(path->get<std::string>()).lexically_normal();
    if (file.filename().empty()) {
      file = file.parent_path();  // "dir/" names the directory
    }
    if (!file.stem().empty()) {
      return file.stem().string();
    }
  }
  return "Object " + std::to_string(number);
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
  // Closed before a migration saves the project: Windows cannot replace a file that is open.
  const Json json = [&dir] {
    std::ifstream in(dir / "project.json");
    if (!in) {
      throw std::invalid_argument("Not a VoxelSieve project: " + dir.string());
    }
    return Json::parse(in);
  }();
  const int format = json.value("format", 0);
  if (format > kProjectFormat) {
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
  project.active_object_ = json.value("active_object", "");
  project.next_object_id_ = json.value("next_object_id", 1);
  if (format < 2) {
    // Before objects: every step that made a dataset from no object's data becomes an object, and
    // the steps working on it belong to it.
    std::map<int, std::string> object_of;
    for (Step& step : project.steps_) {
      for (const auto& [name, ref] : step.inputs) {
        if (const auto it = object_of.find(ref.step);
            it != object_of.end() && !it->second.empty()) {
          step.object = it->second;
          break;
        }
      }
      const bool creates = std::ranges::any_of(step.output_types, [](const auto& output) {
        return objectKind(output.second) != nullptr;
      });
      if (step.object.empty() && creates) {
        const int number = project.next_object_id_++;
        step.object = "o" + std::to_string(number);
        step.object_name = defaultObjectName(step.params, number);
      }
      object_of[step.id] = step.object;
    }
    changed = true;
  }
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

std::optional<ArtifactRef> Project::latest(const std::string& type,
                                           const std::string& object) const {
  for (std::size_t i = cursor_; i > 0; --i) {
    const Step& step = steps_[i - 1];
    if (step.status != "done" || (!object.empty() && step.object != object)) {
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

std::vector<ProjectObject> Project::objects() const {
  std::vector<ProjectObject> objects;
  const auto find = [&objects](const std::string& id) -> ProjectObject* {
    const auto it = std::ranges::find(objects, id, &ProjectObject::id);
    return it == objects.end() ? nullptr : &*it;
  };
  for (std::size_t i = 0; i < cursor_; ++i) {
    const Step& step = steps_[i];
    if (step.status != "done") {
      continue;
    }
    if (!step.object.empty()) {
      ProjectObject* object = find(step.object);
      for (const auto& [name, type] : step.output_types) {
        const char* kind = objectKind(type);
        if (kind == nullptr) {
          continue;
        }
        if (object == nullptr) {
          objects.push_back({.id = step.object,
                             .name = step.object_name.empty() ? step.object : step.object_name,
                             .kind = kind,
                             .source = {},
                             .created_by = step.id,
                             .pose = {},
                             .moved_by = {}});
          object = &objects.back();
        }
        if (object->kind == kind) {
          object->source = {step.id, name};
        }
        break;
      }
    }
    const auto moved = step.summary.find(kMovedObjectsKey);
    const auto motion = step.summary.find(kMotionKey);
    if (moved != step.summary.end() && motion != step.summary.end()) {
      const auto values = motion->get<std::vector<double>>();
      const RigidTransform transform = RigidTransform::fromMatrix(values);
      for (const Json& id : *moved) {
        if (ProjectObject* object = find(id.get<std::string>())) {
          object->pose = transform.after(object->pose);
          object->moved_by.push_back(step.id);
        }
      }
    }
  }
  return objects;
}

ProjectObject Project::object(const std::string& id) const {
  for (ProjectObject& object : objects()) {
    if (object.id == id) {
      return std::move(object);
    }
  }
  throw std::invalid_argument("No object '" + id + "'");
}

std::string Project::activeObject() const {
  if (active_object_.empty()) {
    return {};
  }
  const auto all = objects();
  return std::ranges::find(all, active_object_, &ProjectObject::id) == all.end() ? std::string()
                                                                                 : active_object_;
}

void Project::selectObject(const std::string& id) {
  if (!id.empty()) {
    (void)object(id);  // throws for unknown ids
  }
  active_object_ = id;
  save();
}

Json toJson(const ProjectObject& object) {
  return {{"id", object.id},
          {"name", object.name},
          {"kind", object.kind},
          {"source", {{"step", object.source.step}, {"output", object.source.output}}},
          {"created_by", object.created_by},
          {"pose", object.pose.matrix()},
          {"moved_by", object.moved_by}};
}

const Step& Project::run(const OperationRegistry& registry, const std::string& operation_id,
                         const Json& params, const std::map<std::string, ArtifactRef>& inputs,
                         const std::function<void(double)>& progress,
                         const std::atomic<bool>* cancel, const StepTarget& target) {
  const auto operation = registry.find(operation_id);
  if (!operation) {
    throw std::invalid_argument("Unknown operation '" + operation_id + "'");
  }
  const OperationInfo& info = operation->info();
  Step step;
  step.operation = info.id;
  step.title = info.title;
  step.params = validateParameters(info.parameters, params);
  const std::vector<ProjectObject> known = objects();
  if (!target.object.empty() &&
      std::ranges::find(known, target.object, &ProjectObject::id) == known.end()) {
    throw std::invalid_argument("No object '" + target.object + "'");
  }
  // Inputs not given come from the target object only, or from the active object when it has them.
  const std::string preferred = target.object.empty() ? activeObject() : target.object;
  const auto default_input = [&](const std::string& type) -> std::optional<ArtifactRef> {
    if (!preferred.empty()) {
      if (auto found = latest(type, preferred)) {
        return found;
      }
    }
    return target.object.empty() ? latest(type) : std::nullopt;
  };

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
    } else if (const auto found = default_input(port.type)) {
      step.inputs[port.name] = *found;
    } else if (!port.optional) {
      throw std::invalid_argument(
          "No " + port.type + " available for input '" + port.name + "' of " + info.id +
          (target.object.empty() ? std::string() : " in object '" + target.object + "'"));
    }
  }
  for (const auto& [name, ref] : inputs) {
    if (!step.inputs.contains(name)) {
      throw std::invalid_argument(info.id + " has no input '" + name + "'");
    }
  }

  // The step belongs to the target object, else to the object of its first input; one that makes
  // a dataset or a mesh from no object's data creates a new object.
  step.object = target.object;
  for (const PortInfo& port : info.inputs) {
    if (!step.object.empty()) {
      break;
    }
    if (const auto it = step.inputs.find(port.name); it != step.inputs.end()) {
      step.object = stepById(it->second.step).object;
    }
  }
  const bool creates = std::ranges::any_of(
      info.outputs, [](const PortInfo& port) { return objectKind(port.type) != nullptr; });
  if (step.object.empty() && creates) {
    const int number = next_object_id_++;
    step.object = "o" + std::to_string(number);
    step.object_name = target.name.empty() ? defaultObjectName(step.params, number) : target.name;
  } else if (!target.name.empty()) {
    throw std::invalid_argument(info.id + " creates no object, so it takes no object name");
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
  // A cancelled operation stops at its next progress report; the step fails with this message.
  context.progress = [progress, cancel](double fraction) {
    if (cancel != nullptr && cancel->load()) {
      throw std::runtime_error("Cancelled");
    }
    if (progress) {
      progress(fraction);
    }
  };
  context.log = [&stored](const std::string& message) { stored.messages.push_back(message); };
  context.cancel = cancel;
  // The objects with their latest output of every type, so operations can work on several.
  std::map<std::string, Json> outputs;
  for (std::size_t i = 0; i < cursor_; ++i) {
    const Step& done = steps_[i];
    if (done.status == "done" && !done.object.empty()) {
      for (const auto& [name, type] : done.output_types) {
        outputs[done.object][type] = resolve({done.id, name}).string();
      }
    }
  }
  for (const ProjectObject& object : known) {
    context.objects.push_back(
        {{"id", object.id},
         {"name", object.name},
         {"kind", object.kind},
         {"pose", object.pose.matrix()},
         {"outputs", outputs.contains(object.id) ? outputs.at(object.id) : Json::object()}});
  }
  context.object = step.object;
  Telemetry telemetry(info.id);
  try {
    OperationResult result;
    {
      const TelemetryScope scope(telemetry);
      result = operation->run(context);
    }
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
    stored.telemetry = telemetrySummary(telemetry.finish());
    stored.finished = nowUtc();
    save();
    throw;
  }
  const Json record = telemetry.finish();
  stored.telemetry = telemetrySummary(record);
  std::ofstream(dir / kTelemetryFile) << record.dump() << '\n';
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
  return {{"format", kProjectFormat},
          {"name", name_},
          {"created", created_},
          {"cursor", cursor_},
          {"steps", steps},
          {"view", view_},
          {"saved_views", views},
          {"active_object", active_object_},
          {"next_object_id", next_object_id_}};
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
