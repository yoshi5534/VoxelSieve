add_executable(vs-sieve main.cpp)
target_link_libraries(vs-sieve PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
