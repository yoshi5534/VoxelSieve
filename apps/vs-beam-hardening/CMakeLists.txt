add_executable(vs-beam-hardening main.cpp)
target_link_libraries(vs-beam-hardening PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
