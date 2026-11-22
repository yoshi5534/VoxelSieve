# A plugin with another build text, which ProjectTest.PluginsAddOperations expects to be rejected.
# The plugin directory holds plugins only: vcpkg would copy the dependency DLLs next to it.
set(VCPKG_APPLOCAL_DEPS OFF)
add_library(voxelsieve_other_build MODULE other_build.cpp)
target_link_libraries(voxelsieve_other_build PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
set_target_properties(voxelsieve_other_build PROPERTIES
  PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins_other_build)
