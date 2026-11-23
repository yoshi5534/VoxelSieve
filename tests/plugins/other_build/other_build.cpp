// A plugin that says it was built with other libraries than VoxelSieve: it must not be loaded
// (ProjectTest.PluginsAddOperations).
#include "voxelsieve/operation.hpp"

// NOLINTBEGIN(readability-identifier-naming): the names the plugin loader looks for
VOXELSIEVE_PLUGIN_EXPORT int voxelsieve_plugin_api_version() {
  return voxelsieve::kPluginApiVersion;
}

VOXELSIEVE_PLUGIN_EXPORT const char* voxelsieve_plugin_build() { return "another compiler"; }

VOXELSIEVE_PLUGIN_EXPORT void voxelsieve_register_operations(
    voxelsieve::OperationRegistry& /*registry*/) {}
// NOLINTEND(readability-identifier-naming)
