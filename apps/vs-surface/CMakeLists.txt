add_executable(vs-surface main.cpp)
target_link_libraries(vs-surface PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
