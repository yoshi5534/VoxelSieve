add_executable(vs-denoise main.cpp)
target_link_libraries(vs-denoise PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
