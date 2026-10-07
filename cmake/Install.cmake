# Installation: the command-line tools and libvoxelsieve, plus the example inspection order and
# the license files. With VOXELSIEVE_INSTALL_RUNTIME_DEPS the shared libraries from vcpkg (and on
# Windows the MSVC runtime) are installed too, so the tree runs on a machine without a build;
# that is how the release packages are made (.github/workflows/release.yml).

set(VOXELSIEVE_PROGRAMS
  vs-compare vs-phantom vs-porosity vs-report vs-segment vs-sieve vs-studio vs-surface vs-synth)

install(TARGETS voxelsieve ${VOXELSIEVE_PROGRAMS}
  RUNTIME_DEPENDENCY_SET voxelsieve_runtime
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES examples/inspection_order.json DESTINATION ${CMAKE_INSTALL_DATADIR}/voxelsieve/examples)
install(FILES LICENSE NOTICE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/voxelsieve)

if(VOXELSIEVE_INSTALL_RUNTIME_DEPS)
  set(_vs_vcpkg_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
  # Libraries of the operating system stay out: the C and C++ runtime of Linux, the system
  # frameworks of macOS and the DLLs of Windows itself.
  install(RUNTIME_DEPENDENCY_SET voxelsieve_runtime
    DIRECTORIES "${_vs_vcpkg_dir}/bin" "${_vs_vcpkg_dir}/lib"
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES
      "^/lib/" "^/lib64/" "^/usr/lib/" "^/usr/lib64/" "^/System/" "[/\\\\][Ss]ystem32[/\\\\]"
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  if(WIN32)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
    include(InstallRequiredSystemLibraries)
  endif()
endif()
