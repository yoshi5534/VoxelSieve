add_executable(vs-diconde main.cpp)
target_link_libraries(vs-diconde PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
