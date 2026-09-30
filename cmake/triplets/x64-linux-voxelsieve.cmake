# Linux, shared libraries, release builds only (ADR 0016). Shared so that OpenVDB's grid registry
# exists once when libvoxelsieve, the tools and plugins use it; release only to halve build time.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
