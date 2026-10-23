# The validity lane: generate -> run -> refuse.
#
# One lane for the five rows of the plan-5 follow-up's session S1 (W-02,
# W-03, W-05, W-10, W-12). The three things every row needs:
#
#   1. BOTH input paths. The source path (.proto) and the descriptor path
#      (.pbs) must produce the SAME header for the same schema - the two
#      frontends store a default value the same way, so the emitter must
#      read it the same way.
#   2. THE HEADER COMPILES. A generated header that does not compile is a
#      failure; "the tool exited 0" is not a pass. The compile belongs to
#      the build: the runtime target compiles every descriptor-path header
#      with the tree's own compiler and standard, and the byte-for-byte
#      comparison above extends that proof to the source path. The driver
#      does not invoke the compiler itself, because cl.exe started outside
#      the Visual Studio environment cannot find its own standard library
#      headers (C1083 on <cstdint>).
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
# schema. The values in them are the ones the runtime checker reads.
set(paired_fixtures
    defaults-quote
    defaults-backslash
    defaults-newline
    defaults-nul
    defaults-quote-byte
    defaults-trigraph
    defaults-inf
    defaults-nan
    defaults-float-overflow
    defaults-bytes-upper-hex
    packages-deep-101
    enum-alias-same-number
    json-names-distinct
    plus-controls
    leading-space-control)

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

