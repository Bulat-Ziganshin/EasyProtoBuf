# One descriptor-path header for the names lane.
#
# Required arguments:
#   CODEGEN the standalone codegen executable
#   INPUT  the .pbs descriptor set
#   OUTPUT the header to write
#
# Optional:
#   OPTIONS the generation options as one space-separated string (for example
#           `--no-has-fields`). The build draws them from family_cases.cmake,
#           so a compiled variant header is the same header the names driver
#           generates for that accepted case.

foreach(required_var CODEGEN INPUT OUTPUT)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

set(extra_options)
# `-` is the family table's "default option set", not an argument to pass
# through: the parser used to drop a bare dash silently, so relying on it
# worked by accident. It is not an argument (CR-34/B9).
if(DEFINED OPTIONS AND NOT "${OPTIONS}" STREQUAL "" AND
   NOT "${OPTIONS}" STREQUAL "-")
    separate_arguments(extra_options UNIX_COMMAND "${OPTIONS}")
endif()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

execute_process(
    COMMAND "${CODEGEN}" --descriptor-set ${extra_options} "${INPUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE generated
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "Names header generation failed (${result})\nstdout:\n${generated}\nstderr:\n${error}")
endif()

file(WRITE "${OUTPUT}" "${generated}")
