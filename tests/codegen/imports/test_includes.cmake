# Plan 5's include-planner lane (invariants I2/I3/I4), driven through the
# standalone CLI on committed complete `.pbs` closures:
#
#   I2 positive     - a custom-option target whose only compiler-support
#                     import is `easypb/options.proto` generates without an
#                     options or descriptor include, and the complete-closure
#                     output is byte-identical to the compatibility output of
#                     the same single-file set (the exception changes no byte);
#   I2 counterexample - a real field naming a support-file message restores
#                     the dependency include;
#   I3              - a public re-export of a support import restores the
#                     include even when nothing references its declarations;
#   I4              - the support file stays in the semantic graph: the same
#                     fixture with its descriptor import missing is refused
#                     with the linker's missing-import diagnostic.
#
# Required arguments: CODEGEN, DATA_DIR (the include_rules fixtures),
# CUSTOM_TYPES_PBS (the compatibility single-file set of the same schema).

foreach(required_var CODEGEN DATA_DIR CUSTOM_TYPES_PBS)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

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

function(run_fail output_var error_var)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 0)
        message(FATAL_ERROR
            "Command unexpectedly succeeded: ${ARGN}\nstdout:\n${output}\nstderr:\n${error}")
    endif()
    set(${output_var} "${output}" PARENT_SCOPE)
    set(${error_var} "${error}" PARENT_SCOPE)
endfunction()

function(require_contains variable_name expected description)
    string(FIND "${${variable_name}}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${description}: missing '${expected}'")
    endif()
endfunction()

function(require_absent variable_name forbidden description)
    string(FIND "${${variable_name}}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "${description}: found '${forbidden}'")
    endif()
endfunction()

function(require_empty variable_name description)
    if(NOT "${${variable_name}}" STREQUAL "")
        message(FATAL_ERROR
            "${description}: expected empty output, got '${${variable_name}}'")
    endif()
endfunction()

# --- I2: the metadata-only exception, positive half -------------------------

run_ok(complete_out complete_err ${CODEGEN} --descriptor-set
    "${DATA_DIR}/option_closure.pbs" --target custom_types.proto)
require_absent(complete_out "#include \"easypb/options.pb.hpp\""
    "I2: an options-only target must not include the options header")
require_absent(complete_out "#include \"google/protobuf/descriptor.pb.hpp\""
    "I2: the transitive descriptor support file stays metadata-only")
require_contains(complete_out "std::deque<"
    "I2: the target still generates its customized fields")

# The exception changes no byte of the generated body: the complete-closure
# output equals the single-file compatibility output of the same schema.
run_ok(compat_out compat_err ${CODEGEN} --descriptor-set "${CUSTOM_TYPES_PBS}")
if(NOT complete_out STREQUAL compat_out)
    message(FATAL_ERROR
        "I2: complete-closure output differs from compatibility output")
endif()

# --- I2 counterexample: a real reference restores the include ---------------

run_ok(reference_out reference_err ${CODEGEN} --descriptor-set
    "${DATA_DIR}/support_reference_closure.pbs" --target support_reference.proto)
require_contains(reference_out "#include \"google/protobuf/descriptor.pb.hpp\""
    "I2 counterexample: a real field type must restore the dependency include")

# --- I3: a public re-export restores the include ----------------------------

run_ok(reexport_out reexport_err ${CODEGEN} --descriptor-set
    "${DATA_DIR}/public_option_closure.pbs" --target public_custom_types.proto)
require_contains(reexport_out "#include \"easypb/options.pb.hpp\""
    "I3: a public re-export must restore the support dependency include")

# --- I4: support files stay in the semantic graph ---------------------------

run_fail(incomplete_out incomplete_err ${CODEGEN} --descriptor-set
    "${DATA_DIR}/support_reference_only.pbs" --target support_reference.proto)
require_empty(incomplete_out "I4: the refused run writes no stdout")
require_contains(incomplete_err "missing import \"google/protobuf/descriptor.proto\""
    "I4: the missing support file is still a semantic error")