# Descriptor-only accepted shapes: the absent/empty syntax of a descriptor is
# proto2 (W-15), and a negative descriptor default keeps the accepted control
# beside the W-40 refusals.
run_ok(empty_syntax_desc_out empty_syntax_desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/empty-syntax-descriptor.pbs")
require_empty(empty_syntax_desc_err "The empty-syntax descriptor stderr")

run_ok(plus_desc_controls_out plus_desc_controls_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/plus-descriptor-controls.pbs")
require_empty(plus_desc_controls_err "The descriptor plus-control stderr")

# W-43's proto2 warning pairs: generation succeeds, the sentence is printed,
# and the source and descriptor paths carry the same shape.
foreach(name json-name-custom-vs-default-proto2 json-name-proto2-warning)
    run_ok(warn_desc_out warn_desc_err
        ${CODEGEN} --descriptor-set "${DATA_DIR}/${name}.pbs")
    file(WRITE "${WORK_DIR}/descriptor/${name}.pb.hpp" "${warn_desc_out}")
    require_contains(warn_desc_err "conflicts with" "The ${name} descriptor warning")

    if(FULL_BUILD)
        run_ok(warn_src_out warn_src_err
            ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${name}.proto")
        file(WRITE "${WORK_DIR}/source/${name}.pb.hpp" "${warn_src_out}")
        require_contains(warn_src_err "conflicts with" "The ${name} source warning")
        file(READ "${WORK_DIR}/descriptor/${name}.pb.hpp" warn_desc_header)
        file(READ "${WORK_DIR}/source/${name}.pb.hpp" warn_src_header)
        if(NOT warn_src_header STREQUAL warn_desc_header)
            message(FATAL_ERROR
                "The source path and the descriptor path disagree for ${name}")
        endif()
    endif()
endforeach()

# W-43's carried names are visible in the diagnostic descriptor, including the
# values a protoc-written `.pbs` retains. `--print-descriptor` is part of the
# parser build, so the lite profile skips this presentation check.
if(FULL_BUILD)
    run_ok(json_print_out json_print_err
        ${CODEGEN} --descriptor-set --print-descriptor
        "${DATA_DIR}/json-names-distinct.pbs")
    require_empty(json_print_err "The JSON-name print stderr")
    require_contains(json_print_out "json_name=\"alpha\"" "The first custom JSON name")
    require_contains(json_print_out "json_name=\"beta\"" "The second custom JSON name")
endif()

# W-41: a BOM is consumed by the lexer, and the header is byte-identical to
# the no-BOM twin once both files carry the same logical basename.
if(FULL_BUILD)
    file(MAKE_DIRECTORY "${WORK_DIR}/bom")
    file(MAKE_DIRECTORY "${WORK_DIR}/bom-control")
    file(COPY "${DATA_DIR}/bom.proto" DESTINATION "${WORK_DIR}/bom")
    file(COPY "${DATA_DIR}/bom-control.proto" DESTINATION "${WORK_DIR}/bom-control")
    file(RENAME "${WORK_DIR}/bom-control/bom-control.proto"
         "${WORK_DIR}/bom-control/bom.proto")

    run_ok(bom_out bom_err
        ${CODEGEN} -I "${WORK_DIR}/bom" "${WORK_DIR}/bom/bom.proto")
    require_empty(bom_err "The BOM source stderr")
    run_ok(bom_control_out bom_control_err
        ${CODEGEN} -I "${WORK_DIR}/bom-control"
        "${WORK_DIR}/bom-control/bom.proto")
    require_empty(bom_control_err "The no-BOM source stderr")
    if(NOT bom_out STREQUAL bom_control_out)
        message(FATAL_ERROR "The BOM fixture changed the generated header")
    endif()
endif()

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

# A default value with an embedded NUL cannot travel in a C++ string literal:
# it needs the two-argument form std::string(literal, length), and only
# std::string can be built from a literal and a length. With another string
# type the value cannot be carried at all, so the schema is refused with a
# diagnostic instead of a header that cannot mean what the schema says (the
# owner's decision of 2026-10-21).
run_fail(type_out type_err
    ${CODEGEN} --descriptor-set --string-type "::my::String"
    "${DATA_DIR}/defaults-nul.pbs")
require_empty(type_out "The custom-string-type refusal stdout")
require_contains(type_err "cannot carry it" "The custom-string-type refusal")

if(FULL_BUILD)
    run_fail(source_type_out source_type_err
        ${CODEGEN} -I "${DATA_DIR}" --string-type "::my::String"
        "${DATA_DIR}/defaults-nul.proto")
    require_empty(source_type_out
        "The custom-string-type refusal stdout on the source path")
    require_contains(source_type_err "cannot carry it"
        "The custom-string-type refusal on the source path")
endif()

# --- S7a: the schema acceptance boundary ------------------------------------
#
# W-15: an explicit empty syntax statement is refused; the descriptor twin
# above stays accepted.
if(FULL_BUILD)
    run_fail(syntax_out syntax_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/empty-syntax.proto")
    require_empty(syntax_out "The empty-syntax refusal stdout")
    require_contains(syntax_err "syntax must be" "The empty-syntax refusal")
endif()

# W-38: 101 components are accepted (paired above), 102 are refused on both
# paths.
run_fail(pkg_desc_out pkg_desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/packages-deep-102.pbs")
require_empty(pkg_desc_out "The deep-package descriptor stdout")
require_contains(pkg_desc_err "package depth" "The deep-package descriptor refusal")
if(FULL_BUILD)
    run_fail(pkg_src_out pkg_src_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/packages-deep-102.proto")
    require_empty(pkg_src_out "The deep-package source stdout")
    require_contains(pkg_src_err "package depth" "The deep-package source refusal")
endif()

# W-04: the canonicalized ERANGE family is accepted through the source path and
# a raw descriptor (paired above); the runtime checker below reads the values.

# W-39: the uppercase hex escape is accepted and decoded on both paths
# (paired above); the runtime checker below reads the byte.

# W-40: every leading plus is refused on the source path.
if(FULL_BUILD)
    run_fail(plus_int_out plus_int_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/plus-int-default.proto")
    require_empty(plus_int_out "The plus-int stdout")
    require_contains(plus_int_err "Expected integer" "The plus-int refusal")

    run_fail(plus_enum_out plus_enum_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/plus-enum-value.proto")
    require_empty(plus_enum_out "The plus-enum stdout")
    require_contains(plus_enum_err "Expected integer" "The plus-enum refusal")

    run_fail(plus_float_out plus_float_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/plus-float-default.proto")
    require_empty(plus_float_out "The plus-float stdout")
    require_contains(plus_float_err "Expected number" "The plus-float refusal")

    run_fail(plus_inf_out plus_inf_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/plus-inf-nan.proto")
    require_empty(plus_inf_out "The plus-inf stdout")
    require_contains(plus_inf_err "Expected number" "The plus-inf refusal")
endif()

# W-40: the descriptor integer and float spellings are refused too; the
# negative controls are the descriptor-only fixture accepted above.
run_fail(plus_desc_int_out plus_desc_int_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/plus-int-descriptor.pbs")
require_empty(plus_desc_int_out "The plus descriptor-int stdout")
require_contains(plus_desc_int_err "may not start with" "The plus descriptor-int refusal")

run_fail(plus_desc_float_out plus_desc_float_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/plus-float-descriptor.pbs")
require_empty(plus_desc_float_out "The plus descriptor-float stdout")
require_contains(plus_desc_float_err "may not start with" "The plus descriptor-float refusal")

# W-03's last sub-case: a descriptor floating-point default whose stored text
# starts with a space is refused, as protoc refuses it. The accepted control
# is the paired `leading-space-control` above: whitespace before a source
# token and a string default's leading space both stay legal.
run_fail(leading_space_out leading_space_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/defaults-leading-space.pbs")
require_empty(leading_space_out "The leading-space default stdout")
require_contains(leading_space_err "leading whitespace" "The leading-space default refusal")

# W-42: folded enum names are refused on both paths; the same-number alias is
# accepted above.
run_fail(enum_case_desc_out enum_case_desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/enum-name-collision-case.pbs")
require_empty(enum_case_desc_out "The enum-case descriptor stdout")
require_contains(enum_case_desc_err "same name as" "The enum-case descriptor refusal")
run_fail(enum_prefix_desc_out enum_prefix_desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/enum-name-collision-prefix.pbs")
require_empty(enum_prefix_desc_out "The enum-prefix descriptor stdout")
require_contains(enum_prefix_desc_err "same name as" "The enum-prefix descriptor refusal")
run_fail(enum_diff_desc_out enum_diff_desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/enum-alias-diff-number.pbs")
require_empty(enum_diff_desc_out "The enum-diff descriptor stdout")
require_contains(enum_diff_desc_err "same name as" "The enum-diff descriptor refusal")

if(FULL_BUILD)
    foreach(name enum-name-collision-case enum-name-collision-prefix enum-alias-diff-number)
        run_fail(enum_src_out enum_src_err
            ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${name}.proto")
        require_empty(enum_src_out "The ${name} source stdout")
        require_contains(enum_src_err "same name as" "The ${name} source refusal")
    endforeach()
endif()

# W-43: the default, custom, and custom-vs-default proto3 collisions are
# refused on both paths. The proto2 warning pairs are accepted above.
run_fail(json_default_desc_out json_default_desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/json-name-default-collision.pbs")
require_empty(json_default_desc_out "The JSON-default descriptor stdout")
require_contains(json_default_desc_err "conflicts with" "The JSON-default descriptor refusal")
run_fail(json_custom_desc_out json_custom_desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/json-name-custom-collision.pbs")
require_empty(json_custom_desc_out "The JSON-custom descriptor stdout")
require_contains(json_custom_desc_err "conflicts with" "The JSON-custom descriptor refusal")
run_fail(json_cvd_desc_out json_cvd_desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/json-name-custom-vs-default-proto3.pbs")
require_empty(json_cvd_desc_out "The JSON custom-vs-default descriptor stdout")
require_contains(json_cvd_desc_err "conflicts with" "The JSON custom-vs-default descriptor refusal")

if(FULL_BUILD)
    foreach(name json-name-default-collision json-name-custom-collision json-name-custom-vs-default-proto3)
        run_fail(json_src_out json_src_err
            ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${name}.proto")
        require_empty(json_src_out "The ${name} source stdout")
        require_contains(json_src_err "conflicts with" "The ${name} source refusal")
    endforeach()

    # A map field is a source field too: its custom name is carried through
    # the synthetic-entry expansion and checked by the same sweep.
    run_fail(json_map_out json_map_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/json-name-map-collision.proto")
    require_empty(json_map_out "The map JSON-name source stdout")
    require_contains(json_map_err "conflicts with" "The map JSON-name source refusal")
endif()

# W-06: the out-of-range integer default stays refused on both paths; the
# in-range control is plus-controls (accepted above).
run_fail(int_overflow_desc_out int_overflow_desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/int-default-overflow.pbs")
require_empty(int_overflow_desc_out "The integer-overflow descriptor stdout")
require_contains(int_overflow_desc_err "default does not fit" "The integer-overflow descriptor refusal")
if(FULL_BUILD)
    run_fail(int_overflow_src_out int_overflow_src_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/int-default-overflow.proto")
    require_empty(int_overflow_src_out "The integer-overflow source stdout")
    require_contains(int_overflow_src_err "default does not fit" "The integer-overflow source refusal")
endif()

# W-07: a descriptor field with a default but no declared type is refused by
# W-10's absent-type rule.
run_fail(no_type_out no_type_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/default-no-type.pbs")
require_empty(no_type_out "The no-type default stdout")
require_contains(no_type_err "declares no type" "The no-type default refusal")
