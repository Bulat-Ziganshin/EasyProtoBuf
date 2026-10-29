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

function(require_absent variable_name unexpected description)
    string(FIND "${${variable_name}}" "${unexpected}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "${description}: found '${unexpected}' in ${variable_name}")
    endif()
endfunction()

# The first differing line of two generated headers. The two frontends run the
# same canonicalizers, so a disagreement is a defect in one of them and the
# sentence alone does not say which way it went.
function(require_identical_headers first_var second_var description)
    if("${${first_var}}" STREQUAL "${${second_var}}")
        return()
    endif()
    string(REPLACE "\n" ";" first_lines "${${first_var}}")
    string(REPLACE "\n" ";" second_lines "${${second_var}}")
    list(LENGTH first_lines first_count)
    list(LENGTH second_lines second_count)
    set(limit "${first_count}")
    if(second_count LESS limit)
        set(limit "${second_count}")
    endif()
    set(difference "the files differ past the ${limit} lines they share")
    math(EXPR last_index "${limit} - 1")
    if(limit GREATER 0)
        foreach(index RANGE 0 ${last_index})
            list(GET first_lines ${index} first_line)
            list(GET second_lines ${index} second_line)
            if(NOT first_line STREQUAL second_line)
                set(difference
                    "line ${index}:\n  descriptor: ${first_line}\n  source:     ${second_line}")
                break()
            endif()
        endforeach()
    endif()
    message(FATAL_ERROR "${description} (${difference})")
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
    defaults-float-subnormal
    defaults-bytes-upper-hex
    packages-deep-101
    enum-alias-same-number
    json-names-distinct
    plus-controls
    leading-space-control
    literals-escapes
    literals-lexemes
    grammar-table
    scope-easypb
    scope-nested-reserved
    p3opt-ok
    oneof-contiguous
    message-set-empty
    message-set-extensions
    map-enum-zero
    reserved-range-end
    reserved-range-wide
    enum-keyword-default
    option-value-defaults
    option-value-defaults-control
    bool-default)

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
            # Name the first line that differs: the two fronts run the same
            # canonicalizers, so a disagreement is a defect in one of them and
            # the sentence alone does not say which way it went.
            string(REPLACE "\n" ";" descriptor_lines "${descriptor_header}")
            string(REPLACE "\n" ";" source_lines "${source_header}")
            list(LENGTH descriptor_lines descriptor_count)
            list(LENGTH source_lines source_count)
            set(limit "${descriptor_count}")
            if(source_count LESS limit)
                set(limit "${source_count}")
            endif()
            set(difference "the files differ past the ${limit} lines they share")
            math(EXPR last_index "${limit} - 1")
            if(limit GREATER 0)
                foreach(index RANGE 0 ${last_index})
                    list(GET descriptor_lines ${index} descriptor_line)
                    list(GET source_lines ${index} source_line)
                    if(NOT source_line STREQUAL descriptor_line)
                        set(difference
                            "line ${index}:\n  descriptor: ${descriptor_line}\n  source:     ${source_line}")
                        break()
                    endif()
                endforeach()
            endif()
            message(FATAL_ERROR
                "The source path and the descriptor path disagree for ${name} (${difference})")
        endif()
    endif()

    # The header is compiled by the build's runtime target, which includes
    # the descriptor-path header of every fixture. When the source path
    # runs, the comparison above proves its header is the same file, so it
    # compiles too.
endforeach()

