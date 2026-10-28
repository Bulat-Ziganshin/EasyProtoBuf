# The names lane: generate -> run -> refuse.
#
# One lane for the two rows of the plan-5 follow-up's session S2: W-08 (a
# C++ keyword in a declaration name) and W-09 (a presence flag that collides
# with a member of the same message), for the post-closure review of
# 2026-10-22 that found both rules incomplete (PC-01: a nested type collides
# with a presence flag as well; PC-02: the escape rule is not injective), and
# for step B6 of the verification stage, which turned the scope rule into an
# occupant enumeration (CR-03 enumerators, CR-04 codec names, CR-09 runtime
# namespaces, CR-10 non-identifier names, CR-43 the local-name helper). The
# three things every row needs:
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
#   CODEGEN       the standalone codegen executable
#   DATA_DIR      the fixture directory
#   WORK_DIR      the scratch directory
#   RUNTIME       the built runtime checker
#   GENERATED_DIR the descriptor-path headers the build generated and compiled
#   FULL_BUILD    1 when the proto parser is available
#
# Optional arguments:
#   REPO_DIR        the checkout root; with it and DESCRIPTOR_ROOT the lane
#                   compiles the repository's own `package easypb;` schema
#                   (`codegen/easypb/options.proto`) on the source path
#   DESCRIPTOR_ROOT the directory holding `google/protobuf/descriptor.proto`
#                   that `codegen/easypb/options.proto` imports

foreach(required_var CODEGEN DATA_DIR WORK_DIR RUNTIME GENERATED_DIR)
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

# --- the scope-occupant family ----------------------------------------------
#
# CR-03, CR-04 and CR-09 are one rule: the C++ scope check has to enumerate the
# occupants a generated scope will hold, not the instances the Protobuf names
# happen to spell. The family (occupant kinds x collision pairs x syntax x
# options) is data in family_cases.cmake, shared with the build so that every
# accepted case is also compiled.
#
# The driver proves three things per accepted case: the tool exits 0, the two
# input paths emit the same bytes, and the header the build compiled exists.
# Per refused case: the tool exits non-zero, stdout stays empty, and the one
# diagnostic names both occupants.
#
# The keyword-package refusal (E5) is not part of this family: it is a
# deliberate escape-rule divergence, not a scope collision.

include("${CMAKE_CURRENT_LIST_DIR}/family_cases.cmake")

foreach(entry ${codegen_names_accept_variants})
    string(REPLACE "|" ";" parts "${entry}")
    list(LENGTH parts parts_len)
    if(NOT parts_len EQUAL 3)
        message(FATAL_ERROR "Malformed accepted family case: ${entry}")
    endif()
    list(GET parts 0 name)
    list(GET parts 1 tag)
    list(GET parts 2 options)

    set(extra_options)
    if(NOT "${options}" STREQUAL "-")
        separate_arguments(extra_options UNIX_COMMAND "${options}")
    endif()

    # The build generates and compiles this header from the same fixture and
    # options. A missing one means the two consumers of family_cases.cmake
    # disagree.
    set(compiled_header "${GENERATED_DIR}/${name}.${tag}.pb.hpp")
    if(NOT EXISTS "${compiled_header}")
        message(FATAL_ERROR
            "The build did not generate the compiled family header ${compiled_header}")
    endif()

    run_ok(desc_out desc_err
        ${CODEGEN} --descriptor-set ${extra_options} "${DATA_DIR}/${name}.pbs")
    require_empty(desc_err
        "The accepted scope case stderr for ${name} (${tag})")
    if("${desc_out}" STREQUAL "")
        message(FATAL_ERROR
            "The accepted scope case ${name} (${tag}) emitted no header")
    endif()

    if(FULL_BUILD AND EXISTS "${DATA_DIR}/${name}.proto")
        run_ok(src_out src_err
            ${CODEGEN} -I "${DATA_DIR}" ${extra_options}
            "${DATA_DIR}/${name}.proto")
        require_empty(src_err
            "The accepted scope case source stderr for ${name} (${tag})")
        if(NOT "${src_out}" STREQUAL "${desc_out}")
            message(FATAL_ERROR
                "The two input paths disagree for the accepted scope case "
                "${name} (${tag})")
        endif()
    endif()
