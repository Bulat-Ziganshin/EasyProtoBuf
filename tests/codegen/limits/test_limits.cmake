# The limits lane: generate -> compile -> run -> refuse.
#
# One lane for the three rows of the plan-5 follow-up's session S3: W-01 (no
# nesting-depth limit), W-30 (the publisher does not validate the output name)
# and W-32 (the output tree is created without a bound). The three things
# every row needs:
#
#   1. BOTH input paths. The source path (.proto) and the descriptor path
#      (.pbs) must agree: the same schema is accepted by both or refused by
#      both, and the two paths produce the SAME header for the accepted one.
#   2. THE HEADER COMPILES. A generated header that does not compile is a
#      failure; "the tool exited 0" is not a pass. The compile happens with
#      the tree's own compiler and the tree's own standard. This one has an
#      exception, and only this one: MSVC's compiler limit is 16 levels of
#      nested struct definitions (C1046) and the accepted nesting is 31, so
#      the MSVC profile cannot compile that header and gives up the compile
#      and the runtime check - it keeps the other two.
#   3. THE REFUSAL IS A DIAGNOSTIC. A non-zero exit, an empty stdout and a
#      stderr that names the offending thing. A crash is a failure, not a
#      refusal.
#
# Required arguments:
#   CODEGEN     the standalone codegen executable
#   DATA_DIR    the fixture directory
#   WORK_DIR    the scratch directory
#   RUNTIME     the built runtime checker (required when COMPILE_ACCEPTED)
#   CXX         the C++ compiler (required when COMPILE_ACCEPTED)
#   CXX_FLAGS   the tree's compiler flags (one argument, semicolons separate
#               the flags: the driver expands the value as a list)
#   CXX_STD_FLAG the tree's standard flag (may be empty)
#   INCLUDE_DIR the library's include directory (required when
#               COMPILE_ACCEPTED)
#   FULL_BUILD  1 when the proto parser is available
#   COMPILE_ACCEPTED
#               1 when the compiler of this profile can compile the accepted
#               header, 0 when it cannot (MSVC: its compiler limit is 16
#               levels of nested struct definitions - C1046 - and the
#               accepted nesting is 31)

foreach(required_var CODEGEN DATA_DIR WORK_DIR)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()
if(NOT DEFINED FULL_BUILD)
    set(FULL_BUILD 1)
endif()
if(NOT DEFINED COMPILE_ACCEPTED)
    set(COMPILE_ACCEPTED 1)
endif()
# The compiler arguments and the runtime checker belong to the compile of the
# accepted header. A profile that cannot compile that header passes neither,
# and the driver says so instead of failing on a missing argument.
if(COMPILE_ACCEPTED)
    foreach(required_var RUNTIME CXX INCLUDE_DIR)
        if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
            message(FATAL_ERROR
                "${required_var} is required when the accepted header is compiled")
        endif()
    endforeach()
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

# --- the accepted nesting ----------------------------------------------------
#
# 31 levels of nested messages: the deepest schema the reference compiler
# accepts (its MaxMessageDeclarationNestingDepth() is 32, and it refuses the
# 32nd level). Both paths must accept it, and they must produce the same
# header.
set(accepted nesting-31)

