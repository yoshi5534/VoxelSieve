cmake_minimum_required(VERSION 3.24)
project(VoxelSieve VERSION 0.2.0 LANGUAGES CXX)

# The presets set VCPKG_TARGET_TRIPLET. CMake only loads a toolchain on the first configure of a
# build directory, so a directory configured before the switch to vcpkg silently keeps finding the
# system libraries; stop with a hint instead.
if(DEFINED VCPKG_TARGET_TRIPLET AND NOT VCPKG_TOOLCHAIN)
  message(FATAL_ERROR
    "vcpkg's toolchain is not active in ${CMAKE_BINARY_DIR}. The directory was probably configured "
    "before VoxelSieve used vcpkg. Run tools/setup-vcpkg.sh, export VCPKG_ROOT and configure again "
    "with --fresh, e.g. cmake --preset release --fresh")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VOXELSIEVE_BUILD_TESTS "Build unit tests" ON)
option(VOXELSIEVE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(VOXELSIEVE_INSTALL_RUNTIME_DEPS
       "Install the shared libraries the programs need next to them, for self-contained packages" OFF)

# Installed programs find libvoxelsieve and the bundled libraries in ../lib (on Windows, DLLs sit
# next to the programs in bin).
include(GNUInstallDirs)
if(APPLE)
  set(CMAKE_INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
elseif(UNIX)
  set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FindOpenVDBModule)
find_package(OpenVDB REQUIRED)
# vcpkg's TBB turns on TBB_USE_DEBUG for Debug builds, which fills the inlined OpenVDB and TBB code
# with checks and nearly doubles the ASan test time; the dependencies are release builds anyway.
if(TARGET TBB::tbb)
  get_target_property(_vs_tbb_definitions TBB::tbb INTERFACE_COMPILE_DEFINITIONS)
  if(_vs_tbb_definitions)
    list(FILTER _vs_tbb_definitions EXCLUDE REGEX "TBB_USE_DEBUG")
    set_target_properties(TBB::tbb PROPERTIES INTERFACE_COMPILE_DEFINITIONS "${_vs_tbb_definitions}")
  endif()
endif()
find_package(nlohmann_json 3.10 REQUIRED)
# File formats and compression come from established libraries, never our own code (ADR 0016).
find_package(TIFF REQUIRED)
find_package(PNG REQUIRED)
find_package(libzip CONFIG REQUIRED)
find_package(tinyply CONFIG REQUIRED)

add_library(voxelsieve_warnings INTERFACE)
if(MSVC)
  # Standard C++ and UTF-8 sources; C4251/C4275 (members and bases of exported classes need a DLL
  # interface) only matter for mixing compilers, which a C++ API cannot do anyway.
  target_compile_options(voxelsieve_warnings INTERFACE /W4 /permissive- /utf-8 /bigobj
                         /Zc:__cplusplus /Zc:preprocessor /external:W0 /wd4251 /wd4275)
  target_compile_definitions(voxelsieve_warnings INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN
                             _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
  # Every target links voxelsieve_warnings, so every program gets the UTF-8 manifest.
  target_sources(voxelsieve_warnings INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/utf8.manifest)
  if(VOXELSIEVE_WARNINGS_AS_ERRORS)
    target_compile_options(voxelsieve_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(voxelsieve_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
  if(VOXELSIEVE_WARNINGS_AS_ERRORS)
    target_compile_options(voxelsieve_warnings INTERFACE -Werror)
  endif()
endif()

if(WIN32)
  # The library has no export macros: export everything, as on Linux and macOS. Programs, the
  # library and vcpkg's DLLs share one directory, so the tools and tests find them.
  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

add_subdirectory(src)
add_subdirectory(apps/vs-phantom)
add_subdirectory(apps/vs-porosity)
add_subdirectory(apps/vs-report)
add_subdirectory(apps/vs-segment)
add_subdirectory(apps/vs-sieve)
add_subdirectory(apps/vs-studio)
add_subdirectory(apps/vs-surface)
add_subdirectory(apps/vs-compare)
add_subdirectory(apps/vs-synth)

if(VOXELSIEVE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(cmake/Install.cmake)