endforeach()

foreach(entry ${codegen_names_refusal_cases})
    string(REPLACE "|" ";" parts "${entry}")
    list(LENGTH parts parts_len)
    if(NOT parts_len EQUAL 3)
        message(FATAL_ERROR "Malformed refused family case: ${entry}")
    endif()
    list(GET parts 0 name)
    list(GET parts 1 options)
    list(GET parts 2 needles)

    set(extra_options)
    if(NOT "${options}" STREQUAL "-")
        separate_arguments(extra_options UNIX_COMMAND "${options}")
    endif()

    run_fail(desc_out desc_err
        ${CODEGEN} --descriptor-set ${extra_options} "${DATA_DIR}/${name}.pbs")
    require_empty(desc_out
        "The scope refusal stdout for ${name} (${options})")
    string(REPLACE ">" ";" needle_list "${needles}")
    foreach(needle ${needle_list})
        require_contains(desc_err "${needle}"
            "The scope refusal for ${name} (${options})")
    endforeach()

    if(FULL_BUILD AND EXISTS "${DATA_DIR}/${name}.proto")
        run_fail(src_out src_err
            ${CODEGEN} -I "${DATA_DIR}" ${extra_options}
            "${DATA_DIR}/${name}.proto")
        require_empty(src_out
            "The scope refusal stdout for ${name} (${options}) on the source path")
        foreach(needle ${needle_list})
            require_contains(src_err "${needle}"
                "The scope refusal for ${name} (${options}) on the source path")
        endforeach()
    endif()
endforeach()

# --- R11: the refusal carries the source position (U-30) ---------------------
#
# The emitter throws a positioned EmitterError, and the CLI turns the path into
# the file:line:column the parser recorded for that declaration. The descriptor
# door has no source text, so its sentence keeps the file name alone and must
# not fabricate coordinates.
if(FULL_BUILD)
    run_fail(position_out position_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/escape-field.proto")
    require_empty(position_out "The positioned name-refusal stdout")
    if(NOT position_err MATCHES "escape-field[.]proto:[0-9]+:[0-9]+: ")
        message(FATAL_ERROR
            "U-30: the name refusal carries no file:line:column: ${position_err}")
    endif()
    require_contains(position_err "field 'class'"
        "U-30: the positioned refusal names the first occupant")
    require_contains(position_err "field 'class_'"
        "U-30: the positioned refusal names the second occupant")
endif()

run_fail(position_desc_out position_desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/escape-field.pbs")
require_empty(position_desc_out "The descriptor name-refusal stdout")
if(position_desc_err MATCHES "escape-field[.]proto:[0-9]+:[0-9]+: ")
    message(FATAL_ERROR
        "The descriptor refusal fabricated a source position: ${position_desc_err}")
endif()

# --- U-02/V-01: the repository's own `package easypb;` schema -----------------
#
# Owner decision #1 allows `package easypb;` and refuses only a declaration
# that carries a runtime declaration's name. The control is the repository's
# own `codegen/easypb/options.proto`: it declares `package easypb;` and
# `CppFieldOptions`, so it must keep compiling. Its imports are the roots the
# custom-types lane already uses (the `easypb` package directory and the
# test-only `google/protobuf/descriptor.proto`).
if(FULL_BUILD AND DEFINED REPO_DIR AND DEFINED DESCRIPTOR_ROOT)
    run_ok(options_out options_err
        ${CODEGEN} -I "${REPO_DIR}/codegen" -I "${DESCRIPTOR_ROOT}"
        "${REPO_DIR}/codegen/easypb/options.proto")
    # The only diagnostic is the documented `extend` notice: the file's
    # extension is parsed and validated but not stored in the trimmed
    # descriptor model, so it cannot reach the emitter as a declaration.
    require_contains(options_err "extend declaration parsed but not stored"
        "The repository's options.proto extend notice")
    require_contains(options_out "namespace easypb {"
        "The repository's options.proto package namespace")
    require_contains(options_out "struct CppFieldOptions"
        "The repository's options.proto declaration")
endif()
