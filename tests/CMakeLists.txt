find_package(GTest REQUIRED)

add_executable(voxelsieve_tests
  test_dataset.cpp
  test_io.cpp
  test_materials.cpp
  test_model.cpp
  test_phantom.cpp
  test_porosity.cpp
  test_alignment.cpp
  test_objects.cpp
  test_project.cpp
  test_studio.cpp
  test_http.cpp
  test_slice.cpp
  test_report.cpp
  test_sieve.cpp
  test_surface.cpp
  test_compare.cpp
  test_render.cpp
  test_synthetic.cpp
  test_telemetry.cpp
  test_tiff.cpp
  test_voxel_size.cpp
  test_vdb.cpp
)
target_link_libraries(voxelsieve_tests PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings GTest::gtest_main Boost::headers
                      TIFF::TIFF libzip::zip)
# Tests of internals (detail/) see the private headers.
target_include_directories(voxelsieve_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

# The example plugin is built with the tests and loaded by ProjectTest.PluginsAddOperations.
add_subdirectory(${PROJECT_SOURCE_DIR}/examples/plugins/histogram
                 ${CMAKE_BINARY_DIR}/examples/plugins/histogram)
add_dependencies(voxelsieve_tests voxelsieve_histogram)
target_compile_definitions(voxelsieve_tests PRIVATE
  VOXELSIEVE_TEST_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins")

include(GoogleTest)
gtest_discover_tests(voxelsieve_tests)

add_test(NAME vs_phantom_cli COMMAND vs-phantom --out ${CMAKE_CURRENT_BINARY_DIR}/cli_phantom --dims 32)
add_test(NAME vs_sieve_cli COMMAND vs-sieve ${CMAKE_CURRENT_BINARY_DIR}/cli_phantom.raw
         --out ${CMAKE_CURRENT_BINARY_DIR}/cli_phantom.vdb)
set_tests_properties(vs_sieve_cli PROPERTIES DEPENDS vs_phantom_cli)
# The dataset writer refuses non-empty output directories, so clear the previous run first.
add_test(NAME vs_sieve_dataset_cli_clean
         COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_BINARY_DIR}/cli_dataset)
set_tests_properties(vs_sieve_dataset_cli_clean PROPERTIES FIXTURES_SETUP cli_dataset)
add_test(NAME vs_sieve_dataset_cli
         COMMAND vs-sieve --phantom 64 --brick-size 32 --out ${CMAKE_CURRENT_BINARY_DIR}/cli_dataset)
set_tests_properties(vs_sieve_dataset_cli PROPERTIES FIXTURES_REQUIRED cli_dataset)
# Materials of the sieved phantom (one class, the box).
add_test(NAME vs_segment_cli_clean
         COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_BINARY_DIR}/cli_materials)
add_test(NAME vs_segment_cli
         COMMAND vs-segment ${CMAKE_CURRENT_BINARY_DIR}/cli_dataset --materials 1
                 --out ${CMAKE_CURRENT_BINARY_DIR}/cli_materials)
set_tests_properties(vs_segment_cli PROPERTIES DEPENDS "vs_sieve_dataset_cli;vs_segment_cli_clean"
                     FIXTURES_REQUIRED cli_dataset PASS_REGULAR_EXPRESSION "material 1 ")
# Voxels that are not cubes: the sidecar carries [x, y, z], vs-sieve prints and keeps it.
add_test(NAME vs_phantom_anisotropic_cli
         COMMAND vs-phantom --out ${CMAKE_CURRENT_BINARY_DIR}/cli_anisotropic --dims 32
                 --voxel-size 0.4,0.4,0.8)
add_test(NAME vs_sieve_anisotropic_cli_clean
         COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_BINARY_DIR}/cli_anisotropic.vsieve)
set_tests_properties(vs_sieve_anisotropic_cli_clean PROPERTIES FIXTURES_SETUP cli_anisotropic)
add_test(NAME vs_sieve_anisotropic_cli
         COMMAND vs-sieve ${CMAKE_CURRENT_BINARY_DIR}/cli_anisotropic.raw --brick-size 16
                 --slice-thickness 0.5 --out ${CMAKE_CURRENT_BINARY_DIR}/cli_anisotropic.vsieve)
set_tests_properties(vs_sieve_anisotropic_cli PROPERTIES DEPENDS vs_phantom_anisotropic_cli
                     FIXTURES_REQUIRED cli_anisotropic
                     PASS_REGULAR_EXPRESSION "0.4 x 0.4 x 0.8 mm, slices 0.5 mm thick")
add_test(NAME vs_synth_cli COMMAND vs-synth --box 6 5 4 --out ${CMAKE_CURRENT_BINARY_DIR}/cli_synth
         --voxel-size 0.2 --lunker 2 --loosening 1 --rings 3 --cupping 0.1)
add_test(NAME vs_porosity_cli_clean
         COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_BINARY_DIR}/cli_porosity.vsieve)
set_tests_properties(vs_porosity_cli_clean PROPERTIES FIXTURES_SETUP cli_porosity)
add_test(NAME vs_porosity_cli_sieve
         COMMAND vs-sieve ${CMAKE_CURRENT_BINARY_DIR}/cli_synth.raw --brick-size 32
                 --out ${CMAKE_CURRENT_BINARY_DIR}/cli_porosity.vsieve)
set_tests_properties(vs_porosity_cli_sieve PROPERTIES DEPENDS vs_synth_cli FIXTURES_REQUIRED cli_porosity)
add_test(NAME vs_porosity_cli COMMAND vs-porosity ${CMAKE_CURRENT_BINARY_DIR}/cli_porosity.vsieve
         --out ${CMAKE_CURRENT_BINARY_DIR}/cli_porosity
         --telemetry ${CMAKE_CURRENT_BINARY_DIR}/cli_porosity_telemetry.json)
