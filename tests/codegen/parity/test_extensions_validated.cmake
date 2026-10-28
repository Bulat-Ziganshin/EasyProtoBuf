# V-09: extension members on the descriptor door are a validated surface now.
# The decoder used to count `DescriptorProto.extension` and skip
# `FileDescriptorProto.extension`; it now decodes every member, and
# `validate_extension_fields` asks protoc's rules: the extendee is present and
# is a message, the tag lies inside the extendee's declared extension range, no
# tag is declared twice for one extendee, and a MessageSet extension is an
# optional message. A valid set is still not emitted, so it keeps the recorded
# E6 warning.
#
# The source door keeps its accepted E6 decision: the parser validates an
# `extend` body as text and discards it, so an unknown extendee still generates
# with the warning. That is the half of CR-25 that remains accepted; the
# descriptor half is validated here.
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

# A refusal is a non-zero exit, an empty stdout and the expected sentence.
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

# An extension member with no extendee. protoc 30.1 refuses the bytes
# ("FieldDescriptorProto.extendee not set for extension field.") and the
# descriptor door now answers the same sentence.
expect_refusal(
    "The descriptor path must refuse an extension member with no extendee"
    "FieldDescriptorProto.extendee not set for extension field."
    ${CODEGEN} --descriptor-set "${DATA_DIR}/ext-member-no-extendee.pbs")

# An extension member whose number is outside the extendee's declared range.
expect_refusal(
    "The descriptor path must refuse an extension member outside the declared range"
    "\"parity.M\" does not declare 5 as an extension number."
    ${CODEGEN} --descriptor-set "${DATA_DIR}/ext-member-out-of-range.pbs")

# The accepted half: the source door keeps the E6 decision for an `extend` of a
# type that does not exist. The warning is part of the accepted shape; if it
# disappears, the accepted register (E6) must be re-evaluated.
if(FULL_BUILD)
    execute_process(
        COMMAND ${CODEGEN} -I "${DATA_DIR}"
            "${DATA_DIR}/ext-unvalidated.proto"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "The source path must still accept an extend declaration of an unknown type (E6).\n"
            "stdout:\n${output}\nstderr:\n${error}")
    endif()
    string(FIND "${error}" "extend declaration parsed but not stored" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "The source-path E6 warning is missing from:\n${error}")
    endif()
endif()
