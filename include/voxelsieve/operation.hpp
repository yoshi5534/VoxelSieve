#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace voxelsieve {

/// Kinds of data that flow between operations. An artifact is a file or directory on disk.
namespace artifact {
inline constexpr const char* kRaw = "raw";            // raw volume with JSON sidecar
inline constexpr const char* kDataset = "dataset";    // bricked dataset written by writeDataset
inline constexpr const char* kPorosity = "porosity";  // directory written by the porosity step
inline constexpr const char* kReport = "report";      // directory with report.html and report.json
inline constexpr const char* kSurface = "surface";    // directory with surface.vss (ADR 0009)
inline constexpr const char* kComparison = "comparison";  // nominal-actual comparison with CAD
}  // namespace artifact

struct PortInfo {
  std::string name;
  std::string type;  // one of the artifact kinds
  std::string description;
  /// An optional input is wired to the latest active output of its type when there is one, and
  /// left out otherwise.
  bool optional = false;
};

struct OperationInfo {
  std::string id;  // stable, used in project files and as MCP tool suffix
  std::string title;
  std::string description;
  std::vector<PortInfo> inputs;
  std::vector<PortInfo> outputs;
  /// JSON schema (type "object") of the parameters; UI forms and MCP tool schemas are built from
  /// it.
  nlohmann::json parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}};
};

/// What an operation gets to run: resolved input paths, validated parameters with defaults, an
/// empty output directory, and callbacks for progress and messages.
struct OperationContext {
  nlohmann::json params;
  std::map<std::string, std::filesystem::path> inputs;
  std::filesystem::path output_dir;
  std::function<void(double fraction)> progress = [](double) {};
  std::function<void(const std::string& message)> log = [](const std::string&) {};
  const std::atomic<bool>* cancel = nullptr;
};

/// Result of an operation: output paths (relative to the output directory) per output port, and a
/// short summary shown in the protocol.
struct OperationResult {
  std::map<std::string, std::filesystem::path> outputs;
  nlohmann::json summary = nlohmann::json::object();
};

class Operation {
 public:
  Operation() = default;
  virtual ~Operation() = default;
  Operation(const Operation&) = delete;
  Operation& operator=(const Operation&) = delete;
  Operation(Operation&&) = delete;
  Operation& operator=(Operation&&) = delete;

  [[nodiscard]] virtual const OperationInfo& info() const = 0;
  /// Runs the operation. Throws on failure; the step is then marked failed with the message.
  [[nodiscard]] virtual OperationResult run(const OperationContext& context) const = 0;
};

class OperationRegistry {
 public:
  /// Registers an operation; a later registration with the same id replaces the earlier one.
  void add(std::shared_ptr<const Operation> operation);
  [[nodiscard]] std::shared_ptr<const Operation> find(const std::string& id) const;
  [[nodiscard]] std::vector<std::shared_ptr<const Operation>> all() const;

  /// Loads every plugin (shared library) in `dir` and returns one message per file, such as
  /// "loaded" or the reason it was skipped.
  std::vector<std::string> loadPlugins(const std::filesystem::path& dir);
  /// Loads one plugin; throws if it is not a VoxelSieve plugin of the current API version.
  void loadPlugin(const std::filesystem::path& library);

 private:
  // Declared first so plugins are unloaded only after their operations are destroyed.
  std::vector<std::shared_ptr<void>> libraries_;
  std::map<std::string, std::shared_ptr<const Operation>> operations_;
};

/// Registers the operations that ship with VoxelSieve: import (sieve), porosity and report.
void registerBuiltinOperations(OperationRegistry& registry);

/// Fills in schema defaults and checks types and required properties. Throws std::invalid_argument
/// with a message naming the parameter.
[[nodiscard]] nlohmann::json validateParameters(const nlohmann::json& schema,
                                                const nlohmann::json& params);

/// Plugin API version; plugins built against another version are rejected.
inline constexpr int kPluginApiVersion = 1;

}  // namespace voxelsieve

/// Plugins define these two functions with extern "C" linkage:
///
///   extern "C" int voxelsieve_plugin_api_version() { return voxelsieve::kPluginApiVersion; }
///   extern "C" void voxelsieve_register_operations(voxelsieve::OperationRegistry& registry) {
///     registry.add(std::make_shared<MyOperation>());
///   }
#define VOXELSIEVE_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
