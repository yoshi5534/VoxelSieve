add_executable(vs-thickness main.cpp)
target_link_libraries(vs-thickness PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
