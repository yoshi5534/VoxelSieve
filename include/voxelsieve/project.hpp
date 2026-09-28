#pragma once

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "voxelsieve/operation.hpp"

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
};

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
  /// with an output of the required type. Undone steps are discarded first, with their outputs.
  /// A failing operation is recorded as a failed step and its exception rethrown. The returned
  /// reference is valid until the next change of the project.
  const Step& run(const OperationRegistry& registry, const std::string& operation,
                  const nlohmann::json& params = nlohmann::json::object(),
                  const std::map<std::string, ArtifactRef>& inputs = {},
                  const std::function<void(double)>& progress = {},
                  const std::atomic<bool>* cancel = nullptr);

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
  /// Latest active output of the given type.
  [[nodiscard]] std::optional<ArtifactRef> latest(const std::string& type) const;
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
};

}  // namespace voxelsieve
