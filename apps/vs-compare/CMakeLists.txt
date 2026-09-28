add_executable(vs-compare main.cpp)
target_link_libraries(vs-compare PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
