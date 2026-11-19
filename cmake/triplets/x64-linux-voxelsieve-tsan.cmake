# Linux like x64-linux-voxelsieve, with every library built for ThreadSanitizer (preset tsan).
# TBB hands tasks from thread to thread inside its own library; TSan sees that hand-over, and so
# does not report every parallel_for as a race, only when TBB itself is instrumented.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_C_FLAGS "-fsanitize=thread -g1")
set(VCPKG_CXX_FLAGS "-fsanitize=thread -g1")
set(VCPKG_LINKER_FLAGS "-fsanitize=thread")
