# macOS on Apple silicon, shared libraries, release builds only (ADR 0016); see
# x64-linux-voxelsieve for why shared.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 13.3)
set(VCPKG_BUILD_TYPE release)
