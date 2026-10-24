# The accepted CR-25 xfail: extension declarations are neither validated nor
# decoded today (triage register E6). The test asserts the CURRENT direction:
# both doors accept a shape protoc 30.1 refuses. It passes while the defect is
# present and fails when the direction changes.
#
# A failure here is not "the product broke": it means extensions became a
# validated surface, so the accepted decision, the E6 citation in
# property-catalogue.md and the CR-25 row must be re-evaluated and this test
# retired (or inverted). The message says so, because the next reader will not
# have the triage in front of them.
#
# Required arguments:
#   CODEGEN     the standalone codegen executable
#   DATA_DIR    the fixture directory
#   FULL_BUILD  1 when the proto parser is available

foreach(required_var CODEGEN DATA_DIR)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()
if(NOT DEFINED FULL_BUILD)
    set(FULL_BUILD 1)
endif()

function(require_contains variable_name expected description)
    string(FIND "${${variable_name}}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${description}: missing '${expected}' in ${variable_name}")
    endif()
endfunction()

# A shape the accepted decision says this tool accepts. A non-zero exit is the
# direction change the xfail exists to catch.
function(expect_still_accepted description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${description}: the tool now REFUSES this shape (exit ${result}).\n"
            "CR-25 is registered as accepted: extension declarations are neither "
            "validated nor decoded (triage register E6). If the refusal is "
            "intended, remove the accepted decision and the E6 citation, delete "
            "or invert this xfail, and record the new rule against CR-25.\n"
            "stdout:\n${output}\nstderr:\n${error}")
    endif()
    set(last_output "${output}" PARENT_SCOPE)
    set(last_error "${error}" PARENT_SCOPE)
endfunction()

# Descriptor shapes protoc refuses: an extension member with no extendee, and
# one whose number is outside the extendee's declared extension range. The
# model counts DescriptorProto.extension and decodes no member, so neither
# question can be asked today.
expect_still_accepted(
    "The descriptor path must still accept an extension member with no extendee"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/ext-member-no-extendee.pbs")
expect_still_accepted(
    "The descriptor path must still accept an extension member outside the declared range"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/ext-member-out-of-range.pbs")

# The source door: `extend` is parsed, its fields are discarded, and the parser
# emits one warning. protoc refuses an unknown extendee. The warning is part of
# the accepted shape - if it disappears while the declaration is still not
# validated, the console contract changed and the row needs a new look.
if(FULL_BUILD)
    expect_still_accepted(
        "The source path must still accept an extend declaration"
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/ext-unvalidated.proto")
    require_contains(last_error "extend declaration parsed but not stored"
        "The source-path CR-25 warning")
endif()
