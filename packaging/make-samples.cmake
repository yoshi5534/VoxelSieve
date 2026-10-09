# Writes the sample data of a release: a synthetic CT scan of the sample housing casting with
# shrinkage cavities and a zone of loosened microstructure, its CAD model and the example
# inspection order, as a ZIP for the quickstart (docs/quickstart.md). The scan's sidecar holds the
# ground truth of the defects, so results can be checked against it.
#
#   cmake -DSYNTH=<path to vs-synth> -DORDER=<inspection_order.json> -DOUT=<samples.zip>
#         -P make-samples.cmake

foreach(var SYNTH ORDER OUT)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "make-samples.cmake needs -D${var}=...")
  endif()
endforeach()
get_filename_component(out_path "${OUT}" ABSOLUTE)
get_filename_component(order_path "${ORDER}" ABSOLUTE)
get_filename_component(out_dir "${out_path}" DIRECTORY)
set(stage "${out_dir}/voxelsieve-samples")
file(REMOVE_RECURSE "${stage}")
file(MAKE_DIRECTORY "${stage}")

execute_process(
  COMMAND "${SYNTH}" --part housing --voxel-size 0.25 --lunker 3 --loosening 1 --noise 400
          --blur 0.1 --cupping 0.05 --stl housing.stl --out housing
  WORKING_DIRECTORY "${stage}" RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "vs-synth failed: ${result}")
endif()
file(COPY "${order_path}" DESTINATION "${stage}")
file(WRITE "${stage}/README.txt"
"VoxelSieve sample data

housing.raw            synthetic CT scan of a cast housing (uint16, little endian, x fastest)
housing.json           its sidecar: dimensions, voxel size and the ground truth of the defects
                       (three shrinkage cavities, one zone of loosened microstructure)
housing.stl            the CAD model of the housing in mm, for the nominal-actual comparison
inspection_order.json  an example inspection order with acceptance limits (BDG P 202)

Start with docs/quickstart.md: https://github.com/yoshi5534/VoxelSieve/blob/main/docs/quickstart.md
")

file(REMOVE "${out_path}")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E tar cf "${out_path}" --format=zip voxelsieve-samples
  WORKING_DIRECTORY "${out_dir}" RESULT_VARIABLE result)
file(REMOVE_RECURSE "${stage}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Packing ${out_path} failed")
endif()
# cmake -E tar succeeds even when it packs nothing; check the content.
execute_process(COMMAND ${CMAKE_COMMAND} -E tar tf "${out_path}" OUTPUT_VARIABLE listing)
foreach(file housing.raw housing.json housing.stl inspection_order.json README.txt)
  string(FIND "${listing}" "voxelsieve-samples/${file}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${out_path} lacks ${file}")
  endif()
endforeach()
message(STATUS "Wrote ${out_path}")
