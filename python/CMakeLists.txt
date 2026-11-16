# The Python package (ADR 0031): the extension module voxelsieve._core, built with nanobind, and
# the pure Python package python/voxelsieve around it. nanobind comes from Python (pip), not from
# vcpkg: it is a build tool of the Python package, like scikit-build-core, which builds it with
# `pip install .` (pyproject.toml).
find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
if(NOT nanobind_DIR)
  execute_process(COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
                  OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_DIR
                  RESULT_VARIABLE _vs_nanobind_result)
  if(NOT _vs_nanobind_result EQUAL 0)
    message(FATAL_ERROR "VOXELSIEVE_BUILD_PYTHON needs nanobind: ${Python_EXECUTABLE} -m pip install nanobind")
  endif()
endif()
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(_core NB_STATIC core.cpp)
# nanobind's headers do not pass VoxelSieve's warnings (-Wshadow); include them as system headers.
get_target_property(_vs_nanobind_includes nanobind-static INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(nanobind-static PROPERTIES
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_vs_nanobind_includes}")
target_link_libraries(_core PRIVATE VoxelSieve::voxelsieve voxelsieve_warnings)
target_compile_definitions(_core PRIVATE VOXELSIEVE_PYTHON_VERSION="${PROJECT_VERSION}")

# In the build tree the module sits next to a copy of the Python sources, so tests (and users of a
# build) put ${CMAKE_CURRENT_BINARY_DIR} on PYTHONPATH. On Windows the DLLs of the build are in
# bin/; the package adds that directory (VOXELSIEVE_DLL_DIR) to the DLL search path.
set(VOXELSIEVE_PYTHON_PACKAGE_DIR ${CMAKE_CURRENT_BINARY_DIR}/voxelsieve)
set_target_properties(_core PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${VOXELSIEVE_PYTHON_PACKAGE_DIR}
  RUNTIME_OUTPUT_DIRECTORY ${VOXELSIEVE_PYTHON_PACKAGE_DIR}
  LIBRARY_OUTPUT_DIRECTORY_RELEASE ${VOXELSIEVE_PYTHON_PACKAGE_DIR}
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${VOXELSIEVE_PYTHON_PACKAGE_DIR})
file(GLOB _vs_python_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/voxelsieve/*.py)
foreach(_vs_source IN LISTS _vs_python_sources)
  get_filename_component(_vs_name ${_vs_source} NAME)
  configure_file(${_vs_source} ${VOXELSIEVE_PYTHON_PACKAGE_DIR}/${_vs_name} COPYONLY)
endforeach()

if(SKBUILD)
  # A wheel: the package with _core, libvoxelsieve and the shared libraries from vcpkg in one
  # directory, which every library finds through its rpath ($ORIGIN, @loader_path) or, on
  # Windows, because DLLs next to a module are found.
  if(APPLE)
    set_target_properties(_core voxelsieve PROPERTIES INSTALL_RPATH "@loader_path")
  elseif(UNIX)
    set_target_properties(_core voxelsieve PROPERTIES INSTALL_RPATH "$ORIGIN")
  endif()
  install(TARGETS _core voxelsieve
    RUNTIME_DEPENDENCY_SET voxelsieve_python_runtime
    LIBRARY DESTINATION voxelsieve
    RUNTIME DESTINATION voxelsieve)
  install(FILES ${_vs_python_sources} DESTINATION voxelsieve)
  if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_vs_vcpkg_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    install(RUNTIME_DEPENDENCY_SET voxelsieve_python_runtime
      DIRECTORIES "${_vs_vcpkg_dir}/bin" "${_vs_vcpkg_dir}/lib"
      PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-" "[Pp]ython[0-9]*"
      POST_EXCLUDE_REGEXES
        "^/lib/" "^/lib64/" "^/usr/lib/" "^/usr/lib64/" "^/System/" "[/\\\\][Ss]ystem32[/\\\\]"
      LIBRARY DESTINATION voxelsieve
      RUNTIME DESTINATION voxelsieve)
  endif()
endif()

# The package's tests run on the module in the build tree, with the standard library's unittest.
if(VOXELSIEVE_BUILD_TESTS AND VOXELSIEVE_BUILD_APPS)
  add_test(NAME python_package
    COMMAND ${Python_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -v)
  set_tests_properties(python_package PROPERTIES
    ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR};VOXELSIEVE_DLL_DIR=${CMAKE_BINARY_DIR}/bin")
endif()
