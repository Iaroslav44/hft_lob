# Require the expected rejection, rather than a process crash or sanitizer failure.
execute_process(COMMAND "${PROGRAM}" ${ARGUMENTS}
                RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
if(NOT "${status}" STREQUAL "1")
  message(FATAL_ERROR "Expected exit code 1, got ${status}\n${output}${error}")
endif()
if(NOT error MATCHES "${EXPECTED_ERROR}")
  message(FATAL_ERROR "Expected diagnostic '${EXPECTED_ERROR}', got:\n${error}")
endif()
