# One descriptor-path header for the validity lane.
#
# Required arguments:
#   CODEGEN the standalone codegen executable
#   INPUT  the .pbs descriptor set
#   OUTPUT the header to write
# Optional:
#   OPTIONS extra codegen flags (a CMake list), for the --no-has-fields half
#           of the default-value family

foreach(required_var CODEGEN INPUT OUTPUT)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

execute_process(
    COMMAND "${CODEGEN}" --descriptor-set ${OPTIONS} "${INPUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE generated
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "Validity header generation failed (${result})\nstdout:\n${generated}\nstderr:\n${error}")
endif()

file(WRITE "${OUTPUT}" "${generated}")
