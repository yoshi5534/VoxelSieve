add_library(voxelsieve
  io.cpp
  phantom.cpp
  vdb.cpp
)
add_library(VoxelSieve::voxelsieve ALIAS voxelsieve)

target_include_directories(voxelsieve PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(voxelsieve
  PUBLIC OpenVDB::openvdb nlohmann_json::nlohmann_json
  PRIVATE voxelsieve_warnings
)
