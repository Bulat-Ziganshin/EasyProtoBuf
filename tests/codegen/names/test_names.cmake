# The names lane: generate -> run -> refuse.
#
# One lane for the two rows of the plan-5 follow-up's session S2: W-08 (a
# C++ keyword in a declaration name) and W-09 (a presence flag that collides
# with a member of the same message), and for the post-closure review of
# 2026-10-22 that found both rules incomplete: a nested type collides with a
# presence flag as well (PC-01), and the escape rule is not injective, so two
# Protobuf names can be one C++ name (PC-02). The three things every row
# needs:
#
#   1. BOTH input paths. The source path (.proto) and the descriptor path
#      (.pbs) must produce the SAME header for the same schema. The two
#      frontends carry the same names, so the emitter must spell them the
#      same way.
#   2. THE HEADER COMPILES. A generated header that does not compile is a
#      failure; "the tool exited 0" is not a pass. The compile belongs to
#      the build: the runtime target compiles every descriptor-path header
#      with the tree's own compiler and standard, and the byte-for-byte
#      comparison above extends that proof to the source path. The driver
#      does not invoke the compiler itself, because cl.exe started outside
#      the Visual Studio environment cannot find its own standard library
#      headers (C1083 on <cstdint>).
#   3. THE NAME CARRIES THE DATA. An escaped member must still hold the
#      value the schema names, and the value must still travel under the
#      field number the schema gives it. The runtime checker writes every
#      escaped member and reads the bytes back.
#
# Required arguments:
#   CODEGEN     the standalone codegen executable
#   DATA_DIR    the fixture directory
#   WORK_DIR    the scratch directory
#   RUNTIME     the built runtime checker
#   FULL_BUILD  1 when the proto parser is available

foreach(required_var CODEGEN DATA_DIR WORK_DIR RUNTIME)
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
        message(FATAL_ERROR "${description}: missing '${expected}' in ${variable_name}")
    endif()
endfunction()

function(require_empty variable_name description)
    if(NOT "${${variable_name}}" STREQUAL "")
        message(FATAL_ERROR "${description}: expected empty output, got '${${variable_name}}'")
    endif()
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/descriptor")
file(MAKE_DIRECTORY "${WORK_DIR}/source")

# The paired fixtures: a .proto source and the .pbs descriptor of the same
# schema. Every one of them names a declaration after a C++ keyword, in one
# of the four kinds the emitter spells: a field, a message, an enum, an enum
# value.
set(paired_fixtures
    keyword-field
    keyword-message
    keyword-enum
    keyword-enum-value
    keyword-nested)

foreach(name ${paired_fixtures})
    # The descriptor path.
    run_ok(desc_out desc_err
        ${CODEGEN} --descriptor-set "${DATA_DIR}/${name}.pbs")
    file(WRITE "${WORK_DIR}/descriptor/${name}.pb.hpp" "${desc_out}")
    require_empty(desc_err "Descriptor generation stderr for ${name}")

    # The source path. The two frontends must agree byte for byte, so the
    # header the descriptor path produced is the reference.
    if(FULL_BUILD)
        run_ok(src_out src_err
            ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${name}.proto")
        file(WRITE "${WORK_DIR}/source/${name}.pb.hpp" "${src_out}")
        require_empty(src_err "Source generation stderr for ${name}")

        file(READ "${WORK_DIR}/descriptor/${name}.pb.hpp" descriptor_header)
        file(READ "${WORK_DIR}/source/${name}.pb.hpp" source_header)
        if(NOT source_header STREQUAL descriptor_header)
            message(FATAL_ERROR
                "The source path and the descriptor path disagree for ${name}")
        endif()
    endif()

    # The header is compiled by the build's runtime target, which includes
    # the descriptor-path header of every fixture. When the source path
    # runs, the comparison above proves its header is the same file, so it
    # compiles too.
endforeach()

# The escaped names carry the schema's data.
run_ok(runtime_out runtime_err ${RUNTIME})
require_empty(runtime_err "The runtime checker's stderr")
require_contains(runtime_out "every escaped name carries the data the schema names"
    "The runtime checker's verdict")

# --- the refusals ------------------------------------------------------------
#
# A name collision in one C++ scope. A scope cannot hold two declarations
# with one name, and the escape rule is not injective: `class` and `class_`
# are two Protobuf names and one C++ name. The shapes are refused with a
# diagnostic, in proto2 and in proto3, on both input paths.
#
# Each entry is the fixture and the words its diagnostic must carry.

set(refusal_fixtures
    "has-collision|collides with the presence flag"
    "has-collision-p3|collides with the presence flag"
    "nested-flag|collides with the presence flag"
    "escape-field|would both be named"
    "escape-message|would both be named"
    "escape-enum-value|would both be named"
    "escape-enum-message|would both be named")

foreach(entry ${refusal_fixtures})
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 expected)

    run_fail(collision_out collision_err
        ${CODEGEN} --descriptor-set "${DATA_DIR}/${name}.pbs")
    require_empty(collision_out "The collision refusal stdout for ${name}")
    require_contains(collision_err "${expected}"
        "The collision refusal for ${name}")

    if(FULL_BUILD)
        run_fail(source_out source_err
            ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${name}.proto")
        require_empty(source_out
            "The collision refusal stdout for ${name} on the source path")
        require_contains(source_err "${expected}"
            "The collision refusal for ${name} on the source path")
    endif()
endforeach()
