add_executable(vs-porosity main.cpp)
target_link_libraries(vs-porosity PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
