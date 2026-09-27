# Built with the tests (tests/CMakeLists.txt adds this directory). A plugin in its own repository
# would find VoxelSieve with find_package and link VoxelSieve::voxelsieve the same way.
add_library(voxelsieve_histogram MODULE histogram.cpp)
target_link_libraries(voxelsieve_histogram PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
set_target_properties(voxelsieve_histogram PROPERTIES
  PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins)
