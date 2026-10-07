# Runs PROGRAM with ARGS (a ;-list) and INPUT as its standard input, the portable form of
# `printf ... | program` for CLI tests: cmake -DPROGRAM=... -DARGS=... -DINPUT=... -P this file.
execute_process(COMMAND "${PROGRAM}" ${ARGS} INPUT_FILE "${INPUT}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "${PROGRAM} exited with ${result}")
endif()
