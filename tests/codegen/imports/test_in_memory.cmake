# Plan 5's in-memory lane driver: prepare the strict `.pbs` reference output
# with the standalone CLI, then run the core-only in-memory generator from an
# empty working directory and require byte equality with that output.
#
# Required arguments:
#   CODEGEN         the standalone codegen executable
#   IN_MEMORY       the core-only in-memory test binary
#   TESTS_SOURCE_DIR  the tests/ source directory (tracked fixtures)
#   WORK_DIR        a scratch root below the build tree

foreach(required_var CODEGEN IN_MEMORY TESTS_SOURCE_DIR WORK_DIR)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

set(project_pbs "${TESTS_SOURCE_DIR}/codegen/imports/data/project.pbs")
set(multi_pbs "${TESTS_SOURCE_DIR}/codegen/parser/data/integration-multi.pbs")
set(strict_dir "${WORK_DIR}/strict")
set(empty_dir "${WORK_DIR}/empty-cwd")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${strict_dir}")
file(MAKE_DIRECTORY "${empty_dir}")

# The strict reference: the CLI loads the same two containers, links complete
# and publishes the same two selected targets. This is a descriptor-only
# command, so it also runs in the lite profile.
execute_process(
    COMMAND "${CODEGEN}" --descriptor-set "${project_pbs}" "${multi_pbs}"
            --target api/report.proto
            --target integration-proto3.proto
            --out-dir "${strict_dir}"
    RESULT_VARIABLE strict_result
    OUTPUT_VARIABLE strict_out
    ERROR_VARIABLE strict_err)
if(NOT strict_result EQUAL 0)
    message(FATAL_ERROR
        "Strict .pbs reference generation failed (${strict_result})\n"
        "stdout:\n${strict_out}\nstderr:\n${strict_err}")
endif()
if(NOT "${strict_out}" STREQUAL "")
    message(FATAL_ERROR "Strict .pbs reference wrote to stdout: ${strict_out}")
endif()

# The in-memory half runs from a directory that holds nothing: no fixture, no
# scratch, no output tree may be reachable through the current directory.
execute_process(
    COMMAND "${IN_MEMORY}" "${TESTS_SOURCE_DIR}" "${strict_dir}"
    WORKING_DIRECTORY "${empty_dir}"
    RESULT_VARIABLE memory_result
    OUTPUT_VARIABLE memory_out
    ERROR_VARIABLE memory_err)
if(NOT memory_result EQUAL 0)
    message(FATAL_ERROR
        "In-memory generation differed from the strict .pbs output "
        "(${memory_result})\nstdout:\n${memory_out}\nstderr:\n${memory_err}")
endif()

# The empty directory stays empty: the run may not write through its current
# directory, and no scratch may survive there.
file(GLOB leftovers "${empty_dir}/*")
if(leftovers)
    message(FATAL_ERROR
        "The in-memory run wrote through its empty working directory: "
        "${leftovers}")
endif()
