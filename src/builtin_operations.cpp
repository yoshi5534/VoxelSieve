// The operations that ship with VoxelSieve. Each wraps a library function; the command-line tools
// use the same functions. The operations themselves are in src/operations/, by what they work on.
#include "operations/common.hpp"

namespace voxelsieve {

using Json = nlohmann::json;

Json sieveParameters() {
  Json properties = Json::object();
  properties["threshold"] = {
      {"type", "number"},
      {"description", "Air/material grey value; default: estimated (valley after the air peak)"}};
  properties["margin_voxels"] = {{"type", "integer"},
                                 {"minimum", 0},
                                 {"default", 3},
                                 {"description", "Air margin kept around the part, in voxels"}};
  properties["min_material_voxels"] = {
      {"type", "integer"},
      {"minimum", 1},
      {"maximum", 512},
      {"default", 1},
      {"description",
       "Voxels above the threshold for an 8^3 block to count as material; raise it for noisy "
       "scans"}};
  properties["outside_air_axes"] = {
      {"type", "string"},
      {"pattern", "^[xyz]+$"},
      {"default", "xyz"},
      {"description",
       "Axes whose boundary faces let outside air in; xy when the first and last slice cut "
       "through the part (a pipe, a long part scanned in sections), so its inside is kept"}};
  properties["brick_size"] = {{"type", "integer"},
                              {"minimum", 8},
                              {"default", 256},
                              {"description", "Brick edge length in voxels, a multiple of 8"}};
  properties["value_type"] = operations::valueTypeParameter();
  properties["staging"] = {
      {"type", "boolean"},
      {"default", true},
      {"description",
       "Copy a slow input (TIFF or DICOM stack, compressed file, network share) first, so that "
       "every slice is decoded once: into memory when the copy fits into the memory budget, else "
       "to a temporary raw file, which needs 1 or 2 bytes per voxel (as the input) of free space. "
       "Off: read the slices directly, with no copy but more decoding"}};
  properties["staging_dir"] = {
      {"type", "string"},
      {"format", "path"},
      {"description",
       "Directory of the temporary copy, best a fast local disk; default: where the dataset is "
       "written (the step's directory in a project)"}};
  return properties;
}

DatasetOptions sieveOptions(const Json& params) {
  DatasetOptions options;
  if (params.contains("threshold")) {
    options.threshold = params.at("threshold").get<float>();
  }
  options.margin_voxels = params.value("margin_voxels", 3);
  options.brick_size = params.value("brick_size", std::int64_t{256});
  options.min_material_voxels = params.value("min_material_voxels", 1);
  options.outside_air_axes = parseAirAxes(params.value("outside_air_axes", std::string("xyz")));
  options.value_type = operations::valueTypeOption(params);
  options.stage_slow_sources = params.value("staging", true);
  if (const std::string dir = params.value("staging_dir", std::string()); !dir.empty()) {
    options.staging_dir = dir;
  }
  return options;
}

Json builtinParameter(const std::string& operation, const std::string& parameter) {
  static const OperationRegistry registry = [] {
    OperationRegistry all;
    registerBuiltinOperations(all);
    return all;
  }();
  const auto found = registry.find(operation);
  if (!found) {
    throw std::logic_error("No built-in operation '" + operation + "'");
  }
  const Json properties = found->info().parameters.value("properties", Json::object());
  if (!properties.contains(parameter)) {
    throw std::logic_error("Operation '" + operation + "' has no parameter '" + parameter + "'");
  }
  return properties.at(parameter);
}

void registerBuiltinOperations(OperationRegistry& registry) {
  operations::registerImportOperations(registry);
  operations::registerInspectionOperations(registry);
  operations::registerSurfaceOperations(registry);
  operations::registerMaterialsOperations(registry);
  operations::registerObjectsOperations(registry);
  operations::registerBatteryOperations(registry);
}

}  // namespace voxelsieve
