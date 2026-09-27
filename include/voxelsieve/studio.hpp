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

namespace voxelsieve {

/// One method of the studio API: name, description and JSON schema of its parameters. MCP lists
/// each method as a tool; the browser UI calls them over HTTP.
struct StudioMethod {
  std::string name;
  std::string description;
  nlohmann::json parameters;
};

/// The studio engine (ADR 0008): one open project and the registered operations, driven through
/// a JSON API. Calls are serialised; operations run on the calling thread.
class Studio {
 public:
  /// Registers the built-in operations and loads plugins from `plugin_dirs`; the load messages are
  /// available through `pluginMessages()`.
  explicit Studio(const std::vector<std::filesystem::path>& plugin_dirs = {});

  [[nodiscard]] const OperationRegistry& registry() const { return registry_; }
  [[nodiscard]] const std::vector<std::string>& pluginMessages() const { return plugin_messages_; }

  /// All API methods: the fixed ones plus `run_<operation>` for every operation.
  [[nodiscard]] std::vector<StudioMethod> methods() const;

  /// Calls a method. Throws std::invalid_argument for unknown methods or bad parameters and
  /// rethrows failures of operations. `progress` receives fractions while an operation runs.
  nlohmann::json call(const std::string& method, const nlohmann::json& params,
                      const std::function<void(double)>& progress = {});

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

  OperationRegistry registry_;
  std::vector<std::string> plugin_messages_;
  std::optional<Project> project_;
  std::atomic<bool> cancel_ = false;
  mutable std::mutex mutex_;
};

/// Plugin directories from the environment variable VOXELSIEVE_PLUGIN_PATH (colon-separated).
[[nodiscard]] std::vector<std::filesystem::path> pluginPathFromEnvironment();

}  // namespace voxelsieve
