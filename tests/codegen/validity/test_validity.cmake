# The validity lane: generate -> compile -> run -> refuse.
#
# One lane for the five rows of the plan-5 follow-up's session S1 (W-02,
# W-03, W-05, W-10, W-12). The three things every row needs:
#
#   1. BOTH input paths. The source path (.proto) and the descriptor path
#      (.pbs) must produce the SAME header for the same schema - the two
#      frontends store a default value the same way, so the emitter must
#      read it the same way.
#   2. THE HEADER COMPILES. A generated header that does not compile is a
#      failure; "the tool exited 0" is not a pass. The compile happens with
#      the tree's own compiler and the tree's own standard.
#   3. THE VALUE IS RIGHT. The runtime program checks the BYTES of every
#      default, because the two silent defects (a backslash that became a
#      backspace, a NUL that truncated the value) compile cleanly and are
#      caught only here.
#
# Required arguments:
#   CODEGEN     the standalone codegen executable
#   DATA_DIR    the fixture directory
#   WORK_DIR    the scratch directory
#   RUNTIME     the built runtime checker
#   CXX         the C++ compiler
#   CXX_FLAGS   the tree's compiler flags
#   CXX_STD_FLAG the tree's standard flag (may be empty)
#   INCLUDE_DIR the library's include directory
#   FULL_BUILD  1 when the proto parser is available

foreach(required_var CODEGEN DATA_DIR WORK_DIR RUNTIME CXX INCLUDE_DIR)
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
# schema. The values in them are the ones the runtime checker reads.
set(paired_fixtures
    defaults-quote
    defaults-backslash
    defaults-newline
    defaults-nul
    defaults-quote-byte
    defaults-trigraph
    defaults-inf
    defaults-nan)

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

    # The header compiles, in the tree's own standard. It is compiled as an
    # INCLUDED header, not as the main file: a header that is the main file
    # draws a "#pragma once in main file" warning that says nothing about the
    # header.
    if(CXX_STD_FLAG)
        set(std_flag "${CXX_STD_FLAG}")
    else()
        set(std_flag "")
    endif()
    file(WRITE "${WORK_DIR}/probe-${name}.cpp"
        "#include \"${name}.pb.hpp\"\n")
    run_ok(syntax_out syntax_err
        ${CXX} ${CXX_FLAGS} ${std_flag} -fsyntax-only
        -I "${INCLUDE_DIR}" -I "${WORK_DIR}/descriptor"
        "${WORK_DIR}/probe-${name}.cpp")
    require_empty(syntax_err "The generated header of ${name} must compile clean")
endforeach()

# The values are right.
run_ok(runtime_out runtime_err ${RUNTIME})
require_empty(runtime_err "The runtime checker's stderr")
require_contains(runtime_out "every default value carries the bytes the schema names"
    "The runtime checker's verdict")

# --- the refusals -----------------------------------------------------------
#
# Every shape the emitter or the linker must refuse: a non-zero exit, an
# empty stdout, and a diagnostic that names the offending thing.

# An enum default that is not an identifier (W-05).
run_fail(enum_out enum_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/enum-default-identifier.pbs")
require_empty(enum_out "The enum-default refusal stdout")
require_contains(enum_err "must be an identifier" "The enum-default refusal")

# A field whose declared type is outside the TYPE_* set (W-10).
run_fail(unknown_out unknown_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/field-type-unknown.pbs")
require_empty(unknown_out "The unknown-type refusal stdout")
require_contains(unknown_err "unknown type" "The unknown-type refusal")

# A field that declares no type at all (W-10).
run_fail(absent_out absent_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/field-type-absent.pbs")
require_empty(absent_out "The absent-type refusal stdout")
require_contains(absent_err "declares no type" "The absent-type refusal")

# A group field (W-10).
run_fail(group_out group_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/field-type-group.pbs")
require_empty(group_out "The group refusal stdout")
require_contains(group_err "does not support group types" "The group refusal")

# A logical file name that carries a control character (W-12).
run_fail(name_out name_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/descriptor-name-newline.pbs")
require_empty(name_out "The control-character refusal stdout")
require_contains(name_err "control character" "The control-character refusal")
