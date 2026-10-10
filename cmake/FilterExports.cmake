# Keeps the exports of a module definition file that belong to VoxelSieve: symbols whose
# decorated name mentions the namespace `voxelsieve`. Run with -DINPUT=<all.def> -DOUTPUT=<def>.
file(STRINGS "${INPUT}" lines)
set(kept "EXPORTS\n")
set(count 0)
foreach(line IN LISTS lines)
  if(line MATCHES "voxelsieve")
    string(APPEND kept "${line}\n")
    math(EXPR count "${count} + 1")
  endif()
endforeach()
list(LENGTH lines all)
math(EXPR all "${all} - 1")  # the EXPORTS line
message(STATUS "Exporting ${count} of ${all} symbols")
file(WRITE "${OUTPUT}" "${kept}")
