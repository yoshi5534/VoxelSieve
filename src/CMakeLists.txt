# Files compiled in become byte arrays: string literals are limited to 64 KiB by the standard and
# by MSVC, and the browser UI's scripts are larger.
function(voxelsieve_embed out_var file)
  file(READ ${file} hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  set(${out_var} "${bytes}" PARENT_SCOPE)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${file})
endfunction()

# The report template is compiled in, so vs-report works without installed data files.
voxelsieve_embed(VOXELSIEVE_REPORT_TEMPLATE ${PROJECT_SOURCE_DIR}/resources/report_template.html)
configure_file(report_template.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/report_template.cpp @ONLY)

# The browser UI of vs-studio is compiled in the same way.
set(VOXELSIEVE_UI_DIR ${PROJECT_SOURCE_DIR}/resources/ui)
voxelsieve_embed(VOXELSIEVE_UI_INDEX ${VOXELSIEVE_UI_DIR}/index.html)
voxelsieve_embed(VOXELSIEVE_UI_SCRIPT ${VOXELSIEVE_UI_DIR}/app.js)
voxelsieve_embed(VOXELSIEVE_UI_STYLE ${VOXELSIEVE_UI_DIR}/app.css)
voxelsieve_embed(VOXELSIEVE_UI_VIEWER ${VOXELSIEVE_UI_DIR}/viewer.js)
voxelsieve_embed(VOXELSIEVE_UI_VOLUME ${VOXELSIEVE_UI_DIR}/volume.js)
voxelsieve_embed(VOXELSIEVE_UI_TRANSFER ${VOXELSIEVE_UI_DIR}/transfer.js)
voxelsieve_embed(VOXELSIEVE_UI_SCENE ${VOXELSIEVE_UI_DIR}/scene.js)
configure_file(ui_resources.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/ui_resources.cpp @ONLY)

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
  model.cpp
  mesh.cpp
  parts.cpp
  phantom.cpp
  porosity.cpp
  report.cpp
  template.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/report_template.cpp
  dataset.cpp
  dataset_reader.cpp
  dicom.cpp
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
  telemetry.cpp
  tiff.cpp
  vdb.cpp
  vgl.cpp
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
  PRIVATE Boost::iostreams Boost::headers TIFF::TIFF PNG::PNG libzip::zip tinyply
          DCMTK::dcmdata pugixml::pugixml ZLIB::ZLIB
          ${CMAKE_DL_LIBS}
  PRIVATE voxelsieve_warnings
)
