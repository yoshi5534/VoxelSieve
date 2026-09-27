add_executable(vs-studio main.cpp)
target_link_libraries(vs-studio PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
