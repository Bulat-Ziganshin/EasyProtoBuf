if(NOT DEFINED BUILD_DIR OR NOT DEFINED TARGET_NAME OR
   NOT DEFINED EXPECTED_DIAGNOSTIC)
    message(FATAL_ERROR
        "BUILD_DIR, TARGET_NAME and EXPECTED_DIAGNOSTIC are required")
endif()

set(build_command
    "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET_NAME}")
if(DEFINED BUILD_CONFIG AND NOT BUILD_CONFIG STREQUAL "")
    list(APPEND build_command --config "${BUILD_CONFIG}")
endif()

execute_process(
    COMMAND ${build_command}
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)

set(build_output "${build_stdout}\n${build_stderr}")
if(build_result EQUAL 0)
    message(FATAL_ERROR
        "${TARGET_NAME} compiled, but compilation was expected to fail")
endif()

string(FIND "${build_output}" "${EXPECTED_DIAGNOSTIC}" diagnostic_offset)
if(diagnostic_offset EQUAL -1)
    message(FATAL_ERROR
        "${TARGET_NAME} failed for the wrong reason. Expected diagnostic: "
        "${EXPECTED_DIAGNOSTIC}\nCompiler output:\n${build_output}")
endif()