set_tests_properties(vs_porosity_cli PROPERTIES DEPENDS vs_porosity_cli_sieve)
add_test(NAME vs_report_cli COMMAND vs-report ${CMAKE_CURRENT_BINARY_DIR}/cli_porosity.vsieve
         --order ${PROJECT_SOURCE_DIR}/examples/inspection_order.json
         --out ${CMAKE_CURRENT_BINARY_DIR}/cli_report)
set_tests_properties(vs_report_cli PROPERTIES DEPENDS vs_porosity_cli_sieve)
add_test(NAME vs_report_cli_template COMMAND vs-report --print-template)
set_tests_properties(vs_report_cli_template PROPERTIES PASS_REGULAR_EXPRESSION "Test report")

add_test(NAME vs_synth_part_cli
         COMMAND vs-synth --part hub --scale 0.3 --voxel-size 0.2 --blur 0.1 --noise 300
                 --stl ${CMAKE_CURRENT_BINARY_DIR}/cli_hub.stl --out ${CMAKE_CURRENT_BINARY_DIR}/cli_hub)
add_test(NAME vs_surface_cli_clean
         COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_BINARY_DIR}/cli_hub.vsieve)
set_tests_properties(vs_surface_cli_clean PROPERTIES FIXTURES_SETUP cli_surface)
add_test(NAME vs_surface_cli_sieve
         COMMAND vs-sieve ${CMAKE_CURRENT_BINARY_DIR}/cli_hub.raw --brick-size 32
                 --out ${CMAKE_CURRENT_BINARY_DIR}/cli_hub.vsieve)
set_tests_properties(vs_surface_cli_sieve PROPERTIES DEPENDS vs_synth_part_cli FIXTURES_REQUIRED cli_surface)
add_test(NAME vs_surface_cli
         COMMAND vs-surface ${CMAKE_CURRENT_BINARY_DIR}/cli_hub.vsieve
                 --out ${CMAKE_CURRENT_BINARY_DIR}/cli_hub.vss --stl ${CMAKE_CURRENT_BINARY_DIR}/cli_hub_surface.stl
                 --vdb ${CMAKE_CURRENT_BINARY_DIR}/cli_hub_surface.vdb)
set_tests_properties(vs_surface_cli PROPERTIES DEPENDS vs_surface_cli_sieve PASS_REGULAR_EXPRESSION "bit/band voxel")
add_test(NAME vs_surface_cli_info COMMAND vs-surface --info ${CMAKE_CURRENT_BINARY_DIR}/cli_hub.vss)
set_tests_properties(vs_surface_cli_info PROPERTIES DEPENDS vs_surface_cli PASS_REGULAR_EXPRESSION "against 16-bit raw")
add_test(NAME vs_compare_cli
         COMMAND vs-compare ${CMAKE_CURRENT_BINARY_DIR}/cli_hub.vss ${CMAKE_CURRENT_BINARY_DIR}/cli_hub.stl
                 --out ${CMAKE_CURRENT_BINARY_DIR}/cli_compare --tolerance 0.1 --aligned-stl)
set_tests_properties(vs_compare_cli PROPERTIES DEPENDS vs_surface_cli PASS_REGULAR_EXPRESSION "% within")
add_test(NAME vs_compare_cli_bad_initial
         COMMAND vs-compare ${CMAKE_CURRENT_BINARY_DIR}/cli_hub.vss ${CMAKE_CURRENT_BINARY_DIR}/cli_hub.stl
                 --out ${CMAKE_CURRENT_BINARY_DIR}/cli_compare_bad --initial 1,2,3)
set_tests_properties(vs_compare_cli_bad_initial PROPERTIES WILL_FAIL TRUE)
add_test(NAME vs_report_cli_images
         COMMAND vs-report ${CMAKE_CURRENT_BINARY_DIR}/cli_hub.vsieve
                 --order ${PROJECT_SOURCE_DIR}/examples/inspection_order.json
                 --out ${CMAKE_CURRENT_BINARY_DIR}/cli_hub_report
                 --surface ${CMAKE_CURRENT_BINARY_DIR}/cli_hub.vss
                 --comparison ${CMAKE_CURRENT_BINARY_DIR}/cli_compare)
set_tests_properties(vs_report_cli_images PROPERTIES DEPENDS vs_compare_cli PASS_REGULAR_EXPRESSION "report.html")

add_test(NAME vs_studio_mcp_cli
  COMMAND sh -c "printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}' | \"$<TARGET_FILE:vs-studio>\" --mcp")
set_tests_properties(vs_studio_mcp_cli PROPERTIES PASS_REGULAR_EXPRESSION "run_porosity")
add_test(NAME vs_studio_rejects_arguments COMMAND vs-studio --bogus)
set_tests_properties(vs_studio_rejects_arguments PROPERTIES WILL_FAIL TRUE)

# The browser UI has no build step; at least make sure the script parses.
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
  add_test(NAME ui_script_syntax COMMAND ${NODE_EXECUTABLE} --check ${PROJECT_SOURCE_DIR}/resources/ui/app.js)
  add_test(NAME ui_viewer_syntax COMMAND ${NODE_EXECUTABLE} --check ${PROJECT_SOURCE_DIR}/resources/ui/viewer.js)
  add_test(NAME ui_volume_syntax COMMAND ${NODE_EXECUTABLE} --check ${PROJECT_SOURCE_DIR}/resources/ui/volume.js)
  add_test(NAME ui_transfer COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/transfer_test.cjs)
  add_test(NAME ui_scene COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/scene_test.cjs)
  add_test(NAME ui_viewer COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ui/viewer_test.cjs)
endif()
