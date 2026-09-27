# OpenVDB ships FindOpenVDB.cmake in a non-standard location. Locate it and add
# its directory to CMAKE_MODULE_PATH so find_package(OpenVDB) works.
find_file(VOXELSIEVE_FIND_OPENVDB_SCRIPT FindOpenVDB.cmake
  PATH_SUFFIXES
    lib/cmake/OpenVDB
    lib/x86_64-linux-gnu/cmake/OpenVDB
    lib/aarch64-linux-gnu/cmake/OpenVDB
    share/openvdb/cmake
)
if(VOXELSIEVE_FIND_OPENVDB_SCRIPT)
  get_filename_component(_vs_openvdb_dir "${VOXELSIEVE_FIND_OPENVDB_SCRIPT}" DIRECTORY)
  list(APPEND CMAKE_MODULE_PATH "${_vs_openvdb_dir}")
endif()
