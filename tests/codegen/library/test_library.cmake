# The library lane: a caller that links the core gets the CLI's answer.
#
# One CTest entry for the four rows of the plan-5 follow-up's session S6
# (W-11, W-47, W-48, W-58). The C++ half lives in test_library.cpp; this
# script runs it and drives the CLI half:
#
#   1. the library suite passes;
#   2. the library's default-option header is byte-identical to the CLI's for
#      the same descriptor set (W-11). A library caller cannot get a parallel
#      answer;
#   3. compatibility mode refuses a message default and a non-identifier enum
#      default (W-47), while still accepting an external type with no default
#      and an external enum with a valid identifier default;
#   4. complete mode refuses the thin set's unresolved type (the --target
#      control), and the source path refuses the same shape.
#
# The W-11 header is also compiled by the build: the emit mode writes it in a
# custom command, because cl.exe started from a test script outside the Visual
# Studio environment cannot find its own standard library headers.
#
# Required arguments:
#   CODEGEN       the standalone codegen executable
#   LIBRARY_TEST  the C++ API suite binary
#   DATA_DIR      the fixture directory
#   WORK_DIR      the scratch directory
#   FULL_BUILD    1 when the proto parser is available

foreach(required_var CODEGEN LIBRARY_TEST DATA_DIR WORK_DIR)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()
if(NOT DEFINED FULL_BUILD)
    set(FULL_BUILD 1)
endif()

function(run_ok output_var error_var)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "Command failed (${result}): ${ARGN}\nstdout:\n${output}\nstderr:\n${error}")
    endif()
    set(${output_var} "${output}" PARENT_SCOPE)
    set(${error_var} "${error}" PARENT_SCOPE)
endfunction()

function(require_contains variable_name expected description)
    string(FIND "${${variable_name}}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${description}: missing '${expected}' in ${variable_name}")
    endif()
endfunction()

function(require_empty variable_name description)
    if(NOT "${${variable_name}}" STREQUAL "")
        message(FATAL_ERROR "${description}: expected empty output, got '${${variable_name}}'")
    endif()
endfunction()

# A refusal is three things at once: a non-zero exit, an empty stdout (no
# half-written header escapes) and a diagnostic that names the offending
# thing.
function(expect_refusal description expected)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 0)
        message(FATAL_ERROR
            "${description}: the command unexpectedly succeeded: ${ARGN}\nstdout:\n${output}\nstderr:\n${error}")
    endif()
    if(NOT "${output}" STREQUAL "")
        message(FATAL_ERROR
            "${description}: the refusal wrote to stdout: ${output}")
    endif()
    string(FIND "${error}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "${description}: missing '${expected}' in the diagnostic:\n${error}")
    endif()
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# --- the C++ API suite -------------------------------------------------------

execute_process(
    COMMAND "${LIBRARY_TEST}" "${DATA_DIR}"
    RESULT_VARIABLE library_result
    OUTPUT_VARIABLE library_output
    ERROR_VARIABLE library_error)
if(NOT library_result EQUAL 0)
    message(FATAL_ERROR
        "The library API suite failed (${library_result})\nstdout:\n${library_output}\nstderr:\n${library_error}")
endif()

# --- L3: library == CLI for the same descriptor set -------------------------

set(library_header "${WORK_DIR}/library-options-default.pb.hpp")
execute_process(
    COMMAND "${LIBRARY_TEST}" --emit-options-default "${library_header}"
            "${DATA_DIR}"
    RESULT_VARIABLE emit_result
    OUTPUT_VARIABLE emit_output
    ERROR_VARIABLE emit_error)
if(NOT emit_result EQUAL 0)
    message(FATAL_ERROR
        "The library emit mode failed (${emit_result})\nstdout:\n${emit_output}\nstderr:\n${emit_error}")
endif()

run_ok(cli_header cli_error
    ${CODEGEN} --descriptor-set "${DATA_DIR}/options-default.pbs")
require_empty(cli_error "The default-option CLI stderr")
file(WRITE "${WORK_DIR}/cli-options-default.pb.hpp" "${cli_header}")
file(READ "${library_header}" library_text)
if(NOT cli_header STREQUAL library_text)
    message(FATAL_ERROR
        "The library and the CLI disagree on the default-option header")
endif()
require_contains(library_text "::std::vector< ::int32_t>"
    "The default-option header must spell the repeated type")
require_contains(library_text "::std::map< ::int32_t,::std::string>"
    "The default-option header must spell the map type")

# --- L6, L7: compatibility mode refuses the default the kind forbids --------

expect_refusal("The descriptor path must refuse a message default on an external type"
    "message fields cannot have defaults"
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/w47-external-message-default.pbs")

expect_refusal("The descriptor path must refuse a non-identifier enum default on an external type"
    "Default value for an enum field must be an identifier."
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/w47-external-enum-1a.pbs")

# --- L7b, L8: the tolerated shapes stay accepted -----------------------------

run_ok(enum_default_out enum_default_err
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/w47-external-enum-default.pbs")
file(WRITE "${WORK_DIR}/w47-external-enum-default.pb.hpp"
     "${enum_default_out}")
require_contains(enum_default_out "::dep::E e = ::dep::E::A;"
    "The valid external enum default must be kept verbatim")
require_contains(enum_default_err "not declared in the linked set"
    "The compatibility warning for the external enum")

run_ok(external_out external_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/w47-external.pbs")
file(WRITE "${WORK_DIR}/w47-external.pb.hpp" "${external_out}")
require_contains(external_out "::dep::Missing x;"
    "The external message field without a default must stay accepted")
require_contains(external_err "not declared in the linked set"
    "The compatibility warning for the external message")

# --- L9, L10: the two controls that must still refuse ------------------------

expect_refusal("The complete-descriptor path must refuse the thin set's unresolved type"
    "unresolved type .dep.E"
    ${CODEGEN} --descriptor-set --target w47-external-enum-1a.proto
    "${DATA_DIR}/w47-external-enum-1a.pbs")

if(FULL_BUILD)
    expect_refusal("The source path must refuse an unresolved type with a default"
        "unresolved type Missing"
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/w47-source-default.proto")
endif()
