# Windows, DLLs, release builds only (ADR 0016): the counterpart of x64-linux-voxelsieve, so that
# OpenVDB's grid registry exists once in openvdb.dll for libvoxelsieve, the tools and plugins.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)
