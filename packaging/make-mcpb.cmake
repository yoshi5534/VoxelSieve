# Packs an installed VoxelSieve tree into an MCP bundle (.mcpb) for one-click installation in
# Claude Desktop and other clients that read MCP bundles: a ZIP archive with manifest.json at its
# root next to bin/ and lib/.
#   cmake -DSTAGE=<install prefix> -DOUT=<file.mcpb> -DVERSION=<x.y.z> -DPLATFORM=<linux|darwin|win32>
#         -P packaging/make-mcpb.cmake
foreach(var STAGE OUT VERSION PLATFORM)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "make-mcpb.cmake needs -D${var}=...")
  endif()
endforeach()
if(PLATFORM STREQUAL "win32")
  set(EXE ".exe")
else()
  set(EXE "")
endif()
# Relative paths are taken from the current directory, which file(GLOB) would not do on its own.
get_filename_component(STAGE "${STAGE}" ABSOLUTE)
get_filename_component(out_path "${OUT}" ABSOLUTE)
if(NOT EXISTS "${STAGE}/bin/vs-studio${EXE}")
  message(FATAL_ERROR "${STAGE} holds no installed vs-studio${EXE}")
endif()
configure_file(${CMAKE_CURRENT_LIST_DIR}/manifest.json.in "${STAGE}/manifest.json" @ONLY)
file(GLOB entries RELATIVE "${STAGE}" "${STAGE}/*")
execute_process(COMMAND ${CMAKE_COMMAND} -E tar cf "${out_path}" --format=zip ${entries}
                WORKING_DIRECTORY "${STAGE}" RESULT_VARIABLE result)
file(REMOVE "${STAGE}/manifest.json")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Packing ${OUT} failed")
endif()
# cmake -E tar succeeds even when it packed nothing; the bundle must hold the manifest and server.
execute_process(COMMAND ${CMAKE_COMMAND} -E tar tf "${out_path}" OUTPUT_VARIABLE listing
                RESULT_VARIABLE result)
string(FIND "${listing}" "manifest.json" manifest_at)
string(FIND "${listing}" "bin/vs-studio${EXE}" server_at)
if(NOT result EQUAL 0 OR manifest_at EQUAL -1 OR server_at EQUAL -1)
  message(FATAL_ERROR "${OUT} lacks manifest.json or bin/vs-studio${EXE}:\n${listing}")
endif()
