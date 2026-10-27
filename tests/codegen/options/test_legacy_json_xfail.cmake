# The U-55 / SD-1 xfail: `deprecated_legacy_json_field_conflicts` is accepted
# and has no effect.
#
# `descriptor.proto` marks the field on `MessageOptions` (field 11) and on
# `EnumOptions` (field 6) `[deprecated = true]`. Stage decision SD-1
# (2026-10-27, narrowing the owner decision for U-55) keeps both rows in the
# known-option table: a migration leftover is accepted and nothing reads the
# value. `codegen/OPTIONS.md` ("deprecated_legacy_json_field_conflicts is a
# no-op") is the durable description of the decision.
#
# The test pins it in the three places the decision can drift:
#
#   1. the option is accepted on BOTH owners (message and enum): exit 0, no
#      diagnostic, and the header still carries both declarations;
#   2. the bracketed custom `json_name` the option enables in protoc
#      (`json_name = "[foo]"`) is still refused;
#   3. the proto2 folded enum-name collision the option downgrades to a warning
#      in protoc (`E_FOO = 0; FOO = 1;`) is still refused.
#
# Facts 2 and 3 assert the CURRENT direction of the no-op. A failure here is
# not "the product broke": it means one of the two refused shapes now passes,
# so the no-op decision has to be re-evaluated and `OPTIONS.md` updated -
# either the option is implemented, or the new acceptance is documented as a
# divergence. Do not delete or invert any assertion to make the test green
# without that documentation change.
#
# The lane is source-only (aggregate option text and a source `json_name` are
# source shapes), so the driver returns early when the build has no parser.
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

if(NOT FULL_BUILD)
    message(STATUS
        "codegen.options.legacy_json_xfail: skipped (the build has no .proto parser)")
    return()
endif()

function(run_codegen output_var error_var result_var name)
    execute_process(
        COMMAND ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${name}.proto"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    set(${output_var} "${output}" PARENT_SCOPE)
    set(${error_var} "${error}" PARENT_SCOPE)
    set(${result_var} "${result}" PARENT_SCOPE)
endfunction()

function(require_contains variable_name expected description)
    string(FIND "${${variable_name}}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "${description}: missing '${expected}' in ${variable_name}")
    endif()
endfunction()

# Fact 1: acceptance on both owners. A refusal means a catalog row was dropped
# (or the option became validated against a value it never had); SD-1 keeps
# the option accepted, so the tool, not the fixture, is wrong.
run_codegen(option_out option_err option_result legacy-json-options)
if(NOT option_result EQUAL 0)
    message(FATAL_ERROR
        "The option is no longer accepted (exit ${option_result}).\n"
        "U-55 is decided as a documented no-op (SD-1, 2026-10-27): both "
        "catalog rows stay, the option is accepted and has no effect. If the "
        "refusal is intended, re-open the decision and update OPTIONS.md "
        "before changing this test.\n"
        "stdout:\n${option_out}\nstderr:\n${option_err}")
endif()
if(NOT "${option_err}" STREQUAL "")
    message(FATAL_ERROR
        "The accepted option must stay diagnostic-free (SD-1 adds no warning):\n"
        "${option_err}")
endif()
require_contains(option_out "struct LegacyJsonMessage"
    "U-55: the message that carries the option")
require_contains(option_out "enum LegacyJsonEnum"
    "U-55: the enum that carries the option")

# Fact 2: the bracket `json_name` protoc accepts under the option stays
# refused. Acceptance is the direction change the xfail exists to catch.
run_codegen(bracket_out bracket_err bracket_result legacy-json-bracket-name)
if(bracket_result EQUAL 0)
    message(FATAL_ERROR
        "The bracketed custom json_name now PASSES under the no-op option.\n"
        "protoc 30.1 accepts this shape exactly while "
        "deprecated_legacy_json_field_conflicts is set; Codegen accepting it "
        "too means the legacy behaviour was implemented (or the no-op became "
        "a partial one). Re-evaluate the SD-1 no-op decision, update the "
        "OPTIONS.md section, and only then change this assertion.\n"
        "stdout:\n${bracket_out}")
endif()
if(NOT "${bracket_out}" STREQUAL "")
    message(FATAL_ERROR
        "The bracketed custom json_name refusal must leave stdout empty:\n"
        "${bracket_out}")
endif()
require_contains(bracket_err "JSON names may not start with '[' and end with ']'"
    "U-55: the bracketed custom json_name refusal")

# Fact 3: the proto2 folded enum-name collision protoc downgrades under the
# option stays refused.
run_codegen(fold_out fold_err fold_result legacy-json-enum-fold)
if(fold_result EQUAL 0)
    message(FATAL_ERROR
        "The proto2 folded enum-name collision now PASSES under the no-op "
        "option.\nprotoc 30.1 downgrades this shape to a warning exactly "
        "while deprecated_legacy_json_field_conflicts is set; Codegen "
        "accepting it too means the legacy behaviour was implemented (or the "
        "no-op became a partial one). Re-evaluate the SD-1 no-op decision, "
        "update the OPTIONS.md section, and only then change this assertion.\n"
        "stdout:\n${fold_out}")
endif()
if(NOT "${fold_out}" STREQUAL "")
    message(FATAL_ERROR
        "The folded enum-name refusal must leave stdout empty:\n${fold_out}")
endif()
require_contains(fold_err "has the same name as"
    "U-55: the folded enum-name refusal")