run_ok(desc_out desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/${accepted}.pbs")
file(WRITE "${WORK_DIR}/descriptor/${accepted}.pb.hpp" "${desc_out}")
require_empty(desc_err "Descriptor generation stderr for ${accepted}")

if(FULL_BUILD)
    run_ok(src_out src_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${accepted}.proto")
    file(WRITE "${WORK_DIR}/source/${accepted}.pb.hpp" "${src_out}")
    require_empty(src_err "Source generation stderr for ${accepted}")

    file(READ "${WORK_DIR}/descriptor/${accepted}.pb.hpp" descriptor_header)
    file(READ "${WORK_DIR}/source/${accepted}.pb.hpp" source_header)
    if(NOT source_header STREQUAL descriptor_header)
        message(FATAL_ERROR
            "The source path and the descriptor path disagree for ${accepted}")
    endif()
endif()

# The header compiles, in the tree's own standard. It is compiled as an
# INCLUDED header, not as the main file: a header that is the main file
# draws a "#pragma once in main file" warning that says nothing about the
# header.
#
# MSVC cannot do this: the accepted header is a chain of 31 nested struct
# definitions and MSVC's compiler limit is 16 of them (C1046). The limit
# itself does not move - it is the reference compiler's own, and the
# generator keeps printing the nesting the way it does - but this profile
# cannot compile that header, so the compile and the runtime check below
# are the two steps it gives up. Everything above and below them is
# compiler-free and still runs: the generation, the agreement of the two
# input paths, and the two refusals.
if(COMPILE_ACCEPTED)
    if(CXX_STD_FLAG)
        set(std_flag "${CXX_STD_FLAG}")
    else()
        set(std_flag "")
    endif()
    file(WRITE "${WORK_DIR}/probe-${accepted}.cpp"
        "#include \"${accepted}.pb.hpp\"\n")
    run_ok(syntax_out syntax_err
        ${CXX} ${CXX_FLAGS} ${std_flag} -fsyntax-only
        -I "${INCLUDE_DIR}" -I "${WORK_DIR}/descriptor"
        "${WORK_DIR}/probe-${accepted}.cpp")
    require_empty(syntax_err "The generated header of ${accepted} must compile clean")

    # The accepted nesting carries the schema's data.
    run_ok(runtime_out runtime_err ${RUNTIME})
    require_empty(runtime_err "The runtime checker's stderr")
    require_contains(runtime_out "every accepted nesting carries the schema's data"
        "The runtime checker's verdict")
else()
    message(STATUS
        "The compiler of this profile cannot compile the accepted header "
        "(MSVC refuses a chain of 31 nested struct definitions - C1046), so "
        "this run checks the generation, the agreement of the two input paths "
        "and the two refusals only.")
endif()

# --- the refused nesting -----------------------------------------------------
#
# The same shape one level deeper. The reference compiler refuses it with
# "Reached maximum recursion limit for nested messages.", and so does this
# project, on both input paths.
set(refused nesting-32)

run_fail(deep_out deep_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/${refused}.pbs")
require_empty(deep_out "The nesting refusal stdout on the descriptor path")
require_contains(deep_err "Reached maximum recursion limit for nested messages"
    "The nesting refusal on the descriptor path")

if(FULL_BUILD)
    run_fail(source_out source_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${refused}.proto")
    require_empty(source_out "The nesting refusal stdout on the source path")
    require_contains(source_err "Reached maximum recursion limit for nested messages"
        "The nesting refusal on the source path")
endif()

# --- the bounded output tree -------------------------------------------------
#
# A logical name with more components than the limit is refused at the
# mapping layer, before the publisher creates a directory: the output root
# must not exist afterwards.
run_fail(name_out name_err
    ${CODEGEN} --descriptor-set --out-dir "${WORK_DIR}/deep-name"
    "${DATA_DIR}/deep-name.pbs")
require_empty(name_out "The deep-name refusal stdout")
require_contains(name_err "must not have more than 100 components"
    "The deep-name refusal")
if(EXISTS "${WORK_DIR}/deep-name")
    file(GLOB_RECURSE deep_entries "${WORK_DIR}/deep-name/*")
    if(deep_entries)
        message(FATAL_ERROR
            "The deep-name refusal created files: ${deep_entries}")
    endif()
endif()

# The same rule on the source path: the argument itself is a logical name, so
# a name with more components than the limit is refused before the tool looks
# for a file. Nothing is read and nothing is written.
if(FULL_BUILD)
    set(deep_argument "")
    foreach(index RANGE 101)
        string(APPEND deep_argument "d/")
    endforeach()
    string(APPEND deep_argument "x.proto")
    run_fail(argument_out argument_err
        ${CODEGEN} -I "${DATA_DIR}" "${deep_argument}")
    require_empty(argument_out "The deep-argument refusal stdout")
    require_contains(argument_err "must not have more than 100 components"
        "The deep-argument refusal")
endif()
