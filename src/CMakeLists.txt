add_library(voxelsieve
  io.cpp
  mesh.cpp
  phantom.cpp
  dataset.cpp
  detail/blocks.cpp
  sieve.cpp
  source.cpp
  synthetic.cpp
  vdb.cpp
)
add_library(VoxelSieve::voxelsieve ALIAS voxelsieve)

target_include_directories(voxelsieve PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_include_directories(voxelsieve PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Boost REQUIRED COMPONENTS iostreams)
target_link_libraries(voxelsieve
  PUBLIC OpenVDB::openvdb nlohmann_json::nlohmann_json
  PRIVATE Boost::iostreams
  PRIVATE voxelsieve_warnings
)
