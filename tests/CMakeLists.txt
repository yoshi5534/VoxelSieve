find_package(GTest REQUIRED)

add_executable(voxelsieve_tests
  test_dataset.cpp
  test_io.cpp
  test_phantom.cpp
  test_porosity.cpp
  test_report.cpp
  test_sieve.cpp
  test_synthetic.cpp
  test_vdb.cpp
)
target_link_libraries(voxelsieve_tests PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings GTest::gtest_main)

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
         --out ${CMAKE_CURRENT_BINARY_DIR}/cli_porosity)
set_tests_properties(vs_porosity_cli PROPERTIES DEPENDS vs_porosity_cli_sieve)
add_test(NAME vs_report_cli COMMAND vs-report ${CMAKE_CURRENT_BINARY_DIR}/cli_porosity.vsieve
         --order ${PROJECT_SOURCE_DIR}/examples/inspection_order.json
         --out ${CMAKE_CURRENT_BINARY_DIR}/cli_report)
set_tests_properties(vs_report_cli PROPERTIES DEPENDS vs_porosity_cli_sieve)
add_test(NAME vs_report_cli_template COMMAND vs-report --print-template)
set_tests_properties(vs_report_cli_template PROPERTIES PASS_REGULAR_EXPRESSION "Prüfbericht")
