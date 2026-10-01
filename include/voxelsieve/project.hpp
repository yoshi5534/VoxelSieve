#pragma once

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "voxelsieve/operation.hpp"
#include "voxelsieve/transform.hpp"

namespace voxelsieve {

/// Reference to an input of a step: the output `output` of step `step`.
struct ArtifactRef {
  int step = 0;
  std::string output;
};

/// One entry of the protocol: an operation that ran (or failed) with everything needed to repeat
/// or report it.
struct Step {
  int id = 0;
  std::string operation;
  std::string title;
  nlohmann::json params = nlohmann::json::object();
  std::map<std::string, ArtifactRef> inputs;
  /// Output paths, relative to the step directory or absolute (referenced data).
  std::map<std::string, std::filesystem::path> outputs;
  std::map<std::string, std::string> output_types;
  std::string status;  // "running", "done", "failed"
  std::string started;
  std::string finished;
  std::vector<std::string> messages;
  nlohmann::json summary = nlohmann::json::object();
  /// Time and resources the step used, per phase, without the timeline (telemetry.hpp); the whole
  /// record with the timeline is telemetry.json in the step directory.
  nlohmann::json telemetry = nlohmann::json::object();
  /// The object whose data the step worked on or created (ADR 0018); empty for steps that belong
  /// to none, such as moving objects.
  std::string object;
  /// Set when the step created its object: the object's name.
  std::string object_name;
};

/// Kinds of objects.
inline constexpr const char* kVolumeObject = "volume";
inline constexpr const char* kMeshObject = "mesh";

/// A volume or a mesh of a project, placed in the project's global coordinate system (ADR 0018).
/// Objects are derived from the active steps: a step that makes a dataset or a mesh from no
/// object's data creates one, and steps that write a pose move them.
struct ProjectObject {
  std::string id;
  std::string name;
  std::string kind;  // kVolumeObject or kMeshObject
  /// The latest active dataset or mesh output of the object.
  ArtifactRef source;
  int created_by = 0;  // step
  /// Object to global coordinates, mm.
  RigidTransform pose;
  /// The steps that moved the object, in order.
  std::vector<int> moved_by;
};

/// The object a new step belongs to (ADR 0018).
struct StepTarget {
  /// An existing object: inputs not given are taken from its outputs, and the step belongs to it.
  /// Empty: inputs not given come from the active object when it has them, and the step belongs
  /// to the object of its first input.
  std::string object;
  /// Name of the object the step creates, for steps that make a dataset or a mesh from no
  /// object's data; default: the name of the file it reads.
  std::string name;
};

/// Summary keys of a step that moves objects: the moved object ids and the motion applied to their
/// poses in global coordinates, a row-major 4x4 matrix (pose <- motion * pose).
inline constexpr const char* kMovedObjectsKey = "moved_objects";
inline constexpr const char* kMotionKey = "motion";

/// A view saved under a name: the view state of the UI and a picture of it (views/<id>.png).
struct SavedView {
  int id = 0;
  std::string name;
  std::string created;
  nlohmann::json state = nlohmann::json::object();
  bool has_image = false;
};

/// A project: a directory with project.json and one directory per step (ADR 0008). Every change
/// is saved immediately. Operations never change their inputs, so undo and redo only move a cursor
/// over the step list.
class Project {
 public:
  /// Creates a new project in an empty or missing directory.
  static Project create(const std::filesystem::path& dir, const std::string& name);
  /// Opens a project. A step that was still running when the project was last written is marked
  /// failed.
  static Project open(const std::filesystem::path& dir);

  [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }
  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const std::vector<Step>& steps() const { return steps_; }
  /// Number of active steps; steps from `cursor()` on are undone.
  [[nodiscard]] std::size_t cursor() const { return cursor_; }
  [[nodiscard]] bool canUndo() const { return cursor_ > 0; }
  [[nodiscard]] bool canRedo() const { return cursor_ < steps_.size(); }

  /// Runs an operation as a new step. Inputs not given are taken from the latest active step
  /// with an output of the required type, of the target or active object first (see StepTarget).
  /// Undone steps are discarded first, with their outputs.
  /// A failing operation is recorded as a failed step and its exception rethrown. The returned
  /// reference is valid until the next change of the project.
  const Step& run(const OperationRegistry& registry, const std::string& operation,
                  const nlohmann::json& params = nlohmann::json::object(),
                  const std::map<std::string, ArtifactRef>& inputs = {},
                  const std::function<void(double)>& progress = {},
                  const std::atomic<bool>* cancel = nullptr, const StepTarget& target = {});

  bool undo();
  bool redo();

  /// Copies the project to `dir` (which must not exist) and continues there.
  void saveAs(const std::filesystem::path& dir);

  /// How the project was last shown (stage, viewer, camera, transfer function, ...), so opening it
  /// shows it the same way. Views are presentation, not processing: they are not steps and not
  /// part of undo (ADR 0008).
  [[nodiscard]] const nlohmann::json& view() const { return view_; }
  void setView(const nlohmann::json& state);
  [[nodiscard]] const std::vector<SavedView>& savedViews() const { return saved_views_; }
  /// Saves a view under a name, with a PNG picture of it when `png` is not empty.
  const SavedView& saveView(const std::string& name, const nlohmann::json& state,
                            std::span<const std::uint8_t> png = {});
  void renameView(int id, const std::string& name);
  void deleteView(int id);
  [[nodiscard]] const SavedView& savedView(int id) const;
  [[nodiscard]] std::filesystem::path viewImage(int id) const;
  /// Takes the view state and saved views of `other`, the same project changed meanwhile (an
  /// operation runs on a copy while the UI keeps saving views).
  void adoptViews(const Project& other);

  /// Absolute path of a step output.
  [[nodiscard]] std::filesystem::path resolve(const ArtifactRef& ref) const;
  /// Latest active output of the given type, of one object when `object` is not empty.
  [[nodiscard]] std::optional<ArtifactRef> latest(const std::string& type,
                                                  const std::string& object = {}) const;

  /// The objects of the active steps in the order they were created, with their current poses.
  [[nodiscard]] std::vector<ProjectObject> objects() const;
  /// An active object; throws for unknown ids.
  [[nodiscard]] ProjectObject object(const std::string& id) const;
  /// The object the user works on: its outputs are the default inputs of new steps. Empty when
  /// none is chosen or the chosen one was undone. Choosing is not a step.
  [[nodiscard]] std::string activeObject() const;
  void selectObject(const std::string& id);
  [[nodiscard]] std::filesystem::path stepDir(const Step& step) const;

  [[nodiscard]] nlohmann::json toJson() const;

 private:
  Project() = default;
  void save() const;
  [[nodiscard]] const Step& stepById(int id) const;
  [[nodiscard]] bool isActive(const Step& step) const;

  std::filesystem::path dir_;
  std::string name_;
  std::string created_;
  std::vector<Step> steps_;
  std::size_t cursor_ = 0;
  int next_id_ = 1;
  nlohmann::json view_ = nlohmann::json::object();
  std::vector<SavedView> saved_views_;
  int next_view_id_ = 1;
  std::string active_object_;
  int next_object_id_ = 1;
};

/// An object as JSON: id, name, kind, source {step, output}, created_by, pose (row-major 4x4) and
/// moved_by.
[[nodiscard]] nlohmann::json toJson(const ProjectObject& object);

}  // namespace voxelsieve
