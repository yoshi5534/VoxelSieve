add_executable(vs-report main.cpp)
target_link_libraries(vs-report PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
