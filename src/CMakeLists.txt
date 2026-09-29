# The report template is compiled in, so vs-report works without installed data files.
set(VOXELSIEVE_REPORT_TEMPLATE_FILE ${PROJECT_SOURCE_DIR}/resources/report_template.html)
file(READ ${VOXELSIEVE_REPORT_TEMPLATE_FILE} VOXELSIEVE_REPORT_TEMPLATE)
configure_file(report_template.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/report_template.cpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${VOXELSIEVE_REPORT_TEMPLATE_FILE})

# The browser UI of vs-studio is compiled in the same way.
set(VOXELSIEVE_UI_DIR ${PROJECT_SOURCE_DIR}/resources/ui)
file(READ ${VOXELSIEVE_UI_DIR}/index.html VOXELSIEVE_UI_INDEX)
file(READ ${VOXELSIEVE_UI_DIR}/app.js VOXELSIEVE_UI_SCRIPT)
file(READ ${VOXELSIEVE_UI_DIR}/app.css VOXELSIEVE_UI_STYLE)
file(READ ${VOXELSIEVE_UI_DIR}/viewer.js VOXELSIEVE_UI_VIEWER)
file(READ ${VOXELSIEVE_UI_DIR}/volume.js VOXELSIEVE_UI_VOLUME)
file(READ ${VOXELSIEVE_UI_DIR}/transfer.js VOXELSIEVE_UI_TRANSFER)
configure_file(ui_resources.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/ui_resources.cpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${VOXELSIEVE_UI_DIR}/index.html ${VOXELSIEVE_UI_DIR}/app.js ${VOXELSIEVE_UI_DIR}/app.css
  ${VOXELSIEVE_UI_DIR}/viewer.js ${VOXELSIEVE_UI_DIR}/volume.js ${VOXELSIEVE_UI_DIR}/transfer.js)

add_library(voxelsieve SHARED
  builtin_operations.cpp
  operation.cpp
  project.cpp
  slice.cpp
  studio.cpp
  mcp.cpp
  http_server.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/ui_resources.cpp
  io.cpp
  materials.cpp
  mesh.cpp
  parts.cpp
  phantom.cpp
  porosity.cpp
  report.cpp
  template.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/report_template.cpp
  dataset.cpp
  dataset_reader.cpp
  detail/blocks.cpp
  detail/png.cpp
  detail/tiff.cpp
  detail/zip.cpp
  sieve.cpp
  source.cpp
  surface.cpp
  compare.cpp
  render.cpp
  synthetic.cpp
  tiff.cpp
  vdb.cpp
  voxel_size.cpp
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
  PRIVATE Boost::iostreams Boost::headers ${CMAKE_DL_LIBS}
  PRIVATE voxelsieve_warnings
)