# --- R06: canonical defaults and accepted boundaries (U-18, U-56) ------------
#
# U-18: the two subnormal float defaults are the value protoc stores. The
# header literal is asserted by text (a regression to the six-digit spelling
# would compile and still read as a subnormal, so the runtime check alone
# cannot see it), and `--print-descriptor` must show the same default text on
# both doors - the defect the review measured was the source door printing
# six digits where the descriptor door and protoc printed nine.
run_ok(subnormal_header subnormal_header_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/defaults-float-subnormal.pbs")
require_empty(subnormal_header_err "The subnormal descriptor stderr")
require_contains(subnormal_header "float tiny = 9.9999461e-41;"
    "U-18: the subnormal float literal")
require_contains(subnormal_header "float minimum = 1.40129846e-45;"
    "U-18: the minimum subnormal float literal")
require_contains(subnormal_header "double tiny_double = 9.99988867182683e-321;"
    "U-18: the subnormal double literal keeps the fifteen-digit form")

# U-56: a reserved declaration may name 536870912 (the exclusive end of the
# last field-number range). The paired loop above proves both doors accept it
# and emit the same header; these assertions name the messages, so a verdict
# that accepted the schema but lost a message cannot pass.
run_ok(reserved_end_out reserved_end_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/reserved-range-end.pbs")
require_empty(reserved_end_err "The reserved-range-end descriptor stderr")
require_contains(reserved_end_out "struct BigReservedValue"
    "U-56: the single-number reserved declaration")
require_contains(reserved_end_out "struct BigReservedRange"
    "U-56: the range whose end is 536870912")

# --- V-03/V-04/V-05: reserved bounds, keyword defaults, MessageSet ranges -----
#
# The paired loop above proves both doors accept the three fixtures and emit
# identical headers; these assertions name the emitted declarations, so a run
# that accepted the schema but lost one cannot pass.
run_ok(reserved_wide_out reserved_wide_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/reserved-range-wide.pbs")
require_empty(reserved_wide_err "The reserved-range-wide descriptor stderr")
require_contains(reserved_wide_out "struct SingleHigh"
    "V-03: the single reserved number above 536870912")
require_contains(reserved_wide_out "struct RangeHigh"
    "V-03: the reserved range up to 2147483646")

run_ok(enum_keyword_out enum_keyword_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/enum-keyword-default.pbs")
require_empty(enum_keyword_err "The enum-keyword-default descriptor stderr")
require_contains(enum_keyword_out "true_ = 0"
    "V-04: the C++-escaped enumerator name")
require_contains(enum_keyword_out "::enumkw::E a = ::enumkw::E::true_;"
    "V-04: the true default")
require_contains(enum_keyword_out "::enumkw::E c = ::enumkw::E::inf;"
    "V-04: the inf default")
require_contains(enum_keyword_out "::enumkw::E d = ::enumkw::E::nan;"
    "V-04: the nan default")

run_ok(message_set_ext_out message_set_ext_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/message-set-extensions.pbs")
require_empty(message_set_ext_err "The message-set-extensions descriptor stderr")
require_contains(message_set_ext_out "struct ToMax"
    "V-05: the MessageSet whose range expands to 2147483646")
require_contains(message_set_ext_out "struct ExplicitWide"
    "V-05: the MessageSet with an explicit wide range")

# V-08, V-12 and V-17 are source-only. protoc refuses the two grammar shapes,
# so it cannot write their descriptor; the trimmed model neither stores nor
# decodes extension members (E6), so a `.pbs` cannot carry the `extend`
# declaration either. The source door owns the verdict in all three cases.
if(FULL_BUILD)
    run_fail(oneof_statement_out oneof_statement_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/oneof-empty-statement.proto")
    require_empty(oneof_statement_out "The oneof empty-statement stdout")
    require_contains(oneof_statement_err "Expected type name."
        "V-08: the bare semicolon is a missing type name")

    run_fail(extend_empty_out extend_empty_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/extend-empty.proto")
    require_empty(extend_empty_out "The empty extend stdout")
    require_contains(extend_empty_err
        "Expected \"required\", \"optional\", or \"repeated\"."
        "V-17: the empty extend body")

    run_fail(extend_semicolon_out extend_semicolon_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/extend-semicolon.proto")
    require_empty(extend_semicolon_out "The semicolon-only extend stdout")
    require_contains(extend_semicolon_err
        "Expected \"required\", \"optional\", or \"repeated\"."
        "V-17: the semicolon-only extend body")

    run_ok(proto3_extend_out proto3_extend_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/proto3-package-extend.proto")
    require_contains(proto3_extend_out "struct Holder"
        "V-12: the package-local proto3 extend keeps generating")
    require_contains(proto3_extend_err
        "extend declaration parsed but not stored by the trimmed FileDescriptorProto model"
        "V-12: the accepted extend keeps the E6 warning")
endif()

# --- S03 (V-06, V-09): map enum zero and descriptor extension members -------
#
# V-06: protoc refuses a map whose enum value type does not start at zero
# (descriptor.cc:8506-8512), because a missing entry is materialized as
# value{}. Both doors must answer the same sentence; the descriptor half is
# crafted from the .txtpb because protoc cannot write a descriptor for the
# schema it refuses.
run_fail(map_enum_nonzero_out map_enum_nonzero_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/map-enum-nonzero.pbs")
require_empty(map_enum_nonzero_out "The map-enum-nonzero descriptor stdout")
require_contains(map_enum_nonzero_err
    "Enum value in map must define 0 as the first value."
    "V-06: the map value enum starts at 1")
if(FULL_BUILD)
    run_fail(map_enum_nonzero_src_out map_enum_nonzero_src_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/map-enum-nonzero.proto")
    require_empty(map_enum_nonzero_src_out
        "The map-enum-nonzero source stdout")
    require_contains(map_enum_nonzero_src_err
        "Enum value in map must define 0 as the first value."
        "V-06: the source door answers the same sentence")
endif()
# The zero-first control is in the paired loop above: both doors accept it and
# emit one header.

# V-09: the descriptor door decodes extension members now, so a tag in the
# protocol-reserved block is refused with the source door's own sentence. The
# source half is refused by the parser; the descriptor half is crafted from the
# .txtpb, because protoc refuses the source too.
run_fail(ext_tag_out ext_tag_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/ext-member-tag-reserved.pbs")
require_empty(ext_tag_out "The ext-member-tag-reserved descriptor stdout")
require_contains(ext_tag_err
    "field numbers 19000..19999 are reserved by Protocol Buffers"
    "V-09: the descriptor-door extension tag rule")
if(FULL_BUILD)
    run_fail(ext_tag_src_out ext_tag_src_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/ext-member-tag-reserved.proto")
    require_empty(ext_tag_src_out "The ext-member-tag-reserved source stdout")
    require_contains(ext_tag_src_err
        "field numbers 19000..19999 are reserved by Protocol Buffers"
        "V-09: the source door answers the same sentence")
endif()

# V-09 accept control: a legal extension set is accepted by both doors and
# emits the recorded E6 warning. The extension members are validated but still
# not emitted, so the warning is the accepted-surface statement, not a defect.
run_ok(ext_valid_out ext_valid_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/ext-member-valid.pbs")
require_contains(ext_valid_out "struct M"
    "V-09: the accepted extension set keeps generating")
require_contains(ext_valid_err
    "extend declaration parsed but not stored by the trimmed FileDescriptorProto model"
    "V-09: the descriptor-door E6 warning")
if(FULL_BUILD)
    run_ok(ext_valid_src_out ext_valid_src_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/ext-member-valid.proto")
    require_contains(ext_valid_src_out "struct M"
        "V-09: the source half of the accepted extension set")
    require_contains(ext_valid_src_err
        "extend declaration parsed but not stored by the trimmed FileDescriptorProto model"
        "V-09: the source-door E6 warning")
    require_identical_headers(ext_valid_out ext_valid_src_out
        "V-09: the accepted extension set emits one header on both doors")
endif()

# --- S04 (V-24): the bool default spelling ----------------------------------
#
# The accepted pair is in the paired loop above: both doors emit the same
# header for `true` and `false`. The crafted `.pbs` carries the spelling a raw
# descriptor can hold and the source grammar cannot; the emitter must refuse it
# instead of writing `bool flag = yes;`.
run_ok(bool_default_out bool_default_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/bool-default.pbs")
require_empty(bool_default_err "The bool-default descriptor stderr")
require_contains(bool_default_out "bool enabled = true;"
    "V-24 the true default")
require_contains(bool_default_out "bool disabled = false;"
    "V-24 the false default")

run_fail(bool_default_bad_out bool_default_bad_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/bool-default-invalid.pbs")
require_empty(bool_default_bad_out "The invalid bool default writes no header")
require_contains(bool_default_bad_err "bool default"
    "V-24 the refusal names the rule")
require_contains(bool_default_bad_err "must be true or false"
    "V-24 the refusal names the accepted spellings")

if(FULL_BUILD)
    run_ok(subnormal_desc_print subnormal_desc_print_err
        ${CODEGEN} --descriptor-set --print-descriptor
        "${DATA_DIR}/defaults-float-subnormal.pbs")
    require_empty(subnormal_desc_print_err
        "The subnormal descriptor print stderr")
    run_ok(subnormal_src_print subnormal_src_print_err
        ${CODEGEN} --print-descriptor
        "${DATA_DIR}/defaults-float-subnormal.proto")
    require_empty(subnormal_src_print_err "The subnormal source print stderr")
    # The `file` line spells the physical path on the source door and the
    # logical name on the descriptor door; every default line is the same.
    foreach(spelling
            "default=9.9999461e-41"
            "default=1.40129846e-45"
            "default=9.99988867182683e-321")
        require_contains(subnormal_desc_print "${spelling}"
            "U-18: the descriptor print")
        require_contains(subnormal_src_print "${spelling}"
            "U-18: the source print")
    endforeach()
endif()

# --- CR-01/02/05/17: the default-value pipeline family -----------------------
#
# Matrix: numeric spelling x field type (float, double, int32, int64, uint64)
# x input path (source, .pbs) x option set (default, --no-has-fields). The
# `defaults-spellings` pair carries the spellings both frontends can write;
# `defaults-float-keywords` carries the CR-02 spellings only a descriptor can
# carry (protoc's source grammar is case-sensitive).
#
# 1. P2: the source and descriptor headers are compared byte for byte for
#    every option set.
# 2. P1: the descriptor is protoc's own `--descriptor_set_out`, so agreement
#    with its header is agreement with protoc's decoded value; the runtime
#    checker reads the same values as numbers.
# 3. P3: the build's `codegen_validity_pedantic_check` target compiles these
#    headers under -std=c++11 -pedantic-errors, and the equality above
#    extends that compile to the source headers.
foreach(options "" "--no-has-fields")
    if("${options}" STREQUAL "")
        set(option_label "default")
    else()
        set(option_label "no-has-fields")
    endif()

    run_ok(family_desc_out family_desc_err
        ${CODEGEN} --descriptor-set ${options}
        "${DATA_DIR}/defaults-spellings.pbs")
    require_empty(family_desc_err
        "The defaults-spellings descriptor stderr (${option_label})")
    require_contains(family_desc_out "float f_octal = 8;"
        "CR-01 float octal (${option_label})")
    require_contains(family_desc_out "double d_octal = 8;"
        "CR-01 double octal (${option_label})")
    require_contains(family_desc_out "float f_hex = 16;"
        "CR-01 float hex (${option_label})")
    require_contains(family_desc_out "float f_neg_octal = -8;"
        "CR-01 negative octal (${option_label})")
    require_contains(family_desc_out "::int32_t i32_octal = 8;"
        "CR-17 int32 octal (${option_label})")
    require_contains(family_desc_out "::int32_t i32_hex = 16;"
        "CR-17 int32 hex (${option_label})")
    require_contains(family_desc_out "::int32_t i32_neg_zero = 0;"
        "CR-17 negative zero (${option_label})")
    require_contains(family_desc_out "::int64_t i64_octal = 63;"
        "CR-17 int64 octal (${option_label})")
    require_contains(family_desc_out "::int64_t i64_min = (-9223372036854775807LL - 1);"
        "CR-05 int64 minimum (${option_label})")
    require_contains(family_desc_out "::uint64_t u64_max = 18446744073709551615ULL;"
        "CR-05 uint64 maximum (${option_label})")
    require_contains(family_desc_out "::uint64_t u64_hex_max = 18446744073709551615ULL;"
        "CR-17 hex uint64 maximum (${option_label})")

    if(FULL_BUILD)
        run_ok(family_src_out family_src_err
            ${CODEGEN} -I "${DATA_DIR}" ${options}
            "${DATA_DIR}/defaults-spellings.proto")
        require_empty(family_src_err
            "The defaults-spellings source stderr (${option_label})")
        require_identical_headers(family_desc_out family_src_out
            "The source and descriptor paths disagree for defaults-spellings (${option_label})")
    endif()

    run_ok(keyword_desc_out keyword_desc_err
        ${CODEGEN} --descriptor-set ${options}
        "${DATA_DIR}/defaults-float-keywords.pbs")
    require_empty(keyword_desc_err
        "The defaults-float-keywords stderr (${option_label})")
    require_contains(keyword_desc_out
        "::std::numeric_limits<float>::infinity()"
        "CR-02 INF spelling (${option_label})")
    require_contains(keyword_desc_out
        "::std::numeric_limits<float>::quiet_NaN()"
        "CR-02 NAN spelling (${option_label})")
    require_contains(keyword_desc_out
        "::std::numeric_limits<double>::quiet_NaN()"
        "CR-02 nan(payload) spelling (${option_label})")
    require_absent(keyword_desc_out "= INF" "CR-02 raw INF (${option_label})")
    require_absent(keyword_desc_out "= Infinity"
        "CR-02 raw Infinity (${option_label})")
    require_absent(keyword_desc_out "= NAN" "CR-02 raw NAN (${option_label})")
    require_absent(keyword_desc_out "= nan("
        "CR-02 raw nan(payload) (${option_label})")
endforeach()

# --- CR-12/13/18/19/29/30: the lexer and literal family ----------------------
#
# Matrix: escape rule (hex, octal, \u, \U, surrogate pair, lone surrogate,
# above 0x10FFFF, raw CR) x literal kind (string, bytes) x input path (source,
# .pbs), plus the lexeme table (0x10, 010, 077, 08, 09) and the two structural
# refusals (nested block comment, NUL in an aggregate option).
#
# `literals-escapes` and `literals-lexemes` are paired fixtures: the `.pbs` is
# protoc 30.1's own `--descriptor_set_out` of the source beside it, and the
# paired loop above compares the two headers byte for byte. The assertions
# below name the decoded value, so a header that compiles but carries the wrong
# bytes fails here as well.
foreach(source literals-escapes literals-lexemes)
    run_ok(lit_desc_out lit_desc_err
        ${CODEGEN} --descriptor-set "${DATA_DIR}/${source}.pbs")
    require_empty(lit_desc_err "The ${source} descriptor stderr")
    if(FULL_BUILD)
        run_ok(lit_src_out lit_src_err
            ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${source}.proto")
        require_empty(lit_src_err "The ${source} source stderr")
        require_identical_headers(lit_desc_out lit_src_out
            "The source and descriptor paths disagree for ${source}")
    endif()
endforeach()

# The escape table: one row per rule, on a string field and (where the value is
# a byte sequence) on a bytes field. Bracket arguments keep the backslashes
# literal; each expected fragment is the exact C++ the emitter writes.
run_ok(escape_out escape_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/literals-escapes.pbs")
require_empty(escape_err "The literals-escapes stderr")
require_contains(escape_out [[::std::string s_hex = "A";]]
    "CR-12 hex escape")
require_contains(escape_out [[::std::string s_hex_upper = "A";]]
    "CR-12 uppercase hex escape")
require_contains(escape_out [[::std::string s_hex_one = "\t";]]
    "CR-12 one-digit hex escape")
require_contains(escape_out [[::std::string s_octal = "A";]]
    "CR-12 octal escape")
require_contains(escape_out [[::std::string s_octal_max = "\377";]]
    "CR-12 octal escape above 0xFF truncates to 0xFF")
require_contains(escape_out [[::std::string s_octal_nul{"a\000b", 3};]]
    "CR-12 octal escape above 0xFF truncates to a NUL byte")
require_contains(escape_out [[::std::string s_octal_one{"\000", 1};]]
    "CR-12 one-digit octal escape")
require_contains(escape_out [[::std::string s_u1 = "A";]]
    "CR-13 two-digit Unicode escape")
require_contains(escape_out [[::std::string s_u2 = "\303\251";]]
    "CR-13 two-byte Unicode escape")
require_contains(escape_out [[::std::string s_u3 = "\342\202\254";]]
    "CR-13 three-byte Unicode escape")
require_contains(escape_out [[::std::string s_u4 = "\360\237\230\200";]]
    "CR-13 long Unicode escape")
require_contains(escape_out [[::std::string s_pair = "\360\237\230\200";]]
    "CR-13 UTF-16 surrogate pair assembles into one code point")
require_contains(escape_out [[::std::string s_lone_head = "\355\240\200";]]
    "CR-13 lone head surrogate is three bytes")
require_contains(escape_out [[::std::string s_lone_tail = "\355\260\200";]]
    "CR-13 lone trail surrogate is three bytes")
require_contains(escape_out [[::std::string s_over_max = "\\U00110000";]]
    "CR-13 a code point above 0x10FFFF is written back as escape text")
require_contains(escape_out [[::std::string s_head_ascii = "\355\240\200A";]]
    "CR-13 a head surrogate without a trail stays a lone code point")
require_contains(escape_out [[::std::string s_raw_cr = "x\ry";]]
    "CR-29 a raw CR is string content")
require_contains(escape_out [[::std::string b_octal_max = "\377";]]
    "CR-12 the bytes kind keeps the same low byte")
require_contains(escape_out [[::std::string b_octal_nul{"\000", 1};]]
    "CR-12 the bytes kind keeps the NUL")
require_contains(escape_out [[::std::string b_pair = "\360\237\230\200";]]
    "CR-13 the bytes kind keeps the assembled pair")
require_contains(escape_out [[::std::string b_lone_head = "\355\240\200";]]
    "CR-13 the bytes kind keeps the lone surrogate")
require_contains(escape_out [[::std::string b_over_max = "\\U00110000";]]
    "CR-13 the bytes kind stores the C-escaped escape text")
require_contains(escape_out [[::std::string b_raw_cr = "x\ry";]]
    "CR-29 the bytes kind keeps the raw CR")

# The lexeme table: `0x` is hex, a leading zero is octal, a zero followed by a
# fraction or exponent is decimal.
run_ok(lexeme_out lexeme_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/literals-lexemes.pbs")
require_empty(lexeme_err "The literals-lexemes stderr")
require_contains(lexeme_out "::int32_t i_hex = 16;" "CR-18 hex lexeme 0x10")
require_contains(lexeme_out "::int32_t i_octal = 8;" "CR-18 octal lexeme 010")
require_contains(lexeme_out "::int32_t i_octal_63 = 63;" "CR-18 octal lexeme 077")
require_contains(lexeme_out "::int32_t i_octal_zero = 0;" "CR-18 octal lexeme 00")
require_contains(lexeme_out "::int32_t i_zero = 0;" "CR-18 decimal lexeme 0")
require_contains(lexeme_out "float f_octal = 8;" "CR-18 octal lexeme on float")
require_contains(lexeme_out "double d_hex = 16;" "CR-18 hex lexeme on double")
require_contains(lexeme_out "double d_fraction = 0.5;" "CR-18 decimal fraction")
require_contains(lexeme_out "double d_exp = 0;" "CR-18 decimal exponent")
require_contains(lexeme_out "::uint64_t u_hex_max = 18446744073709551615ULL;"
    "CR-18 hex uint64 maximum")
# The non-default consumers of the same token rule: an octal enum value and an
# octal reserved-range endpoint stay valid, so the lexer rule is not a blanket
# refusal.
require_contains(lexeme_out "LEXEME_OCTAL = 8"
    "CR-18 octal enum value")

# The descriptor path has no tokenizer: what protoc's own descriptor validator
# accepts there stays accepted (a float or double spelling `08`, `09` or
# `010.5`), and an integral spelling it refuses stays refused. The crafted sets
# are `.txtpb` encoded by `make_descriptors.py` because protoc cannot write a
# descriptor from a source it refuses.
run_ok(desc_octal_out desc_octal_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/literals-descriptor-octal.pbs")
require_empty(desc_octal_err "The descriptor-octal stderr")
require_contains(desc_octal_out "float f08 = 8;" "CR-18 descriptor float 08")
require_contains(desc_octal_out "float f09 = 9;" "CR-18 descriptor float 09")
require_contains(desc_octal_out "double d010 = 10.5;"
    "CR-18 descriptor double 010.5")
require_contains(desc_octal_out "::int32_t i010 = 8;"
    "CR-18 descriptor int32 010 is octal")
run_fail(desc_octal_int_out desc_octal_int_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/literals-descriptor-octal-int.pbs")
require_empty(desc_octal_int_out "The descriptor-octal-int stdout")
require_contains(desc_octal_int_err "does not fit"
    "CR-18 descriptor int32 08 is refused")

if(FULL_BUILD)
    # CR-18 on the source path: the tokenizer refuses the literal for every
    # consumer, so the float default, the ordinary option value and the enum
    # value all fail at the token, not at a later range check.
    foreach(name literals-octal-08 literals-octal-09 literals-octal-enum)
        run_fail(octal_out octal_err
            ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${name}.proto")
        require_empty(octal_out "The ${name} stdout")
        require_contains(octal_err "leading zero must be octal"
            "The ${name} refusal")
    endforeach()

    # CR-30: block comments do not nest.
    run_fail(nested_out nested_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/literals-nested-comment.proto")
    require_empty(nested_out "The nested-comment stdout")
    require_contains(nested_err "cannot be nested" "The nested-comment refusal")

    # CR-19: a raw NUL is a lexical error, and the diagnostic escapes it.
    run_fail(aggregate_out aggregate_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/literals-aggregate-nul.proto")
    require_empty(aggregate_out "The aggregate-NUL stdout")
    require_contains(aggregate_err "control character"
        "The aggregate-NUL refusal")
    require_contains(aggregate_err "0x00" "The escaped NUL in the diagnostic")
endif()

# --- CR-11/20/26/27/28: the statement and option grammar family ---------------
#
# Matrix: rule x input path (source, .pbs). `grammar-table` is a paired fixture
# with one accepted spelling per rule: the empty statement after `syntax`, the
# oneof member's explicit default, `allow_alias = true` with a shared number,
# an enum value without field options, and an ordinary option without a leading
# `+`. protoc writes the `.pbs`, and the paired loop above compares the two
# headers byte for byte.
#
# The invalid spellings are source-only where protoc's grammar refuses to write
# a descriptor at all (CR-20's leading `;`, CR-27's enum-value `json_name`, and
# CR-28's leading `+`). CR-26 is the interesting one: its two rules live in
# protoc's parser (`ValidateEnum`, parser.cc:520-575), so a descriptor that
# carries the same options is accepted by protoc's descriptor path. The two
# crafted `*-descriptor.pbs` companions pin that verdict (codegen must accept
# them too), and the third (`false` with a shared number) is refused by the
# ordinary duplicate-number rule on both descriptor paths.
run_ok(grammar_out grammar_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/grammar-table.pbs")
require_empty(grammar_err "The grammar-table stderr")
require_contains(grammar_out "::int32_t a = 7;"
    "CR-11 the oneof member keeps its explicit default")
require_contains(grammar_out "A = 0" "CR-26 the alias first value")
require_contains(grammar_out "B = 0" "CR-26 the alias second value")

foreach(name grammar-allow-alias-useless-descriptor
             grammar-allow-alias-false-descriptor)
    run_ok(grammar_desc_out grammar_desc_err
        ${CODEGEN} --descriptor-set "${DATA_DIR}/${name}.pbs")
    require_empty(grammar_desc_err
        "The ${name} descriptor stderr (protoc accepts this shape)")
endforeach()

run_fail(grammar_desc_false_out grammar_desc_false_err
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/grammar-allow-alias-false-aliased-descriptor.pbs")
require_empty(grammar_desc_false_out
    "The false-with-alias descriptor stdout")
require_contains(grammar_desc_false_err "repeats number"
    "CR-26 the false-with-alias descriptor refusal")

if(FULL_BUILD)
    run_ok(grammar_src_out grammar_src_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/grammar-table.proto")
    require_empty(grammar_src_err "The grammar-table source stderr")
    require_identical_headers(grammar_out grammar_src_out
        "The source and descriptor paths disagree for grammar-table")

    # CR-20: `syntax` must be the first token.
    run_fail(grammar_semi_out grammar_semi_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/grammar-leading-semicolon.proto")
    require_empty(grammar_semi_out "The leading-semicolon stdout")
    require_contains(grammar_semi_err "first statement"
        "CR-20 the leading-semicolon refusal")

    # CR-11: the repeated-field half of the default rule stays.
    run_fail(grammar_repeated_out grammar_repeated_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/grammar-repeated-default.proto")
    require_empty(grammar_repeated_out "The repeated-default stdout")
    require_contains(grammar_repeated_err "repeated"
        "CR-11 the repeated-default refusal")

    # CR-26: both use shapes are refused by the parser.
    run_fail(grammar_alias_out grammar_alias_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/grammar-allow-alias-useless.proto")
    require_empty(grammar_alias_out "The useless-allow_alias stdout")
    require_contains(grammar_alias_err "no enum values share field numbers"
        "CR-26 the useless-allow_alias refusal")
    run_fail(grammar_alias_false_out grammar_alias_false_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/grammar-allow-alias-false.proto")
    require_empty(grammar_alias_false_out "The allow_alias-false stdout")
    require_contains(grammar_alias_false_err "no effect"
        "CR-26 the allow_alias-false refusal")
    run_fail(grammar_alias_false_alias_out grammar_alias_false_alias_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/grammar-allow-alias-false-aliased.proto")
    require_empty(grammar_alias_false_alias_out
        "The allow_alias-false-with-alias stdout")
    require_contains(grammar_alias_false_alias_err "no effect"
        "CR-26 the allow_alias-false-with-alias refusal")

    # CR-27: `json_name` is not an enum-value option. Since B12 the shared
    # known-option table owns the refusal: the name is not a field of
    # `EnumValueOptions`, so it is refused as unknown (D2-A).
    run_fail(grammar_enum_json_out grammar_enum_json_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/grammar-enum-json-name.proto")
    require_empty(grammar_enum_json_out "The enum-json_name stdout")
    require_contains(grammar_enum_json_err "json_name"
        "CR-27 the enum-value json_name refusal")

    # CR-28: an ordinary option takes no leading `+`, on the file, field and
    # enum-option paths alike.
    foreach(name grammar-option-plus grammar-option-plus-known
                 grammar-field-option-plus)
        run_fail(grammar_plus_out grammar_plus_err
            ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${name}.proto")
        require_empty(grammar_plus_out "The ${name} stdout")
        require_contains(grammar_plus_err "Expected option value"
            "CR-28 the ${name} refusal")
    endforeach()
endif()

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

# V-35: DEL belongs to the control-character class. The sentence still spells
# the byte and its position, exactly like the newline row above, and the print
# door applies the same name rule.
run_fail(del_out del_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/descriptor-name-del.pbs")
require_empty(del_out "The DEL-name refusal stdout")
require_contains(del_err "control character" "The DEL-name refusal")
require_contains(del_err "0x7f" "The DEL-name byte")

run_fail(del_print_out del_print_err
    ${CODEGEN} --descriptor-set --print-descriptor
    "${DATA_DIR}/descriptor-name-del.pbs")
require_empty(del_print_out "The DEL-name print refusal stdout")
require_contains(del_print_err "0x7f" "The DEL-name print byte")

# V-18: a descriptor-set name that escapes the output root. `--out-dir` refused
# it before this row; stdout generation and `--print-descriptor` used to accept
# it because neither writes a path. The shared logical-path rule now runs at
# ingestion, so every input mode refuses the same spelling with the same rule.
run_fail(dotdot_out dotdot_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/descriptor-name-dotdot.pbs")
require_empty(dotdot_out "The dotdot-name refusal stdout")
require_contains(dotdot_err "../escape.proto" "The dotdot-name refusal spelling")
require_contains(dotdot_err "is not a valid logical path"
    "The dotdot-name refusal rule")

run_fail(dotdot_print_out dotdot_print_err
    ${CODEGEN} --descriptor-set --print-descriptor
    "${DATA_DIR}/descriptor-name-dotdot.pbs")
require_empty(dotdot_print_out "The dotdot-name print refusal stdout")
require_contains(dotdot_print_err "../escape.proto"
    "The dotdot-name print spelling")
require_contains(dotdot_print_err "is not a valid logical path"
    "The dotdot-name print rule")

run_fail(dotdot_dir_out dotdot_dir_err
    ${CODEGEN} --descriptor-set --out-dir "${WORK_DIR}/dotdot"
    "${DATA_DIR}/descriptor-name-dotdot.pbs")
require_empty(dotdot_dir_out "The dotdot-name --out-dir refusal stdout")
require_contains(dotdot_dir_err "../escape.proto"
    "The dotdot-name --out-dir spelling")
require_contains(dotdot_dir_err "is not a valid logical path"
    "The dotdot-name --out-dir rule")

# V-42: an out-of-range label. protoc routes the value to the unknown fields and
# the field keeps its LABEL_OPTIONAL default; this project's chosen contract is
# a refusal that names the field, its message and the value. The print door
# reads the same bytes and answers the same way.
run_fail(label_out label_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/label-unknown.pbs")
require_empty(label_out "The label-99 refusal stdout")
require_contains(label_err "field \"a\"" "The label-99 field")
require_contains(label_err ".M" "The label-99 message scope")
require_contains(label_err "label 99" "The label-99 value")

run_fail(label_print_out label_print_err
    ${CODEGEN} --descriptor-set --print-descriptor
    "${DATA_DIR}/label-unknown.pbs")
require_empty(label_print_out "The label-99 print refusal stdout")
require_contains(label_print_err "label 99" "The label-99 print value")

# V-42 control: the label rule rejects only the out-of-range value. Two
# DISTINCT containers carrying one identical valid file still merge into one
# header (the same path twice would be deduplicated by the CLI before the
# loader sees it, which is not the merge rule).
file(MAKE_DIRECTORY "${WORK_DIR}/label-merge")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy
        "${DATA_DIR}/bool-default.pbs" "${WORK_DIR}/label-merge/copy.pbs"
    RESULT_VARIABLE label_copy_result)
if(NOT label_copy_result EQUAL 0)
    message(FATAL_ERROR "cannot stage the identical-copy control")
endif()
run_ok(label_merge_out label_merge_err
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/bool-default.pbs" "${WORK_DIR}/label-merge/copy.pbs")
require_empty(label_merge_err "The identical-copy merge stderr")
require_contains(label_merge_out "BoolDefaults"
    "The identical-copy merge header")

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

# U-51: protoc appends the allow_alias hint whether or not the option is set.
# `enum-name-collision-case` carries no option and `enum-alias-diff-number`
# carries `allow_alias = true`, so the two fixtures pin both branches of the
# message.
set(fold_alias_suffix
    " (If you are using allow_alias, please assign the same number to each enum value name.)")
require_contains(enum_case_desc_err "${fold_alias_suffix}"
    "U-51: the folded-name suffix without allow_alias")
require_contains(enum_diff_desc_err "${fold_alias_suffix}"
    "U-51: the folded-name suffix with allow_alias")

if(FULL_BUILD)
    foreach(name enum-name-collision-case enum-name-collision-prefix)
        run_fail(enum_src_out enum_src_err
            ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${name}.proto")
        require_empty(enum_src_out "The ${name} source stdout")
        require_contains(enum_src_err "same name as" "The ${name} source refusal")
        require_contains(enum_src_err "${fold_alias_suffix}"
            "U-51: the ${name} source suffix")
    endforeach()
    # `enum-alias-diff-number` writes `allow_alias = true` without a shared
    # number, so B7's CR-26 rule refuses the source before the folded-name
    # sweep can run - exactly the order protoc's parser uses (`ValidateEnum`
    # runs before the descriptor builder). The descriptor half above keeps the
    # folded-name refusal.
    run_fail(enum_diff_src_out enum_diff_src_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/enum-alias-diff-number.proto")
    require_empty(enum_diff_src_out "The enum-diff source stdout")
    require_contains(enum_diff_src_err "no enum values share field numbers"
        "The enum-diff source refusal")
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

    # U-25: a custom json_name on an extension field is refused; the equal
    # spelling is accepted. Extension fields are not decoded from a `.pbs`
    # (E6), so this is a source-text rule with no descriptor half.
    run_fail(ext_json_out ext_json_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/ext-json-name.proto")
    require_empty(ext_json_out "The extension json_name refusal stdout")
    require_contains(ext_json_err
        "option json_name is not allowed on extension fields."
        "The extension json_name refusal")

    run_ok(ext_json_equal_out ext_json_equal_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/ext-json-name-equal.proto")
    require_contains(ext_json_equal_out "struct Extendable"
        "The equal extension json_name accepted header")
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

# --- U-02/U-03: the file-scope name rules through both doors -----------------
#
# The names lane owns the rules; this block proves the two input paths reach
# the SAME verdict and the SAME diagnostic, so neither frontend can lose or
# rename the declaration on its way to the emitter. The accepted half
# (`scope-easypb`) is in the paired loop above, which compares the headers byte
# for byte and lets the build compile the descriptor-path one.
run_fail(scope_desc_out scope_desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/scope-globals.pbs")
require_empty(scope_desc_out "The global-typedef refusal stdout")
require_contains(scope_desc_err "message 'int32_t'"
    "U-03: the global-typedef refusal names the message")
require_contains(scope_desc_err "global typedef 'int32_t'"
    "U-03: the global-typedef refusal names the typedef")
if(FULL_BUILD)
    run_fail(scope_src_out scope_src_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/scope-globals.proto")
    require_empty(scope_src_out "The global-typedef refusal stdout on the source path")
    require_contains(scope_src_err "message 'int32_t'"
        "U-03: the source-path refusal names the message")
    require_contains(scope_src_err "global typedef 'int32_t'"
        "U-03: the source-path refusal names the typedef")
    # U-30: the source door carries the position of the refused declaration;
    # a `.pbs` carries no source text, so its sentence has no coordinates.
    # The two doors must agree on the verdict and on the sentence, not on a
    # prefix one of them cannot know.
    if(NOT scope_src_err MATCHES "scope-globals[.]proto:[0-9]+:[0-9]+: ")
        message(FATAL_ERROR
            "The U-03 source refusal carries no file:line:column: ${scope_src_err}")
    endif()
    if(scope_desc_err MATCHES "scope-globals[.]proto:[0-9]+:[0-9]+: ")
        message(FATAL_ERROR
            "The descriptor refusal fabricated a source position: ${scope_desc_err}")
    endif()
endif()

# --- V-01/V-02: the complete file-scope name rules through both doors --------
#
# The names lane owns the rules; this block proves the two input paths reach the
# SAME verdict and the same sentence for the rules the extended occupant list
# adds: the complete runtime declaration list under `package easypb;` (V-01),
# the preamble's C library and implementation names in the global namespace
# (V-02), and the reserved [lex.name] spelling of a top-level declaration
# (V-02). The accepted twin (`scope-nested-reserved`) is in the paired loop
# above: both doors accept it and emit the same header, which the build
# compiles.
set(scope_name_refusals
    "scope-runtime|message 'exception'|runtime type 'exception'"
    "scope-implementation-name|message '__gnu_cxx'|C++ library namespace '__gnu_cxx'"
    "scope-global-name|message 'memcpy'|C library function 'memcpy'"
    "scope-reserved-form|message '_Foo'|reserved C++ identifier form")

foreach(entry ${scope_name_refusals})
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 scope_name)
    list(GET parts 1 scope_needle_a)
    list(GET parts 2 scope_needle_b)

    run_fail(${scope_name}_desc_out ${scope_name}_desc_err
        ${CODEGEN} --descriptor-set "${DATA_DIR}/${scope_name}.pbs")
    require_empty(${scope_name}_desc_out
        "The ${scope_name} descriptor stdout")
    require_contains(${scope_name}_desc_err "${scope_needle_a}"
        "The ${scope_name} descriptor refusal names the declaration")
    require_contains(${scope_name}_desc_err "${scope_needle_b}"
        "The ${scope_name} descriptor refusal names the occupant or rule")

    if(FULL_BUILD)
        run_fail(${scope_name}_src_out ${scope_name}_src_err
            ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${scope_name}.proto")
        require_empty(${scope_name}_src_out
            "The ${scope_name} source stdout")
        require_contains(${scope_name}_src_err "${scope_needle_a}"
            "The ${scope_name} source refusal names the declaration")
        require_contains(${scope_name}_src_err "${scope_needle_b}"
            "The ${scope_name} source refusal names the occupant or rule")
        # U-30: the source door positions the refused declaration; a `.pbs`
        # carries no source text, so its sentence has no coordinates.
        if(NOT "${${scope_name}_src_err}" MATCHES "${scope_name}[.]proto:[0-9]+:[0-9]+: ")
            message(FATAL_ERROR
                "The ${scope_name} source refusal carries no file:line:column: ${${scope_name}_src_err}")
        endif()
        if("${${scope_name}_desc_err}" MATCHES "${scope_name}[.]proto:[0-9]+:[0-9]+: ")
            message(FATAL_ERROR
                "The ${scope_name} descriptor refusal fabricated a source position: ${${scope_name}_desc_err}")
        endif()
    endif()
endforeach()

# --- R05: the accepted twins of the proto3_optional and MessageSet rules ------
#
# The parity lane refuses the crafted shapes; these three pairs are the accept
# side, driven through the paired loop above (identical headers byte for byte,
# and the build compiles the descriptor-path header). The assertions name the
# emitted field so a verdict that accepted the schema but lost the field cannot
# pass.
run_ok(p3opt_ok_out p3opt_ok_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/p3opt-ok.pbs")
require_empty(p3opt_ok_err "The p3opt-ok descriptor stderr")
require_contains(p3opt_ok_out "::int32_t f = 0;" "U-11: the optional field")
require_contains(p3opt_ok_out "bool has_f =" "U-11: explicit presence")

run_ok(oneof_contiguous_out oneof_contiguous_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/oneof-contiguous.pbs")
require_empty(oneof_contiguous_err "The oneof-contiguous descriptor stderr")
require_contains(oneof_contiguous_out "::int32_t a = 0;" "U-21: the first member")
require_contains(oneof_contiguous_out "::int32_t b = 0;" "U-21: the ordinary field")

run_ok(message_set_out message_set_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/message-set-empty.pbs")
require_empty(message_set_err "The message-set-empty descriptor stderr")
require_contains(message_set_out "struct EmptyMessageSet" "U-10: the MessageSet")

# --- U-58: the descriptor decoder is strict about wire types -----------------
#
# `ReservedRange.start` is a varint field. Written as FIXED32 it used to be
# accepted and silently dropped from the model, so the range disappeared and
# the schema generated a header describing something else. A converted value is
# data loss, not a tolerance: the descriptor door must refuse the set, and no
# header may escape. The message decoder keeps its documented conversions
# (`wire.type.conversions` owns that contract), so the strictness is the
# descriptor decoder's alone.
run_fail(wire_type_out wire_type_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/descriptor-wire-fixed32.pbs")
require_empty(wire_type_out "The wrong-wire-type refusal stdout")
require_contains(wire_type_err "cannot decode descriptor set"
    "The wrong-wire-type refusal")
require_contains(wire_type_err "wire type 5"
    "The wrong-wire-type refusal names the wire type it found")
require_contains(wire_type_err "wire type 0 is required"
    "The wrong-wire-type refusal names the required wire type")

# --- R06: semantic validation gaps (U-14, U-29, U-56, U-57) ------------------
#
# Every refused shape is driven through both doors when a source twin can
# carry it: the descriptor half is crafted from the `.txtpb` beside the
# `.proto` (protoc refuses the text, so it cannot write that descriptor).

# U-14: the reserved declarations of an enum nested in a message. The
# descriptor door used to skip `message.enum_type` entirely.
run_fail(nested_range_out nested_range_err
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/nested-enum-reserved-invalid.pbs")
require_empty(nested_range_out "The nested inverted-range refusal stdout")
require_contains(nested_range_err "less than its start"
    "The nested inverted-range refusal")
if(FULL_BUILD)
    run_fail(nested_range_src_out nested_range_src_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/nested-enum-reserved-invalid.proto")
    require_empty(nested_range_src_out
        "The nested inverted-range refusal stdout on the source path")
    require_contains(nested_range_src_err
        "reserved range end is smaller than its start"
        "The nested inverted-range refusal on the source path")
endif()

run_fail(nested_reuse_out nested_reuse_err
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/nested-enum-reserved-reuse.pbs")
require_empty(nested_reuse_out "The nested reserved-number refusal stdout")
require_contains(nested_reuse_err "uses a number reserved"
    "The nested reserved-number refusal")
if(FULL_BUILD)
    run_fail(nested_reuse_src_out nested_reuse_src_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/nested-enum-reserved-reuse.proto")
    require_empty(nested_reuse_src_out
        "The nested reserved-number refusal stdout on the source path")
    require_contains(nested_reuse_src_err "enum value number is reserved"
        "The nested reserved-number refusal on the source path")
endif()

# U-29: an integral default may not start with whitespace. The dedicated
# sentence replaces the range diagnostic the finding reports, and the real
# overflow keeps that range diagnostic (the schema unit test pins it).
run_fail(space_int_out space_int_err
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/defaults-leading-space-int.pbs")
require_empty(space_int_out "The leading-space integral refusal stdout")
require_contains(space_int_err "leading whitespace is not permitted"
    "The leading-space integral refusal")
require_absent(space_int_err "does not fit"
    "The leading-space integral refusal must not report a range overflow")

# U-56: the reserved boundary does not widen a field number. The accepted
# half is the reserved-range-end pair above; both doors refuse the field.
run_fail(field_number_out field_number_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/field-number-too-large.pbs")
require_empty(field_number_out "The oversized field-number descriptor stdout")
require_contains(field_number_err "outside the valid range 1..536870911"
    "The oversized field-number refusal on the descriptor path")
if(FULL_BUILD)
    run_fail(field_number_src_out field_number_src_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/field-number-too-large.proto")
    require_empty(field_number_src_out
        "The oversized field-number refusal stdout on the source path")
    require_contains(field_number_src_err "field number must be in range 1..536870911"
        "The oversized field-number refusal on the source path")
endif()

# U-57: a custom JSON name with an embedded NUL.
run_fail(json_nul_out json_nul_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/json-name-nul.pbs")
require_empty(json_nul_out "The NUL json_name refusal stdout")
require_contains(json_nul_err "json_name cannot have embedded null characters"
    "The NUL json_name refusal")
if(FULL_BUILD)
    run_fail(json_nul_src_out json_nul_src_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/json-name-nul.proto")
    require_empty(json_nul_src_out
        "The NUL json_name refusal stdout on the source path")
    require_contains(json_nul_src_err
        "json_name cannot have embedded null characters"
        "The NUL json_name refusal on the source path")
endif()

# --- R07: the owner-decided option exceptions (U-09, U-15, U-55) -------------
#
# U-15 / E15: EasyProtoBuf has one runtime, so `optimize_for = LITE_RUNTIME`
# is validated and ignored and the reference compiler's ban on importing a
# lite file is not modelled. The pair must be accepted on both doors, emit the
# same header, and stay diagnostic-free: the exception is documentation, not a
# new warning. The descriptor half is crafted because protoc refuses to
# compile the source text it mirrors.
run_ok(lite_import_desc_out lite_import_desc_err
    ${CODEGEN} --descriptor-set --target nonlite-imports-lite.proto
    "${DATA_DIR}/nonlite-imports-lite.pbs")
require_empty(lite_import_desc_err "The lite-import descriptor stderr")
require_contains(lite_import_desc_out "struct NonLiteMessage"
    "U-15: the non-lite importer on the descriptor path")
require_contains(lite_import_desc_out "#include \"lite-runtime-dep.pb.hpp\""
    "U-15: the importer includes the lite dependency header")
if(FULL_BUILD)
    run_ok(lite_import_src_out lite_import_src_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/nonlite-imports-lite.proto")
    require_empty(lite_import_src_err "The lite-import source stderr")
    require_identical_headers(lite_import_desc_out lite_import_src_out
        "The source and descriptor paths disagree for nonlite-imports-lite")
endif()

# --- R08: option values by value (U-08) --------------------------------------
#
# `lazy = false`, `unverified_lazy = false`, `jstype = JS_NORMAL` and
# `packed = false` on a map are the option defaults. The rule keys on the
# value, not the flag, so all four are accepted on both doors and change
# nothing: the option-carrying header must equal the option-less control's on
# each door. The paired loop above already proves each door agrees with itself;
# these two comparisons extend that to the control, which is the byte-level
# proof that the default values are no-ops. The two fixtures have different
# logical names, so the control's name is normalized to the option fixture's
# before the comparison; every other byte must match.
file(READ "${WORK_DIR}/descriptor/option-value-defaults.pb.hpp"
    option_defaults_desc)
file(READ "${WORK_DIR}/descriptor/option-value-defaults-control.pb.hpp"
    option_control_desc)
require_contains(option_defaults_desc "struct ValueDefaults"
    "U-08: the option-carrying descriptor header")
string(REPLACE "option-value-defaults-control.proto"
    "option-value-defaults.proto" option_control_desc
    "${option_control_desc}")
require_identical_headers(option_defaults_desc option_control_desc
    "U-08: the default-valued options changed the descriptor header")
if(FULL_BUILD)
    file(READ "${WORK_DIR}/source/option-value-defaults.pb.hpp"
        option_defaults_src)
    file(READ "${WORK_DIR}/source/option-value-defaults-control.pb.hpp"
        option_control_src)
    string(REPLACE "option-value-defaults-control.proto"
        "option-value-defaults.proto" option_control_src
        "${option_control_src}")
    require_identical_headers(option_defaults_src option_control_src
        "U-08: the default-valued options changed the source header")
endif()

# V-16: an explicit `packed = false` on a map is accepted *and* kept. The
# print path writes `[packed=false]` only when `FieldOptions.has_packed` is
# set, so the marker proves presence and the value proves `packed` is false;
# the two doors must print the same tree. `option-value-defaults` carries the
# map spelling beside the other three default-valued options.
execute_process(
    COMMAND ${CODEGEN} --descriptor-set --print-descriptor
        option-value-defaults.pbs
    WORKING_DIRECTORY "${DATA_DIR}"
    RESULT_VARIABLE option_print_desc_result
    OUTPUT_VARIABLE option_print_desc_out
    ERROR_VARIABLE option_print_desc_err)
if(NOT option_print_desc_result EQUAL 0)
    message(FATAL_ERROR
        "The option-value-defaults descriptor print failed (${option_print_desc_result}): ${option_print_desc_err}")
endif()
require_empty(option_print_desc_err "The V-16 descriptor print stderr")
require_contains(option_print_desc_out "unpacked_map = 4 [packed=false]"
    "V-16: the descriptor print of the map's explicit packed=false")
if(FULL_BUILD)
    execute_process(
        COMMAND ${CODEGEN} --print-descriptor option-value-defaults.proto
        WORKING_DIRECTORY "${DATA_DIR}"
        RESULT_VARIABLE option_print_src_result
        OUTPUT_VARIABLE option_print_src_out
        ERROR_VARIABLE option_print_src_err)
    if(NOT option_print_src_result EQUAL 0)
        message(FATAL_ERROR
            "The option-value-defaults source print failed (${option_print_src_result}): ${option_print_src_err}")
    endif()
    require_empty(option_print_src_err "The V-16 source print stderr")
    require_contains(option_print_src_out "unpacked_map = 4 [packed=false]"
        "V-16: the source print of the map's explicit packed=false")
    if(NOT option_print_src_out STREQUAL option_print_desc_out)
        message(FATAL_ERROR
            "V-16: the source and descriptor prints of option-value-defaults differ\nsource:\n${option_print_src_out}\ndescriptor:\n${option_print_desc_out}")
    endif()
endif()

# --- R11: the emitter validates what it emits (U-20) -------------------------
#
# `--no-class` suppresses every struct, so a member default value has no
# initializer to describe. The custom-string-type refusal that `defaults-nul`
# triggers with a class must not fire without one: the caller declares the
# member, and the codecs - which reference `x.nul` but no default - are still
# emitted. The NUL default itself is accepted with the default std::string
# type (the paired loop above); the refusal is the one the owner's decision of
# 2026-10-21 made for a string type that cannot carry the length.
run_fail(no_class_nul_class_out no_class_nul_class_err
    ${CODEGEN} --descriptor-set --string-type "::my::String"
    "${DATA_DIR}/defaults-nul.pbs")
require_empty(no_class_nul_class_out
    "The custom-string-type refusal stdout without --no-class")
require_contains(no_class_nul_class_err "cannot carry it"
    "U-20: the embedded NUL stays a refusal while the member is emitted")

run_ok(no_class_nul_desc_out no_class_nul_desc_err
    ${CODEGEN} --descriptor-set --no-class --string-type "::my::String"
    "${DATA_DIR}/defaults-nul.pbs")
require_empty(no_class_nul_desc_err
    "The --no-class NUL-default descriptor stderr")
require_absent(no_class_nul_desc_out "struct NulDefaults"
    "U-20: --no-class must not emit the struct")
require_contains(no_class_nul_desc_out "inline void encode("
    "U-20: --no-class still emits the codecs")

if(FULL_BUILD)
    run_ok(no_class_nul_src_out no_class_nul_src_err
        ${CODEGEN} -I "${DATA_DIR}" --no-class --string-type "::my::String"
        "${DATA_DIR}/defaults-nul.proto")
    require_empty(no_class_nul_src_err
        "The --no-class NUL-default source stderr")
    require_identical_headers(no_class_nul_desc_out no_class_nul_src_out
        "The two doors disagree on the --no-class NUL default")
endif()

# `--no-class` skips MEMBER generation only. A schema error still fails: the
# same flag on a field number outside 1..536870911 is refused by the semantics
# layer before the emitter is asked for anything.
run_fail(no_class_number_out no_class_number_err
    ${CODEGEN} --descriptor-set --no-class
    "${DATA_DIR}/field-number-too-large.pbs")
require_empty(no_class_number_out "The --no-class field-number stdout")
require_contains(no_class_number_err "outside the valid range 1..536870911"
    "U-20: --no-class keeps schema-level refusals")

# --- R14: reserved-name warnings and the two map_entry diagnostics (U-50, U-62)
#
# U-50: protoc accepts a reserved name that is not an identifier and prints a
# warning, once per name and on both the message and the enum spelling. The
# verdict stays exit 0 and the header must still be written; the warning names
# the offending text. `reserved "ok"` in the same fixture is the control: a
# legal identifier must not produce a warning.
if(FULL_BUILD)
    run_ok(reserved_warn_out reserved_warn_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/reserved-non-identifier.proto")
    require_contains(reserved_warn_out "struct M"
        "U-50: the header is written beside the warning")
    require_contains(reserved_warn_out "enum E"
        "U-50: the enum declaration is written beside the warning")
    require_contains(reserved_warn_err
        "warning: Reserved name \"foo bar\" is not a valid identifier."
        "U-50: the message reserved-name warning")
    require_contains(reserved_warn_err
        "warning: Reserved name \"x y\" is not a valid identifier."
        "U-50: the enum reserved-name warning")
    require_absent(reserved_warn_err "Reserved name \"ok\""
        "U-50: a legal identifier must not warn")
endif()

# V-30: the explicit-setting sentence wins for every value shape, because
# protoc validates the message option before interpreting the value. The
# value-kind rule still answers rows without a refusal of their own.
if(FULL_BUILD)
    run_fail(map_entry_message_out map_entry_message_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/map-entry-message-value.proto")
    require_empty(map_entry_message_out "The map_entry message-value stdout")
    require_contains(map_entry_message_err
        "map_entry should not be set explicitly"
        "V-30: the message value gets the row's refusal")
    require_absent(map_entry_message_err
        "Value must be identifier for boolean option"
        "V-30: the message value must not name the value-kind rule")

    run_fail(map_entry_primitive_out map_entry_primitive_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/map-entry-primitive-value.proto")
    require_empty(map_entry_primitive_out "The map_entry primitive-value stdout")
    require_contains(map_entry_primitive_err
        "map_entry should not be set explicitly"
        "V-30: the primitive value gets the row's refusal")
    require_absent(map_entry_primitive_err
        "Value must be identifier for boolean option"
        "V-30: the primitive value must not name the value-kind rule")
endif()

# --- S05 (V-25): print mode runs the MessageSet check -----------------------
#
# The generation path refuses a message that declares
# `message_set_wire_format` and ordinary fields; `--print-descriptor` used to
# print the same tree and exit 0. The rule is reference-free, so print mode runs
# the same pass without linking and both doors must answer the same sentence.
# The accepted twin (`message-set-empty`, paired above) is the control: it still
# prints on both doors.
set(s05_parity "${DATA_DIR}/../../parity/data")

run_fail(s05_ms_gen_desc_out s05_ms_gen_desc_err
    ${CODEGEN} --descriptor-set "${s05_parity}/message-set-fields.pbs")
require_empty(s05_ms_gen_desc_out "V-25 generation stdout (descriptor)")
require_contains(s05_ms_gen_desc_err
    "a MessageSet cannot have fields, only extensions"
    "V-25 the generation refusal (descriptor)")

if(FULL_BUILD)
    # The print mode needs the parser profile; the lite profile keeps the
    # generation half above and has no print mode to compare against.
    run_fail(s05_ms_print_desc_out s05_ms_print_desc_err
        ${CODEGEN} --descriptor-set --print-descriptor
        "${s05_parity}/message-set-fields.pbs")
    require_empty(s05_ms_print_desc_out "V-25 print stdout (descriptor)")
    if(NOT s05_ms_gen_desc_err STREQUAL s05_ms_print_desc_err)
        message(FATAL_ERROR
            "V-25 generation and print disagree on the .pbs:\ngeneration:\n${s05_ms_gen_desc_err}\nprint:\n${s05_ms_print_desc_err}")
    endif()

    run_fail(s05_ms_gen_src_out s05_ms_gen_src_err
        ${CODEGEN} -I "${s05_parity}"
        "${s05_parity}/message-set-fields.proto")
    require_empty(s05_ms_gen_src_out "V-25 generation stdout (source)")
    run_fail(s05_ms_print_src_out s05_ms_print_src_err
        ${CODEGEN} --print-descriptor
        "${s05_parity}/message-set-fields.proto")
    require_empty(s05_ms_print_src_out "V-25 print stdout (source)")
    # The sentence and the position agree; the label does not, because the
    # source print mode has always named the input as typed while generation
    # names the logical file (V-25 fixes the verdict, not that spelling).
    foreach(s05_ms_src_err s05_ms_gen_src_err s05_ms_print_src_err)
        if(NOT "${${s05_ms_src_err}}" MATCHES
                "message-set-fields[.]proto:[0-9]+:[0-9]+:")
            message(FATAL_ERROR
                "V-25 the source refusal carries no position: ${${s05_ms_src_err}}")
        endif()
        require_contains(${s05_ms_src_err}
            "a MessageSet cannot have fields, only extensions"
            "V-25 the source refusal carries the generation sentence")
    endforeach()

    run_ok(s05_ms_empty_pbs_out s05_ms_empty_pbs_err
        ${CODEGEN} --descriptor-set --print-descriptor
        "${DATA_DIR}/message-set-empty.pbs")
    require_empty(s05_ms_empty_pbs_err "V-25 valid MessageSet print stderr")
    require_contains(s05_ms_empty_pbs_out "message EmptyMessageSet"
        "V-25 the valid MessageSet still prints")
    run_ok(s05_ms_empty_src_out s05_ms_empty_src_err
        ${CODEGEN} --print-descriptor "${DATA_DIR}/message-set-empty.proto")
    require_empty(s05_ms_empty_src_err
        "V-25 valid source MessageSet print stderr")
    require_contains(s05_ms_empty_src_out "message EmptyMessageSet"
        "V-25 the valid source MessageSet still prints")
endif()

# --- S08: the decoder's wire-level contract ---------------------------------
#
# Three shapes protoc's text format cannot spell: an `edition` enumerator the
# pinned descriptor.proto does not declare, unknown group nests of 100 and 101
# levels, and a tag with field number 0 in the middle of a message. The bytes
# are built by `data/make_descriptors.py` from `common/descriptor_wire.py`; the
# accepted fixtures declare the same logical file, so they must generate one
# header and print one tree. The generation and the print door must agree on
# the verdict of every copy: protoc routes an undeclared edition enumerator to
# the unknown fields (the file then reads as proto2) and aborts on a 101st
# group level and on field number 0 (`WireFormat::SkipField`).

run_ok(wire_base_out wire_base_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/descriptor-wire-base.pbs")
require_empty(wire_base_err "The wire-base generation stderr")

foreach(name descriptor-wire-edition-unknown descriptor-wire-groups-100)
    run_ok(wire_out wire_err
        ${CODEGEN} --descriptor-set "${DATA_DIR}/${name}.pbs")
    require_empty(wire_err "The ${name} generation stderr")
    if(NOT wire_out STREQUAL wire_base_out)
        message(FATAL_ERROR
            "${name}: the ignored wire bytes changed the generated header")
    endif()
endforeach()

execute_process(
    COMMAND ${CODEGEN} --descriptor-set --print-descriptor
        "${DATA_DIR}/descriptor-wire-base.pbs"
    RESULT_VARIABLE wire_base_print_result
    OUTPUT_VARIABLE wire_base_print
    ERROR_VARIABLE wire_base_print_err)
if(NOT wire_base_print_result EQUAL 0)
    message(FATAL_ERROR
        "The wire-base print failed (${wire_base_print_result}): ${wire_base_print_err}")
endif()
require_empty(wire_base_print_err "The wire-base print stderr")

foreach(name descriptor-wire-edition-unknown descriptor-wire-groups-100)
    execute_process(
        COMMAND ${CODEGEN} --descriptor-set --print-descriptor
            "${DATA_DIR}/${name}.pbs"
        RESULT_VARIABLE wire_print_result
        OUTPUT_VARIABLE wire_print_out
        ERROR_VARIABLE wire_print_err)
    if(NOT wire_print_result EQUAL 0)
        message(FATAL_ERROR
            "The ${name} print failed (${wire_print_result}): ${wire_print_err}")
    endif()
    require_empty(wire_print_err "The ${name} print stderr")
    if(NOT wire_print_out STREQUAL wire_base_print)
        message(FATAL_ERROR
            "${name}: the ignored wire bytes changed the printed tree")
    endif()
    if(name STREQUAL "descriptor-wire-edition-unknown")
        require_absent(wire_print_out "edition="
            "The undeclared edition enumerator reached the printed model")
    endif()
endforeach()

run_fail(wire_deep_out wire_deep_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/descriptor-wire-groups-101.pbs")
require_empty(wire_deep_out "The 101-level generation stdout")
require_contains(wire_deep_err
    "group nesting depth exceeds the maximum of 100 levels"
    "The 101-level generation refusal")

run_fail(wire_deep_print_out wire_deep_print_err
    ${CODEGEN} --descriptor-set --print-descriptor
    "${DATA_DIR}/descriptor-wire-groups-101.pbs")
require_empty(wire_deep_print_out "The 101-level print stdout")
require_contains(wire_deep_print_err
    "group nesting depth exceeds the maximum of 100 levels"
    "The 101-level print refusal")

run_fail(wire_tag_out wire_tag_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/descriptor-wire-tag-zero.pbs")
require_empty(wire_tag_out "The tag-zero generation stdout")
require_contains(wire_tag_err "Invalid field number."
    "The tag-zero generation refusal")

run_fail(wire_tag_print_out wire_tag_print_err
    ${CODEGEN} --descriptor-set --print-descriptor
    "${DATA_DIR}/descriptor-wire-tag-zero.pbs")
require_empty(wire_tag_print_out "The tag-zero print stdout")
require_contains(wire_tag_print_err "Invalid field number."
    "The tag-zero print refusal")

# --- S12 (V-36, V-38, V-39, V-44, V-49, V-50): diagnostic text and positions
#
# Every row asserts the whole sentence, not a fragment both the old and the
# new text contain. The source rows carry the token the sentence is about:
# the field's type token (V-36), the RPC endpoint type token (V-38), the
# reserved NUMBER token (V-39), the redeclared service name (V-44) and the
# field that closes the by-value cycle (V-50). The descriptor rows carry the
# same sentence without a fabricated position, because a `.pbs` has no source
# text.

# V-36: `c.C` is declared in the second root, which is not imported. The old
# sentence named the package head `.c`; the full reference is `.c.C`.
if(FULL_BUILD)
    run_fail(v36_src_out v36_src_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/visibility-invisible-compound.proto"
        "${DATA_DIR}/visibility-invisible-compound-dep.proto")
    require_empty(v36_src_out "The V-36 source stdout")
    require_contains(v36_src_err
        "visibility-invisible-compound.proto:6:27: type \".c.C\" is defined in \"visibility-invisible-compound-dep.proto\", which is not visible from \"visibility-invisible-compound.proto\"; add the import of that file"
        "V-36: the full compound reference and its type token")
    require_absent(v36_src_err "type \".c\" is defined"
        "V-36: the package head is not called a type")
endif()

run_fail(v36_desc_out v36_desc_err
    ${CODEGEN} --descriptor-set --target visibility-invisible-compound.proto
    "${DATA_DIR}/visibility-invisible-compound-descriptor.pbs")
require_empty(v36_desc_out "The V-36 descriptor stdout")
require_contains(v36_desc_err
    "visibility-invisible-compound.proto: type \".c.C\" is defined in \"visibility-invisible-compound-dep.proto\", which is not visible from \"visibility-invisible-compound.proto\"; add the import of that file"
    "V-36: the descriptor door answers the same sentence")
if(v36_desc_err MATCHES
        "visibility-invisible-compound[.]proto:[0-9]+:[0-9]+:")
    message(FATAL_ERROR
        "V-36: the descriptor refusal fabricated a source position: ${v36_desc_err}")
endif()

# V-38: both endpoint types are references of one method, and each carries its
# own type-token position.
if(FULL_BUILD)
    run_fail(v38_in_out v38_in_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/rpc-unresolved-endpoint.proto")
    require_empty(v38_in_out "The V-38 request stdout")
    require_contains(v38_in_err
        "rpc-unresolved-endpoint.proto:5:29: method \".Greeter.Call\" input type: unresolved type MissingRequest in .Greeter.Call"
        "V-38: the request endpoint carries its type token")

    run_fail(v38_out_out v38_out_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/rpc-unresolved-response.proto")
    require_empty(v38_out_out "The V-38 response stdout")
    require_contains(v38_out_err
        "rpc-unresolved-response.proto:7:47: method \".Greeter.Call\" output type: unresolved type MissingReply in .Greeter.Call"
        "V-38: the response endpoint carries its own type token")
endif()

# V-39: a number conflict points at the number token, a name conflict at the
# name token.
if(FULL_BUILD)
    run_fail(v39_field_out v39_field_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/reserved-field-number-conflict.proto")
    require_empty(v39_field_out "The V-39 field stdout")
    require_contains(v39_field_err
        "reserved-field-number-conflict.proto:5:32: while loading \"reserved-field-number-conflict.proto\": field number is reserved"
        "V-39: the field number token")
    require_absent(v39_field_err
        "reserved-field-number-conflict.proto:5:28:"
        "V-39: the field name token is not the number conflict")

    run_fail(v39_enum_out v39_enum_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/reserved-enum-number-conflict.proto")
    require_empty(v39_enum_out "The V-39 enum stdout")
    require_contains(v39_enum_err
        "reserved-enum-number-conflict.proto:5:14: while loading \"reserved-enum-number-conflict.proto\": enum value number is reserved"
        "V-39: the enum value number token")
    require_absent(v39_enum_err
        "reserved-enum-number-conflict.proto:5:10:"
        "V-39: the enum value name token is not the number conflict")

    run_fail(v39_name_out v39_name_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/reserved-name-conflict.proto")
    require_empty(v39_name_out "The V-39 name stdout")
    require_contains(v39_name_err
        "reserved-name-conflict.proto:5:28: while loading \"reserved-name-conflict.proto\": field name is reserved: used"
        "V-39: the name conflict keeps the name token")
endif()

# V-44: a message/service collision says service and names both declarations.
if(FULL_BUILD)
    run_fail(v44_src_out v44_src_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/service-message-collision-msg.proto"
        "${DATA_DIR}/service-message-collision.proto")
    require_empty(v44_src_out "The V-44 source stdout")
    require_contains(v44_src_err
        "service-message-collision.proto:7:9: duplicate name .p.Svc: a service in \"service-message-collision.proto\"; already defined as a message in \"service-message-collision-msg.proto\""
        "V-44: the service is named and both declarations are spelled")
    require_absent(v44_src_err "duplicate type name .p.Svc"
        "V-44: a service is not called a duplicate type")
endif()

run_fail(v44_desc_out v44_desc_err
    ${CODEGEN} --descriptor-set --target service-message-collision.proto
    "${DATA_DIR}/service-message-collision-descriptor.pbs")
require_empty(v44_desc_out "The V-44 descriptor stdout")
require_contains(v44_desc_err
    "service-message-collision.proto: duplicate name .p.Svc: a service in \"service-message-collision.proto\" at [6, 0]; already defined as a message in \"service-message-collision-msg.proto\" at [4, 0]"
    "V-44: the descriptor door answers the same sentence")
if(v44_desc_err MATCHES "service-message-collision[.]proto:[0-9]+:[0-9]+:")
    message(FATAL_ERROR
        "V-44: the descriptor refusal fabricated a source position: ${v44_desc_err}")
endif()

# V-49: an invalid enumerator of an IMPORTED enum is blamed on its declaration
# file, not on the field that triggered the spelling. The name is illegal in
# C++ but legal to protoc's symbol rule, so only a descriptor can carry it; a
# descriptor has no source text, hence no line:column.
run_fail(v49_out v49_err
    ${CODEGEN} --descriptor-set --target imported-enum-bad-value-user.proto
    "${DATA_DIR}/imported-enum-bad-value.pbs")
require_empty(v49_out "The V-49 stdout")
require_contains(v49_err
    "imported-enum-bad-value.proto: Protobuf name '0bad' is not a valid C++ identifier"
    "V-49: the enum value declaration is blamed")
require_absent(v49_err "imported-enum-bad-value-user.proto:"
    "V-49: the using field is not blamed")

# V-50: the cycle refusal points at the field that closes the walk (B's `a`),
# not at a node of the cycle (A's declaration).
if(FULL_BUILD)
    run_fail(v50_src_out v50_src_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/message-cycle-closing-edge.proto")
    require_empty(v50_src_out "The V-50 source stdout")
    require_contains(v50_src_err
        "message-cycle-closing-edge.proto:6:24: recursive message value dependency: ::A -> ::B -> ::A"
        "V-50: the closing field carries the refusal")
    require_absent(v50_src_err "message-cycle-closing-edge.proto:5:9:"
        "V-50: the cycle node is not blamed")
endif()

run_fail(v50_desc_out v50_desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/message-cycle-closing-edge.pbs")
require_empty(v50_desc_out "The V-50 descriptor stdout")
require_contains(v50_desc_err
    "message-cycle-closing-edge.proto: recursive message value dependency: ::A -> ::B -> ::A"
    "V-50: the descriptor door answers the same sentence")
if(v50_desc_err MATCHES "message-cycle-closing-edge[.]proto:[0-9]+:[0-9]+:")
    message(FATAL_ERROR
        "V-50: the descriptor refusal fabricated a source position: ${v50_desc_err}")
endif()

# --- S13 (V-19, V-27, V-43, V-45): the documented strictness, pinned ---------
#
# `OPTIONS.md` documents these refusals in `Deliberate strictness and
# non-goals`. Each assertion below is the whole sentence the documentation
# quotes, on the door that produces it, so the text and the executable boundary
# cannot drift apart.

# V-19 (owner decision D-03): the protocol-reserved field-number block is
# refused in an IMPORTED file, not only in a positional root. The root carries
# no reserved number; protoc accepts the pair (the descriptor is the
# `--include_imports` set of the same two files) and checks only its command
# line inputs, while both EasyProtoBuf doors refuse the graph. `--target` names
# the clean root, so the refusal must come from the linked dependency.
run_fail(v19_desc_out v19_desc_err
    ${CODEGEN} --descriptor-set --target imported-reserved-number.proto
    "${DATA_DIR}/imported-reserved-number.pbs")
require_empty(v19_desc_out "V-19 descriptor stdout")
require_contains(v19_desc_err
    "imported-reserved-number-dep.proto: field \"bad\" number 19000 is inside the reserved range 19000..19999"
    "V-19: the descriptor door names the imported file and the field")
if(FULL_BUILD)
    run_fail(v19_src_out v19_src_err
        ${CODEGEN} -I "${DATA_DIR}"
        "${DATA_DIR}/imported-reserved-number.proto")
    require_empty(v19_src_out "V-19 source stdout")
    require_contains(v19_src_err "imported-reserved-number-dep.proto:"
        "V-19: the source door names the imported file")
    require_contains(v19_src_err
        "chain \"imported-reserved-number.proto\" -> \"imported-reserved-number-dep.proto\""
        "V-19: the source refusal carries the import chain")
    require_contains(v19_src_err
        "field numbers 19000..19999 are reserved by Protocol Buffers"
        "V-19: the source door carries the reserved-block sentence")
endif()

# V-27 (owner decision D-02): an out-of-range integral default is refused, not
# clamped, and the sentence names the declared kind. The signed fixture stops at
# its first field, so the unsigned half has its own one-field fixture.
foreach(s13_default_case
        "int-default-overflow|default does not fit signed 32-bit field"
        "uint-default-overflow|default does not fit unsigned 32-bit field")
    string(REPLACE "|" ";" s13_default_parts "${s13_default_case}")
    list(GET s13_default_parts 0 s13_default_fixture)
    list(GET s13_default_parts 1 s13_default_sentence)
    run_fail(s13_default_desc_out s13_default_desc_err
        ${CODEGEN} --descriptor-set
        "${DATA_DIR}/${s13_default_fixture}.pbs")
    require_empty(s13_default_desc_out
        "The ${s13_default_fixture} descriptor stdout")
    require_contains(s13_default_desc_err "${s13_default_sentence}"
        "V-27: the descriptor refusal (${s13_default_fixture})")
    if(FULL_BUILD)
        run_fail(s13_default_src_out s13_default_src_err
            ${CODEGEN} -I "${DATA_DIR}"
            "${DATA_DIR}/${s13_default_fixture}.proto")
        require_empty(s13_default_src_out
            "The ${s13_default_fixture} source stdout")
        require_contains(s13_default_src_err "${s13_default_sentence}"
            "V-27: the source refusal (${s13_default_fixture})")
    endif()
endforeach()

# V-43 (decision W-10): a group is refused on both doors. The compiled `.pbs`
# is protoc's own encoding of the refused source text: protoc supports proto2
# groups, this generator has no group kind.
if(FULL_BUILD)
    run_fail(v43_src_out v43_src_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/group-field.proto")
    require_empty(v43_src_out "V-43 source stdout")
    require_contains(v43_src_err "group fields are not supported"
        "V-43: the source refusal names the group keyword")
endif()
run_fail(v43_desc_out v43_desc_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/group-field.pbs")
require_empty(v43_desc_out "V-43 descriptor stdout")
require_contains(v43_desc_err
    "field \"legacy\" is a group, and this generator does not support group types"
    "V-43: the descriptor refusal names the field and the kind")

# V-45: a package component becomes a C++ namespace, so the four documented
# spellings are refused on both doors with the component named. The compiled
# `.pbs` beside each `.proto` is protoc's own encoding: protoc accepts all four
# (it escapes a keyword and writes `namespace std`), which is the divergence.
set(s13_package_cases
    "package-keyword|Protobuf package component 'class' is a C++ keyword and cannot be used as a namespace"
    "package-double-underscore|Protobuf package component '__foo' uses a reserved C++ identifier form: it contains a double underscore"
    "package-underscore-upper|Protobuf package component '_Bar' uses a reserved C++ identifier form: it begins with an underscore followed by an uppercase letter"
    "package-std|Protobuf package cannot generate the global C++ namespace 'std'")
foreach(s13_package_case ${s13_package_cases})
    string(REPLACE "|" ";" s13_package_parts "${s13_package_case}")
    list(GET s13_package_parts 0 s13_package_fixture)
    list(GET s13_package_parts 1 s13_package_sentence)
    run_fail(s13_package_desc_out s13_package_desc_err
        ${CODEGEN} --descriptor-set
        "${DATA_DIR}/${s13_package_fixture}.pbs")
    require_empty(s13_package_desc_out
        "The ${s13_package_fixture} descriptor stdout")
    require_contains(s13_package_desc_err "${s13_package_sentence}"
        "V-45: the descriptor refusal (${s13_package_fixture})")
    if(FULL_BUILD)
        run_fail(s13_package_src_out s13_package_src_err
            ${CODEGEN} -I "${DATA_DIR}"
            "${DATA_DIR}/${s13_package_fixture}.proto")
        require_empty(s13_package_src_out
            "The ${s13_package_fixture} source stdout")
        require_contains(s13_package_src_err "${s13_package_sentence}"
            "V-45: the source refusal (${s13_package_fixture})")
    endif()
endforeach()
