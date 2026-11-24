#pragma once

#include <openvdb/version.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <version>

namespace voxelsieve {

struct DatasetOptions;

/// Kinds of data that flow between operations. An artifact is a file or directory on disk.
namespace artifact {
inline constexpr const char* kRaw = "raw";            // raw volume with JSON sidecar
inline constexpr const char* kDataset = "dataset";    // bricked dataset written by writeDataset
inline constexpr const char* kPorosity = "porosity";  // directory written by the porosity step
inline constexpr const char* kReport = "report";      // directory with report.html and report.json
inline constexpr const char* kSurface = "surface";    // directory with surface.vss (ADR 0009)
inline constexpr const char* kComparison = "comparison";  // nominal-actual comparison with CAD
inline constexpr const char* kMaterials = "materials";    // material volume (ADR 0013)
inline constexpr const char* kMaterialMeshes = "material_meshes";  // one STL in mm per material
inline constexpr const char* kMeasurements = "measurements";       // objects.csv and objects.json
inline constexpr const char* kMesh = "mesh";  // triangle mesh as STL in mm (ADR 0018)
inline constexpr const char* kPose = "pose";  // directory with pose.json: objects moved (ADR 0018)
inline constexpr const char* kDiconde = "diconde";  // DICONDE series of a scan (ADR 0024)
inline constexpr const char* kDimensionalMeasurement =
    "dimensional_measurement";  // characteristics of a drawing measured on a scan (ADR 0027)
inline constexpr const char* kFibreOrientation =
    "fibre_orientation";  // directory with orientation.json, images and VDB (ADR 0028)
inline constexpr const char* kWallThickness =
    "wall_thickness";  // directory with wall_thickness.json, thickness.ply and views (ADR 0030)
inline constexpr const char* kBattery =
    "battery";  // directory with battery.json and images of a cell inspection (ADR 0029)
inline constexpr const char* kInspectionRegions =
    "inspection_regions";  // one STL per inspection region of an order (ADR 0032)
inline constexpr const char* kRegion =
    "region";  // directory with region.json and region.stl: a region of a scan (ADR 0041)
}  // namespace artifact

struct PortInfo {
  std::string name;
  std::string type;  // one of the artifact kinds
  std::string description;
  /// An optional input is wired to the latest active output of its type when there is one, and
  /// left out otherwise.
  bool optional = false;
  /// The operation that makes this input from the other data of the object, with its default
  /// parameters: when the project has no output of the type, it runs that operation first, as a
  /// step of its own that later steps use too (the surface a comparison needs). Empty: a
  /// missing input is an error.
  std::string made_by = {};
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
  /// The operation continues work it finds in its output directory, such as an import with its
  /// journal. A project then keeps the files of a run that failed or was interrupted and hands
  /// them to the next run with the same parameters and inputs; without this, an operation
  /// always starts in an empty directory.
  bool resumable = false;
};

/// What an operation gets to run: resolved input paths, validated parameters with defaults, an
/// empty output directory, and callbacks for progress and messages.
struct OperationContext {
  nlohmann::json params;
  std::map<std::string, std::filesystem::path> inputs;
  std::filesystem::path output_dir;
  std::function<void(double fraction)> progress = [](double) {};
  std::function<void(const std::string& message)> log = [](const std::string&) {};
  /// Set when the operation should stop. `progress` throws once it is set, so an operation that
  /// reports progress stops without looking at it.
  const std::atomic<bool>* cancel = nullptr;
  /// The objects of the project before this step (ADR 0018): an array of {id, name, kind, pose,
  /// outputs}, the pose as a row-major 4x4 matrix from object to global coordinates in mm and
  /// outputs as {type: absolute path} of the object's latest output of each type.
  nlohmann::json objects = nlohmann::json::array();
  /// The object the step belongs to; empty for steps of no object.
  std::string object;
  /// Hands over a first look at the input while an import still runs (ADR 0020): the summary of
  /// the preview (previewSummary) and its picture as PNG. The GUI shows it at once. Last
  /// member, so plugins built against the same API version keep the layout of the others.
  std::function<void(const nlohmann::json& summary, const std::vector<std::uint8_t>& png)> preview =
      [](const nlohmann::json&, const std::vector<std::uint8_t>&) {};
  /// Throws when the caller may not read `path`, as a server that is limited to some
  /// directories. Parameters that are paths are checked before the operation runs; an operation
  /// calls this for every file that another file names (the mesh of a region in an inspection
  /// order, the volume files of a project file) before it reads it.
  std::function<void(const std::filesystem::path& path)> check_path =
      [](const std::filesystem::path&) {};
};

/// Result of an operation: output paths (relative to the output directory) per output port, and a
/// short summary shown in the protocol.
struct OperationResult {
  std::map<std::string, std::filesystem::path> outputs;
  nlohmann::json summary = nlohmann::json::object();
};

/// What a run of an operation is expected to need (ADR 0034); 0 where it is not known.
struct OperationEstimate {
  /// Work memory besides the brick caches.
  std::uint64_t memory_bytes = 0;
  /// Disk space of its outputs.
  std::uint64_t output_bytes = 0;
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
  /// Estimates what a run needs from its inputs and parameters, before it starts. The project
  /// checks the free disk space of the output directory against it, and admits the run only when
  /// its memory fits beside the runs of other operations (`admitOperation`). Default: unknown.
  [[nodiscard]] virtual OperationEstimate estimate(const OperationContext& context) const;
  /// Checks the validated parameters before a project makes a step of the run, cheaply: what
  /// throws here leaves no failed step in the protocol and no undone steps removed. Default:
  /// every parameter that is a path (format "path") names something that exists. Operations
  /// override it to check more (the keys of an inspection order), or less when a path of theirs
  /// is one to write to.
  virtual void check(const nlohmann::json& params) const;
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

/// The schema of one parameter of a built-in operation: its type, limits and default. The tools
/// check their options against it (`cli::Arguments::number(operation, parameter)`), so an option
/// and the parameter of the operation behind it accept the same values. Throws for a name that
/// does not exist.
[[nodiscard]] nlohmann::json builtinParameter(const std::string& operation,
                                              const std::string& parameter);

/// The parameters of the sieve as JSON-schema properties (threshold, margin_voxels,
/// min_material_voxels, outside_air_axes, brick_size, value_type, staging, staging_dir): every
/// import operation takes them and vs-sieve makes its options from them, so they are defined
/// once.
[[nodiscard]] nlohmann::json sieveParameters();

/// The options of the sieve from parameters of sieveParameters(); those not given keep their
/// defaults. The callbacks (progress, preview) are left to the caller.
[[nodiscard]] DatasetOptions sieveOptions(const nlohmann::json& params);

/// Fills in schema defaults and checks types and required properties. Throws std::invalid_argument
/// with a message naming the parameter.
[[nodiscard]] nlohmann::json validateParameters(const nlohmann::json& schema,
                                                const nlohmann::json& params);

/// File extension of plugin libraries on this platform (CMake's MODULE libraries).
#ifdef _WIN32
inline constexpr const char* kPluginExtension = ".dll";
#else
inline constexpr const char* kPluginExtension = ".so";
#endif

/// Plugin API version; plugins built against another version are rejected.
inline constexpr int kPluginApiVersion = 7;

}  // namespace voxelsieve

// The plugin interface is C++: operations, std::function and nlohmann::json cross it. A plugin
// therefore needs the standard library and the versions of the libraries in the public headers
// that VoxelSieve was built with. VOXELSIEVE_PLUGIN_BUILD names them; it is a macro so that a
// plugin states what its own compiler saw, and a plugin whose text differs is not loaded.
#define VOXELSIEVE_TEXT_OF_(value) #value
#define VOXELSIEVE_TEXT_OF(value) VOXELSIEVE_TEXT_OF_(value)
#if defined(_MSVC_STL_VERSION)
#define VOXELSIEVE_STANDARD_LIBRARY                                                    \
  "msvc " VOXELSIEVE_TEXT_OF(_MSVC_STL_VERSION) " iterator-debug " VOXELSIEVE_TEXT_OF( \
      _ITERATOR_DEBUG_LEVEL)
#elif defined(_LIBCPP_VERSION)
#define VOXELSIEVE_STANDARD_LIBRARY "libc++ abi " VOXELSIEVE_TEXT_OF(_LIBCPP_ABI_VERSION)
#elif defined(__GLIBCXX__)
#define VOXELSIEVE_STANDARD_LIBRARY "libstdc++ abi " VOXELSIEVE_TEXT_OF(_GLIBCXX_USE_CXX11_ABI)
#else
#define VOXELSIEVE_STANDARD_LIBRARY "unknown standard library"
#endif
#define VOXELSIEVE_JSON_VERSION                   \
  VOXELSIEVE_TEXT_OF(NLOHMANN_JSON_VERSION_MAJOR) \
  "." VOXELSIEVE_TEXT_OF(NLOHMANN_JSON_VERSION_MINOR)
#define VOXELSIEVE_OPENVDB_VERSION                                                         \
  VOXELSIEVE_TEXT_OF(OPENVDB_LIBRARY_MAJOR_VERSION_NUMBER)                                 \
  "." VOXELSIEVE_TEXT_OF(OPENVDB_LIBRARY_MINOR_VERSION_NUMBER) " abi " VOXELSIEVE_TEXT_OF( \
      OPENVDB_ABI_VERSION_NUMBER)
#define VOXELSIEVE_PLUGIN_BUILD                                 \
  VOXELSIEVE_STANDARD_LIBRARY ", json " VOXELSIEVE_JSON_VERSION \
                              ", openvdb " VOXELSIEVE_OPENVDB_VERSION

/// Plugins define these three functions with extern "C" linkage:
///
///   extern "C" int voxelsieve_plugin_api_version() { return voxelsieve::kPluginApiVersion; }
///   extern "C" const char* voxelsieve_plugin_build() { return VOXELSIEVE_PLUGIN_BUILD; }
///   extern "C" void voxelsieve_register_operations(voxelsieve::OperationRegistry& registry) {
///     registry.add(std::make_shared<MyOperation>());
///   }
#ifdef _WIN32
#define VOXELSIEVE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define VOXELSIEVE_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif
