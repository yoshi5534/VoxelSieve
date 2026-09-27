# The report template is compiled in, so vs-report works without installed data files.
set(VOXELSIEVE_REPORT_TEMPLATE_FILE ${PROJECT_SOURCE_DIR}/resources/report_template.html)
file(READ ${VOXELSIEVE_REPORT_TEMPLATE_FILE} VOXELSIEVE_REPORT_TEMPLATE)
configure_file(report_template.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/report_template.cpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${VOXELSIEVE_REPORT_TEMPLATE_FILE})

add_library(voxelsieve SHARED
  builtin_operations.cpp
  operation.cpp
  project.cpp
  studio.cpp
  mcp.cpp
  io.cpp
  mesh.cpp
  phantom.cpp
  porosity.cpp
  report.cpp
  template.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/report_template.cpp
  dataset.cpp
  dataset_reader.cpp
  detail/blocks.cpp
  detail/png.cpp
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
target_compile_definitions(voxelsieve PRIVATE VOXELSIEVE_VERSION="${PROJECT_VERSION}")
find_package(Boost REQUIRED COMPONENTS iostreams)
target_link_libraries(voxelsieve
  PUBLIC OpenVDB::openvdb nlohmann_json::nlohmann_json
  PRIVATE Boost::iostreams ${CMAKE_DL_LIBS}
  PRIVATE voxelsieve_warnings
)
