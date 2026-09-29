add_executable(vs-segment main.cpp)
target_link_libraries(vs-segment PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
