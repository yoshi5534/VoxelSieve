find_package(GTest REQUIRED)

add_executable(voxelsieve_tests
  test_io.cpp
  test_phantom.cpp
  test_sieve.cpp
  test_vdb.cpp
)
target_link_libraries(voxelsieve_tests PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(voxelsieve_tests)

add_test(NAME vs_phantom_cli COMMAND vs-phantom --out ${CMAKE_CURRENT_BINARY_DIR}/cli_phantom --dims 32)
add_test(NAME vs_sieve_cli COMMAND vs-sieve ${CMAKE_CURRENT_BINARY_DIR}/cli_phantom.raw
         --out ${CMAKE_CURRENT_BINARY_DIR}/cli_phantom.vdb)
set_tests_properties(vs_sieve_cli PROPERTIES DEPENDS vs_phantom_cli)
