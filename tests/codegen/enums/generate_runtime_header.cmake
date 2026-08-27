foreach(required_var CODEGEN INPUT OUTPUT)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

execute_process(
    COMMAND "${CODEGEN}" --descriptor-set "${INPUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE generated
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "Enum runtime generation failed (${result})\nstdout:\n${generated}\nstderr:\n${error}")
endif()

file(WRITE "${OUTPUT}" "${generated}")
