#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "voxelsieve/operation.hpp"
#include "voxelsieve/project.hpp"
#include "voxelsieve/slice.hpp"
#include "voxelsieve/surface.hpp"

namespace voxelsieve {

/// One method of the studio API: name, description and JSON schema of its parameters. MCP lists
/// each method as a tool; the browser UI calls them over HTTP.
struct StudioMethod {
  std::string name;
  std::string description;
  nlohmann::json parameters;
};

/// The studio engine (ADR 0008): one open project and the registered operations, driven through
/// a JSON API. Thread-safe: one operation runs at a time on the calling thread, while other
/// threads can still query the project; changes to the project wait until the operation is done.
class Studio {
 public:
  /// Registers the built-in operations and loads plugins from `plugin_dirs`; the load messages are
  /// available through `pluginMessages()`.
  explicit Studio(const std::vector<std::filesystem::path>& plugin_dirs = {});

  [[nodiscard]] const OperationRegistry& registry() const { return registry_; }
  /// For adding operations in-process; not while calls are running.
  [[nodiscard]] OperationRegistry& registry() { return registry_; }
  [[nodiscard]] const std::vector<std::string>& pluginMessages() const { return plugin_messages_; }

  /// All API methods: the fixed ones plus `run_<operation>` for every operation.
  [[nodiscard]] std::vector<StudioMethod> methods() const;

  /// Calls a method. Throws std::invalid_argument for unknown methods or bad parameters and
  /// rethrows failures of operations. `progress` receives fractions while an operation runs.
  nlohmann::json call(const std::string& method, const nlohmann::json& params,
                      const std::function<void(double)>& progress = {});

  /// A file inside a step output (or the output itself when `file` is empty), for serving
  /// results. Throws for paths outside the output.
  [[nodiscard]] std::filesystem::path outputFile(int step, const std::string& output,
                                                 const std::string& file) const;

  /// The picture of a saved view of the open project, for serving it.
  [[nodiscard]] std::filesystem::path viewImage(int id) const;

  /// A slice rectangle of the dataset output of `dataset_step` (default: the latest dataset) for
  /// the viewer, with pores and zones of `porosity_step` in the overlay when given. Datasets and
  /// porosity results stay open between calls, so repeated tiles only read bricks.
  [[nodiscard]] SliceImage sliceTile(std::optional<int> dataset_step,
                                     std::optional<int> porosity_step,
                                     const SliceRequest& request) const;

  /// The volume of a dataset step for the 3D view (see readVolumePreview).
  [[nodiscard]] VolumePreview volumePreview(std::optional<int> dataset_step,
                                            std::optional<int> porosity_step,
                                            std::int64_t max_size) const;

  /// Display mesh of the surface output of `surface_step` (default: the latest surface) in
  /// level-0 voxel coordinates, with at most about `max_triangles` (see surfaceDisplayMesh). The
  /// last meshes stay cached.
  [[nodiscard]] std::shared_ptr<const IndexedMesh> surfaceMesh(std::optional<int> surface_step,
                                                               std::size_t max_triangles) const;

  /// The compared surface of a nominal-actual comparison step (default: the latest) in level-0
  /// voxel coordinates, with the deviation per vertex in mm and the tolerance and colour range.
  struct DeviationView {
    IndexedMesh mesh;
    std::vector<float> deviation_mm;
    double tolerance_mm = 0.0;
    double range_mm = 0.0;
  };
  [[nodiscard]] std::shared_ptr<const DeviationView> deviationMesh(
      std::optional<int> comparison_step) const;

  /// Asks a running operation to stop at its next check.
  void cancel() { cancel_ = true; }

 private:
  Project& project();
  [[nodiscard]] const Project& project() const;
  nlohmann::json status() const;
  nlohmann::json runOperation(const std::string& operation, nlohmann::json params,
                              const std::function<void(double)>& progress);
  [[nodiscard]] std::filesystem::path artifactPath(const nlohmann::json& params) const;
  [[nodiscard]] ArtifactRef artifactRef(const nlohmann::json& params,
                                        const std::string& type) const;
  nlohmann::json viewSlice(const nlohmann::json& params) const;
  [[nodiscard]] std::pair<std::shared_ptr<const Dataset>, std::shared_ptr<const PorosityResult>>
  openView(std::optional<int> dataset_step, std::optional<int> porosity_step) const;
  [[nodiscard]] std::shared_ptr<const Dataset> openDataset(const std::filesystem::path& dir) const;
  [[nodiscard]] std::shared_ptr<const PorosityResult> openPorosity(
      const std::filesystem::path& dir) const;

  OperationRegistry registry_;
  std::vector<std::string> plugin_messages_;
  std::optional<Project> project_;
  std::atomic<bool> cancel_ = false;
  std::atomic<double> progress_ = 0.0;
  bool running_ = false;  // guarded by mutex_, like project_
  std::string running_operation_;
  mutable std::mutex mutex_;
  // Open datasets and porosity results for viewing, most recently used last.
  mutable std::vector<std::pair<std::filesystem::path, std::shared_ptr<const Dataset>>> datasets_;
  mutable std::vector<std::pair<std::filesystem::path, std::shared_ptr<const PorosityResult>>>
      porosity_results_;
  mutable std::mutex view_mutex_;
  // Display meshes by surface file and triangle budget; never locked while a mesh is built.
  mutable std::vector<std::pair<std::filesystem::path, std::shared_ptr<const IndexedMesh>>>
      surface_meshes_;
  mutable std::vector<std::pair<std::filesystem::path, std::shared_ptr<const DeviationView>>>
      deviation_meshes_;
  mutable std::mutex surface_mutex_;
};

/// Plugin directories from the environment variable VOXELSIEVE_PLUGIN_PATH (colon-separated).
[[nodiscard]] std::vector<std::filesystem::path> pluginPathFromEnvironment();

}  // namespace voxelsieve
