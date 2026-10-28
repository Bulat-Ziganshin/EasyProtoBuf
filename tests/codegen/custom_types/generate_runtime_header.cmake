if(NOT DEFINED CODEGEN OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "CODEGEN, INPUT and OUTPUT are required")
endif()

# V-10: the global container templates, when the caller wants the documented
# grammar exercised instead of the defaults.
set(template_options)
if(DEFINED REPEATED_TYPE AND NOT "${REPEATED_TYPE}" STREQUAL "")
    list(APPEND template_options --repeated-type "${REPEATED_TYPE}")
endif()
if(DEFINED MAP_TYPE AND NOT "${MAP_TYPE}" STREQUAL "")
    list(APPEND template_options --map-type "${MAP_TYPE}")
endif()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
execute_process(
    COMMAND "${CODEGEN}" --descriptor-set ${template_options} "${INPUT}"
    RESULT_VARIABLE result
    OUTPUT_FILE "${OUTPUT}"
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Codegen failed (${result}): ${error}")
endif()
