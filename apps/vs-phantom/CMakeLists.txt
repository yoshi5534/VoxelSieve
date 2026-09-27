add_executable(vs-phantom main.cpp)
target_link_libraries(vs-phantom PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
