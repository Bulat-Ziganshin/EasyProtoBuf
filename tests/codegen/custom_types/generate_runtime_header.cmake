if(NOT DEFINED CODEGEN OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "CODEGEN, INPUT and OUTPUT are required")
endif()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
execute_process(
    COMMAND "${CODEGEN}" --descriptor-set "${INPUT}"
    RESULT_VARIABLE result
    OUTPUT_FILE "${OUTPUT}"
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Codegen failed (${result}): ${error}")
endif()
