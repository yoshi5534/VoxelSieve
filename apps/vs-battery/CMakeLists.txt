add_executable(vs-battery main.cpp)
target_link_libraries(vs-battery PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
