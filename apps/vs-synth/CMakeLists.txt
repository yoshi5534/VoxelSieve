add_executable(vs-synth main.cpp)
target_link_libraries(vs-synth PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
