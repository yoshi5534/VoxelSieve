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
configure_file(${CMAKE_CURRENT_LIST_DIR}/manifest.json.in ${STAGE}/manifest.json @ONLY)
file(GLOB entries RELATIVE ${STAGE} ${STAGE}/*)
get_filename_component(out_path "${OUT}" ABSOLUTE)
execute_process(COMMAND ${CMAKE_COMMAND} -E tar cf "${out_path}" --format=zip ${entries}
                WORKING_DIRECTORY ${STAGE} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Packing ${OUT} failed")
endif()
file(REMOVE ${STAGE}/manifest.json)
