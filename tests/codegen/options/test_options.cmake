# D2-A black-box lane: accepted and refused known options on the source path.
#
# The table-driven unit test (`parser.unit`) covers all 47 rows; this driver
# proves the executable verdicts and the value-dependent field rules. It is
# source-only, so a lite build without the parser skips it.

if(NOT DEFINED CODEGEN)
    message(FATAL_ERROR "CODEGEN is required")
endif()
if(NOT DEFINED DATA_DIR)
    message(FATAL_ERROR "DATA_DIR is required")
endif()
if(NOT DEFINED FULL_BUILD)
    set(FULL_BUILD 1)
endif()

function(run_ok outvar errvar)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "Command failed (${result}): ${ARGN}\nstdout:\n${output}\nstderr:\n${error}")
    endif()
    set(${outvar} "${output}" PARENT_SCOPE)
    set(${errvar} "${error}" PARENT_SCOPE)
endfunction()

function(run_fail outvar errvar)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 0)
        message(FATAL_ERROR
            "Command unexpectedly succeeded: ${ARGN}\nstdout:\n${output}\nstderr:\n${error}")
    endif()
    set(${outvar} "${output}" PARENT_SCOPE)
    set(${errvar} "${error}" PARENT_SCOPE)
endfunction()

function(require_contains variable_name expected description)
    string(FIND "${${variable_name}}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "${description}: missing '${expected}' in ${variable_name}")
    endif()
endfunction()

function(require_empty variable_name description)
    if(NOT "${${variable_name}}" STREQUAL "")
        message(FATAL_ERROR
            "${description}: expected empty output, got '${${variable_name}}'")
    endif()
endfunction()

if(NOT FULL_BUILD)
    message(STATUS "codegen.options: skipped (the build has no .proto parser)")
    return()
endif()

# The accepted fixtures. `accept-custom-options.proto` is accepted by policy:
# unresolved custom options stay opaque (P1 exception E13), and the README
# states why protoc would refuse the same text. `accept-aggregate-bodies.proto`
# is the same boundary for the three aggregate-valued standard options
# (`declaration`, `edition_defaults`, `feature_support`): brace balance only,
# no nested field lookup (U-09, owner decision of 2026-10-27).
set(accepted_fixtures
    accept-file-options
    accept-message-options
    accept-field-options
    accept-value-defaults
    accept-cpp-type-spellings
    accept-enum-options
    accept-service-options
    accept-ext-range-options
    accept-custom-options
    accept-aggregate-bodies)

foreach(name ${accepted_fixtures})
    run_ok(accepted_out accepted_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${name}.proto")
    require_empty(accepted_err "The ${name} stderr")
    require_contains(accepted_out "#pragma once"
        "The ${name} generated header")
endforeach()

# An accepted option must not change what the emitter does. `packed` is the one
# known field option with a code-generation effect, so its two values are read
# back from the header of the field fixture.
run_ok(field_out field_err
    ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/accept-field-options.proto")
require_empty(field_err "The accept-field-options stderr")
require_contains(field_out "put_packed_int32(7, x.packed_values);"
    "packed = true selects the packed codec")
require_contains(field_out "put_repeated_int32(8, x.unpacked_values);"
    "packed = false selects the unpacked codec")

# U-54 / R09: `(easypb.cpp).type` is retained only on a field, and the three
# spellings the emitter supports - plain, fully-qualified leading dot, and
# spaces inside the parentheses - normalize to the same option. Reading the
# header proves the accepted fixtures still configure the emitted members.
run_ok(cpp_type_out cpp_type_err
    ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/accept-cpp-type-spellings.proto")
require_empty(cpp_type_err "The accept-cpp-type-spellings stderr")
require_contains(cpp_type_out "PlainBox plain = 0;"
    "The plain (easypb.cpp).type spelling")
require_contains(cpp_type_out "DotBox leading_dot = 0;"
    "The leading-dot (.easypb.cpp).type spelling")
require_contains(cpp_type_out "SpacedBox spaced = 0;"
    "The spaced ( easypb.cpp ).type spelling")

# U-16: proto3 accepts `extend` only for the nine descriptor-option messages
# protoc's `AllowedExtendeeInProto3` names (`descriptor.cc:1105-1131`). The
# declaration is parsed and discarded, so the header still carries the ordinary
# message and the E6 warning says the extension surface is unmodelled. The
# fixture spells all nine names, plus the leading-dot form of one of them.
run_ok(proto3_extend_out proto3_extend_err
    ${CODEGEN} -I "${DATA_DIR}"
    "${DATA_DIR}/accept-proto3-extend-options.proto")
require_contains(proto3_extend_out "struct Payload"
    "The proto3 extend accepted header")
require_contains(proto3_extend_err
    "extend declaration parsed but not stored"
    "The E6 warning for a proto3 descriptor-option extend")

# E14: the validation-only table has no nested option model. protoc accepts a
# dotted sub-field assignment to a singular message option; this frontend
# refuses the name and supports the aggregate spelling instead. The fixture is
# protoc-valid by construction, so this is the recorded stricter boundary, not
# a typo in the fixture.
run_fail(subfield_out subfield_err
    ${CODEGEN} -I "${DATA_DIR}"
    "${DATA_DIR}/policy-feature-support-subfield.proto")
require_empty(subfield_out "The feature_support sub-field stdout")
require_contains(subfield_err
    "Option \"feature_support.edition_introduced\" unknown"
    "The feature_support sub-field refusal")

# One refusal per rule. The expected text is the shared table's sentence, so a
# fixture that starts failing for a different reason is a failure of this lane,
# not a silent pass.
function(expect_refusal name expected)
    run_fail(refused_out refused_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${name}.proto")
    require_empty(refused_out "The ${name} stdout")
    require_contains(refused_err "${expected}" "The ${name} refusal")
endfunction()

expect_refusal(reject-file-string-kind
    "Value must be quoted string for string option")
expect_refusal(reject-file-bool-kind
    "Value must be identifier for boolean option")
expect_refusal(reject-optimize-for-kind
    "Value must be identifier for enum-valued option")
expect_refusal(reject-optimize-for-value
    "has no enum value named")
expect_refusal(reject-jstype-kind
    "jstype is only allowed on int64")
expect_refusal(reject-jstype-value
    "has no enum value named")
expect_refusal(reject-lazy-scalar
    "[lazy = true] can only be specified for submessage fields")
expect_refusal(reject-unverified-lazy-scalar
    "[lazy = true] can only be specified for submessage fields")
expect_refusal(reject-lazy-enum
    "[lazy = true] can only be specified for submessage fields")
# U-54 / R09: `(easypb.cpp).type` is a field option; the owner is part of the
# check, so a statement or bracket list at any other declaration scope refuses
# with a diagnostic that names both the option and the scope.
expect_refusal(reject-cpp-type-file
    "Option \"(easypb.cpp).type\" is a field option and cannot be set at file scope")
expect_refusal(reject-cpp-type-message
    "Option \"(easypb.cpp).type\" is a field option and cannot be set at message scope")
expect_refusal(reject-cpp-type-enum
    "Option \"(easypb.cpp).type\" is a field option and cannot be set at enum scope")
expect_refusal(reject-cpp-type-ext-range
    "Option \"(easypb.cpp).type\" is a field option and cannot be set at extension range scope")
# U-16: only the nine descriptor-option extendees are allowed in proto3; a
# user message keeps the proto2-only refusal.
expect_refusal(reject-proto3-extend-user
    "extend declarations are allowed only in proto2")
expect_refusal(reject-map-entry
    "map_entry should not be set explicitly")
# U-62: a non-boolean value is a value-kind defect first; the row's own
# refusal sentence is reserved for a well-typed value.
expect_refusal(reject-map-entry-kind
    "Value must be identifier for boolean option \"map_entry\"")
# U-08: the default `packed = false` is accepted on a map (the accepted
# fixture above), while the non-default `true` stays refused.
expect_refusal(reject-map-packed
    "map fields cannot have default or packed options")
expect_refusal(reject-ext-range-unverified-declaration
    "cannot mark the extension range as UNVERIFIED")
expect_refusal(reject-file-unknown-option
    "Option \"java_packages\" unknown")
expect_refusal(reject-file-option-placement
    "Option \"java_package\" unknown")
expect_refusal(reject-field-option-on-enum
    "Option \"packed\" unknown")
expect_refusal(reject-field-bracket-placement
    "Option \"java_package\" unknown")
expect_refusal(reject-oneof-option
    "Option \"deprecated\" unknown")
expect_refusal(reject-method-option-on-service
    "Option \"idempotency_level\" unknown")
expect_refusal(reject-ext-range-option-placement
    "Option \"verification\" unknown")
expect_refusal(reject-enum-value-option-placement
    "Option \"jstype\" unknown")
expect_refusal(reject-file-option-duplicate
    "Option \"java_package\" was already set")
expect_refusal(reject-field-option-duplicate
    "Option \"deprecated\" was already set")
expect_refusal(reject-custom-leading-identifier
    "Option \"file_scope.(vendor.file)\" unknown")

# U-09: the documented boundary is brace balance, not "anything goes". The
# accepted half is `accept-aggregate-bodies` above; a body that never closes
# is still refused by the same parser. U-55/SD-1 has its own registration
# (`codegen.options.legacy_json_xfail`), so the option stays out of these two
# lists.
expect_refusal(reject-aggregate-unterminated
    "unterminated aggregate option value")
