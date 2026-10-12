add_executable(vs-gui main.cpp)
target_link_libraries(vs-gui PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
