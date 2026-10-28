# The parity lane: one rule, two input paths.
#
# One lane for the six rows of the plan-5 follow-up's session S5 (W-16,
# W-36, W-37, W-44, W-45, W-68). Every row is a rule that used to exist on
# only one of the two frontends - the .proto parser and the .pbs descriptor
# reader - or that the two of them read differently. The lane's three claims:
#
#   1. THE ACCEPTED PAIR PRODUCES ONE HEADER. For every schema both paths
#      accept, the source path (.proto) and the descriptor path (.pbs) emit
#      the same bytes. A rule that lives on one path only shows up here as a
#      difference, even when both exits are zero.
#   2. THE HEADER COMPILES. A generated header that does not compile is a
#      failure; "the tool exited 0" is not a pass. The compile belongs to
#      the build: the lane's compile check target compiles every accepted
#      descriptor-path header with the tree's own compiler and standard, and
#      the byte-for-byte comparison above extends that proof to the source
#      path. The driver does not invoke the compiler itself, because cl.exe
#      started outside the Visual Studio environment cannot find its own
#      standard library headers (C1083 on <cstdint>).
#   3. THE REFUSAL IS THE SAME ON BOTH PATHS. Every shape the source path
#      refuses, the descriptor path refuses too - and the other way round -
#      with a diagnostic that names the offending thing.
#
# Two shapes cannot be written as .proto source at all, and the lane says so
# where it skips the source half:
#
#   - a reversed map entry (W-37): the source parser SYNTHESISES the entry in
#     key-then-value order, so a source file cannot carry the malformed
#     declaration; only a hand-built descriptor can;
#   - two containers holding one logical file (W-36): the source path reads
#     a file tree, which has one definition of a file by construction.
#
# Compatibility mode is deliberately more permissive than --target mode for
# an ABSOLUTE reference into a file the set does not carry (W-16, W-68): the
# thin single-file descriptor cannot know whether the name is external. The
# lane pins that divergence too, because it is intentional - see the parity
# lane's README and the follow-up ledger's Decisions table.
#
# Required arguments:
#   CODEGEN     the standalone codegen executable
#   DATA_DIR    the fixture directory
#   WORK_DIR    the scratch directory
#   FULL_BUILD  1 when the proto parser is available

foreach(required_var CODEGEN DATA_DIR WORK_DIR)
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

# A refusal is three things at once: a non-zero exit, an empty stdout (no
# half-written header escapes) and a diagnostic that names the offending
# thing. A crash that exits non-zero without a diagnostic is not a refusal.
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

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/descriptor")
file(MAKE_DIRECTORY "${WORK_DIR}/source")
file(MAKE_DIRECTORY "${WORK_DIR}/w68")

# --- the accepted pairs ------------------------------------------------------
#
# Each pair carries a shape one row is about, in the smallest form that still
# reaches the rule:
#
#   methods-ok   service methods with real message endpoints (W-16);
#   map-ok       a synthesised map entry in key-then-value order (W-37);
#   enum-ok      a proto3 enum whose first value is zero (W-44);
#   ext-ok       a proto2 extension range that leaves room for its field
#                (W-45);
#   ext-max      the accepted extension-range boundary: exclusive end 536870912,
#                one past the last extension number (CR-07);
#   message-set-empty  a MessageSet with no ordinary fields, which protoc
#                accepts and whose option has no header effect (CR-23);
#   retained-ranges  `reserved` ranges/names and an extension range, the
#                declarations the source print used to drop (V-15).
#   labels-ok    all three descriptor.proto labels, the accepted side of V-42:
#                the source grammar can spell every one, and both doors must
#                accept them and emit one header. The out-of-range value (99)
#                has no source spelling; the validity lane refuses it on the
#                descriptor door alone.
set(paired_fixtures
    methods-ok
    map-ok
    enum-ok
    ext-ok
    ext-max
    message-set-empty
    retained-ranges
    labels-ok
    opt-jstype-int64
    opt-lazy-message
    opt-extrange-declaration)

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

endforeach()

# --- V-15: the source print carries the retained declarations ---------------
#
# `--print-descriptor` reads one model on both doors. The source parser used to
# validate `reserved` and `extensions` in locals and drop them, so a `.proto`
# dump lost declarations the `.pbs` dump of the same file carried. The two dumps
# of `retained-ranges` must now be the same bytes; both commands run from the
# fixture directory so the logical file name is the same on both doors (the
# source door reports the spelling it was given). The explicit markers keep a
# backend that printed nothing from passing the equality test.
execute_process(
    COMMAND ${CODEGEN} --descriptor-set --print-descriptor retained-ranges.pbs
    WORKING_DIRECTORY "${DATA_DIR}"
    RESULT_VARIABLE retained_desc_result
    OUTPUT_VARIABLE retained_desc_out
    ERROR_VARIABLE retained_desc_err)
if(NOT retained_desc_result EQUAL 0)
    message(FATAL_ERROR
        "The retained-ranges descriptor print failed (${retained_desc_result}): ${retained_desc_err}")
endif()
require_empty(retained_desc_err "The retained-ranges descriptor print stderr")
require_contains(retained_desc_out "reserved 5 to 10;"
    "V-15: the descriptor print of the message reserved range")
require_contains(retained_desc_out "reserved \"old\";"
    "V-15: the descriptor print of the message reserved name")
require_contains(retained_desc_out "extensions 100 to 199;"
    "V-15: the descriptor print of the extension range")
require_contains(retained_desc_out "reserved 2 to 3;"
    "V-15: the descriptor print of the enum reserved range")
require_contains(retained_desc_out "reserved \"B\";"
    "V-15: the descriptor print of the enum reserved name")
if(FULL_BUILD)
    execute_process(
        COMMAND ${CODEGEN} --print-descriptor retained-ranges.proto
        WORKING_DIRECTORY "${DATA_DIR}"
        RESULT_VARIABLE retained_src_result
        OUTPUT_VARIABLE retained_src_out
        ERROR_VARIABLE retained_src_err)
    if(NOT retained_src_result EQUAL 0)
        message(FATAL_ERROR
            "The retained-ranges source print failed (${retained_src_result}): ${retained_src_err}")
    endif()
    require_empty(retained_src_err "The retained-ranges source print stderr")
    if(NOT retained_src_out STREQUAL retained_desc_out)
        message(FATAL_ERROR
            "V-15: the source and descriptor prints of retained-ranges differ\nsource:\n${retained_src_out}\ndescriptor:\n${retained_desc_out}")
    endif()
endif()

# --- W-16: method endpoints and duplicate names ------------------------------
#
# The parser used to drop every method, so the source path never checked a
# duplicate method name and never resolved an endpoint. Both frontends now
# keep the method's name, input and output types as written; the linker
# resolves them.

# A duplicate method name inside one service, on both paths.
expect_refusal("The descriptor path must refuse a duplicate method name"
    "duplicate method name \"A\" in .parity.S"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/methods-dup.pbs")
if(FULL_BUILD)
    expect_refusal("The source path must refuse a duplicate method name"
        "duplicate method name \"A\" in .parity.S"
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/methods-dup.proto")
endif()

# A method endpoint that names nothing. --target puts the descriptor path
# into complete mode, the mode the source path always runs in; without it
# the thin set cannot tell an external name from a typo (the divergence
# pinned below).
expect_refusal("The descriptor path must refuse an unresolved method endpoint"
    "method \".parity.S.A\" input type: unresolved type"
    ${CODEGEN} --descriptor-set --target methods-missing.proto
    "${DATA_DIR}/methods-missing.pbs")
if(FULL_BUILD)
    expect_refusal("The source path must refuse an unresolved method endpoint"
        "method \".parity.S.A\" input type: unresolved type"
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/methods-missing.proto")
endif()

# The documented divergence: compatibility mode reads an absolute endpoint
# the set does not carry as an external reference and warns (A5), exactly as
# it does for a field type.
run_ok(missing_compat_out missing_compat_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/methods-missing.pbs")
require_contains(missing_compat_err "not declared in the linked set"
    "The compatibility-mode warning for an external method endpoint")

# --- W-36: one logical file in two containers --------------------------------
#
# Identical copies merge into one file - the duplicate is dropped, not
# emitted twice - and different definitions are refused with both container
# names. The source path has one definition of a file by construction, so
# this rule is descriptor-only.

run_ok(w36_single_out w36_single_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/w36-same-a.pbs")
require_empty(w36_single_err "The single-container W-36 stderr")

run_ok(w36_merged_out w36_merged_err
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/w36-same-a.pbs" "${DATA_DIR}/w36-same-b.pbs")
file(WRITE "${WORK_DIR}/descriptor/w36-same.pb.hpp" "${w36_merged_out}")
require_empty(w36_merged_err "The merged W-36 stderr")
if(NOT w36_merged_out STREQUAL w36_single_out)
    message(FATAL_ERROR
        "Identical copies of one logical file must merge into the header of a single copy")
endif()

run_ok(w36_target_out w36_target_err
    ${CODEGEN} --descriptor-set --target w36_same.proto
    "${DATA_DIR}/w36-same-a.pbs" "${DATA_DIR}/w36-same-b.pbs")
require_empty(w36_target_err "The merged W-36 stderr in complete mode")
if(NOT w36_target_out STREQUAL w36_merged_out)
    message(FATAL_ERROR
        "The merged W-36 header must not depend on --target")
endif()

expect_refusal("The descriptor path must refuse two different definitions of one logical file"
    "two containers carry different definitions of one logical file"
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/w36-conflict-a.pbs" "${DATA_DIR}/w36-conflict-b.pbs")

# The diagnostic names both containers, so the user knows which two inputs
# disagree. The first container is named by the copy the set kept, the
# second by the file that disagrees with it.
execute_process(
    COMMAND ${CODEGEN} --descriptor-set
        "${DATA_DIR}/w36-conflict-a.pbs" "${DATA_DIR}/w36-conflict-b.pbs"
    RESULT_VARIABLE w36_conflict_result
    OUTPUT_VARIABLE w36_conflict_out
    ERROR_VARIABLE w36_conflict_err)
if(w36_conflict_result EQUAL 0)
    message(FATAL_ERROR "The W-36 conflict unexpectedly succeeded")
endif()
require_contains(w36_conflict_err "w36-conflict-a.pbs"
    "The W-36 conflict diagnostic must name the first container")
require_contains(w36_conflict_err "w36-conflict-b.pbs"
    "The W-36 conflict diagnostic must name the second container")

# --- U-04/U-07/U-52: identity is the effective semantic value ---------------
#
# One logical file in two containers must merge when the two copies describe
# the same schema, and must be refused when they do not. The rows below spell
# one option, label, range or default two ways; the two single-container
# headers are byte-identical, and the merge must accept BOTH orders and
# produce that same header, so the wire format can never depend on which
# container was named first. A genuinely different value stays a conflict in
# both orders.
function(expect_merge_accept description first second)
    run_ok(merge_first_out merge_first_err
        ${CODEGEN} --descriptor-set "${DATA_DIR}/${first}.pbs")
    run_ok(merge_second_out merge_second_err
        ${CODEGEN} --descriptor-set "${DATA_DIR}/${second}.pbs")
    require_empty(merge_first_err "${description}: single ${first} stderr")
    require_empty(merge_second_err "${description}: single ${second} stderr")
    if(NOT merge_first_out STREQUAL merge_second_out)
        message(FATAL_ERROR
            "${description}: the two single-container headers differ")
    endif()
    run_ok(merge_ab_out merge_ab_err
        ${CODEGEN} --descriptor-set
        "${DATA_DIR}/${first}.pbs" "${DATA_DIR}/${second}.pbs")
    run_ok(merge_ba_out merge_ba_err
        ${CODEGEN} --descriptor-set
        "${DATA_DIR}/${second}.pbs" "${DATA_DIR}/${first}.pbs")
    require_empty(merge_ab_err "${description}: merged stderr")
    require_empty(merge_ba_err "${description}: reverse merged stderr")
    if(NOT merge_ab_out STREQUAL merge_first_out)
        message(FATAL_ERROR
            "${description}: the merged header differs from a single container")
    endif()
    if(NOT merge_ba_out STREQUAL merge_ab_out)
        message(FATAL_ERROR
            "${description}: the merged header depends on the argument order")
    endif()
endfunction()

function(expect_merge_conflict description first second)
    expect_refusal("${description}: first order"
        "two containers carry different definitions of one logical file"
        ${CODEGEN} --descriptor-set
        "${DATA_DIR}/${first}.pbs" "${DATA_DIR}/${second}.pbs")
    expect_refusal("${description}: reverse order"
        "two containers carry different definitions of one logical file"
        ${CODEGEN} --descriptor-set
        "${DATA_DIR}/${second}.pbs" "${DATA_DIR}/${first}.pbs")
endfunction()

# U-04: `packed` is compared at the value `write_as_packed` emits. An absent
# proto3 option on a repeated packable field is packed; an explicit
# `packed = true` is the same file, an explicit `packed = false` is not. A
# field that cannot be packed is never packed, so an explicit false on it is
# the default. proto2 defaults to unpacked, so there the true spelling is the
# different definition.
expect_merge_accept("a proto3 packed default beside an explicit packed = true"
    w36-packed-default w36-packed-true)
expect_merge_accept("a proto2 packed default beside an explicit packed = false"
    w36-packed-proto2-default w36-packed-proto2-false)
expect_merge_accept("an unpackable repeated string beside packed = false"
    w36-packed-string-default w36-packed-string-false)
expect_merge_conflict("a proto3 packed default beside packed = false"
    w36-packed-default w36-packed-unpacked)
expect_merge_conflict("a proto2 packed default beside packed = true"
    w36-packed-proto2-default w36-packed-proto2-true)

# U-07: an absent label is LABEL_OPTIONAL, not "no label". The single headers
# are identical, so the pair is one file; required is a different definition.
expect_merge_accept("an absent label beside LABEL_OPTIONAL"
    w36-label-absent w36-label-explicit)
expect_merge_conflict("LABEL_OPTIONAL beside LABEL_REQUIRED"
    w36-label-explicit w36-label-required)

# U-07: an absent range endpoint is the protobuf default 0. An empty enum
# reserved range is [0, 0], which is a valid declaration and the same file as
# the explicit spelling; a different range is a different definition.
expect_merge_accept("an empty enum reserved range beside [0, 0]"
    w36-enum-reserved-empty w36-enum-reserved-zero)
expect_merge_conflict("an enum reserved range [0, 0] beside [2, 2]"
    w36-enum-reserved-zero w36-enum-reserved-two)

# U-07: a default value is the canonical text both frontends store, so the
# hexadecimal `0x0A` is the decimal `10`, while `11` is another value.
expect_merge_accept("an int32 default spelled 0x0A beside 10"
    w36-default-hex w36-default-decimal)
expect_merge_conflict("an int32 default 10 beside 11"
    w36-default-decimal w36-default-other)

# U-52: an unsupported class keeps its carrier. A file whose class comes from
# the edition field and one whose class comes from the syntax string are two
# different definitions, even when the stored edition value agrees; the merge
# must refuse the pair instead of silently keeping one copy.
expect_merge_conflict("an edition carrier beside a syntax carrier"
    w36-edition-empty w36-edition-absent)

# --- W-37: the map entry's declaration order ---------------------------------
#
# A map entry is key first, value second by declaration order. A hand-built
# descriptor whose entry declares the value first is malformed even when the
# numbers look right. The source parser cannot produce such an entry - it
# synthesises the two fields itself - so only the descriptor path is driven.

expect_refusal("The descriptor path must refuse a reversed map entry"
    "an order that declares the field numbered 1 first and the field numbered 2 second is required"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/map-reversed.pbs")

# --- W-44: the first proto3 enum value ---------------------------------------
#
# proto3 requires the first declared value to be zero. The check sits before
# allow_alias's early return, so an aliased enum cannot slip a non-zero first
# value past it.

expect_refusal("The descriptor path must refuse a non-zero first proto3 enum value"
    "the first proto3 enum value must be zero"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/enum-first.pbs")
if(FULL_BUILD)
    expect_refusal("The source path must refuse a non-zero first proto3 enum value"
        "the first proto3 enum value must be zero"
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/enum-first.proto")
endif()

# --- W-45: extension ranges ---------------------------------------------------
#
# A range is only usable in proto2, it must leave room for the fields the
# message declares, and two ranges of one message must not overlap. The
# source parser already refuses these shapes; the descriptor linker now
# enforces the same rules.

expect_refusal("The descriptor path must refuse a proto3 extension range"
    "allowed only in proto2"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/ext-p3.pbs")
if(FULL_BUILD)
    expect_refusal("The source path must refuse a proto3 extension range"
        "allowed only in proto2"
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/ext-p3.proto")
endif()

expect_refusal("The descriptor path must refuse a range that includes a declared field"
    "includes field \"a\" (5)"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/ext-field.pbs")
if(FULL_BUILD)
    expect_refusal("The source path must refuse a range that includes a declared field"
        "field number lies inside an extension range"
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/ext-field.proto")
endif()

expect_refusal("The descriptor path must refuse overlapping extension ranges"
    "overlaps range [100, 200)"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/ext-overlap.pbs")
if(FULL_BUILD)
    expect_refusal("The source path must refuse overlapping extension ranges"
        "extension ranges overlap"
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/ext-overlap.proto")
endif()

# --- CR-07: the extension-range upper bound --------------------------------
#
# The exclusive end may not exceed 536870912 (the last extension number plus
# one). The source parser bounds the endpoint as text; the descriptor model
# must refuse the same numbers, because a `.pbs` is the only door through
# which a range beyond the protocol maximum reaches the linker. `ext-max` in
# the accepted pair above is the boundary control.
expect_refusal("The descriptor path must refuse an extension range above the protocol maximum"
    "exclusive end exceeds 536870912"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/ext-high.pbs")
if(FULL_BUILD)
    expect_refusal("The source path must refuse an extension endpoint above the protocol maximum"
        "field number must be in range 1..536870911"
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/ext-high.proto")
endif()

# --- CR-06: reserved ranges and names ---------------------------------------
#
# Seven shapes the reference compiler refuses. Each has a `.proto` fixture for
# the source parser and a `.pbs` fixture encoded from the `.txtpb` beside it,
# because protoc refuses these shapes and therefore cannot write the
# descriptor. The two paths must agree on the verdict, and the descriptor
# diagnostic must name the reserved declaration it violates; the exact
# sentences differ deliberately (the source parser answers while parsing, the
# descriptor sweep answers over the model).
function(expect_reserved_refusal name descriptor_text source_text)
    expect_refusal("The descriptor path must refuse ${name}" "${descriptor_text}"
        ${CODEGEN} --descriptor-set "${DATA_DIR}/${name}.pbs")
    if(FULL_BUILD)
        expect_refusal("The source path must refuse ${name}" "${source_text}"
            ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${name}.proto")
    endif()
endfunction()

# message shape 1: a field number inside a reserved range.
expect_reserved_refusal(res-field-number
    "uses a number reserved by [3, 8)"
    "field number is reserved")
# message shape 2: a field name that is reserved.
expect_reserved_refusal(res-field-name
    "field name \"a\" is reserved in message \".parity.M\""
    "field name is reserved: a")
# message shape 3: an extension range overlapping a reserved range.
expect_reserved_refusal(res-ext-overlap
    "overlaps reserved range [7, 8)"
    "reserved and extension ranges overlap")
# message shape 4: the same reserved range twice.
expect_reserved_refusal(res-dup-range
    "overlaps range [3, 5)"
    "reserved ranges overlap")
# message shape 5: the same reserved name twice.
expect_reserved_refusal(res-dup-name
    "field name \"a\" is reserved multiple times"
    "duplicate reserved name a")
# enum shape 1: an enum value using a reserved number (inclusive range).
expect_reserved_refusal(enum-reserved
    "uses a number reserved by [0, 0]"
    "enum value number is reserved")
# enum shape 2: an enum value using a reserved name.
expect_reserved_refusal(enum-reserved-name
    "enum value name \"A\" is reserved"
    "enum value name is reserved: A")

# --- CR-23 / U-10: MessageSet with fields ------------------------------------
#
# The option changes the wire encoding, so an ordinary struct cannot represent
# it. protoc refuses a MessageSet that declares fields and accepts one that
# does not; the accepted `message-set-empty` pair above pins the second half.
# Since R05 the parser retains `message_set_wire_format` in the model instead of
# validating and dropping the statement (U-10), so the field-bearing shape is a
# verdict on BOTH doors now, and the same reference-free sweep answers it. The
# descriptor half stays crafted from `.txtpb` because protoc refuses the source
# and cannot write its descriptor.
expect_refusal("The descriptor path must refuse a MessageSet with fields"
    "a MessageSet cannot have fields"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/message-set-fields.pbs")
if(FULL_BUILD)
    expect_refusal("The source path must refuse a MessageSet with fields"
        "a MessageSet cannot have fields"
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/message-set-fields.proto")
endif()

# --- R05 (U-12): MessageSet in proto3 ----------------------------------------
#
# A MessageSet is encoded with the proto2 MessageSet wire format and proto3 has
# no extension ranges, so protoc refuses the option in a proto3 file even when
# the message declares no fields ("MessageSet is not supported in proto3.",
# descriptor.cc:7900-7904). The source door answers the same rule from the
# option it now retains; the descriptor half is crafted because protoc refuses
# the source and cannot write its descriptor.
expect_refusal("The descriptor path must refuse a proto3 MessageSet"
    "MessageSet is not supported in proto3"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/message-set-proto3.pbs")
if(FULL_BUILD)
    expect_refusal("The source path must refuse a proto3 MessageSet"
        "MessageSet is not supported in proto3"
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/message-set-proto3.proto")
endif()

# --- R05 (U-11/U-21): proto3_optional and the oneof layout -------------------
#
# Four crafted descriptors the reference compiler refuses and this project used
# to accept. Every one is descriptor-only: the source parser sets the flag only
# for an explicit proto3 `optional`, synthesizes the one-field oneof itself and
# appends it after every real oneof (B12), while a source oneof body is parsed
# as one block - so no source file can spell these shapes. The accepted twins
# live in the validity lane (`p3opt-ok`, `oneof-contiguous`), and
# `message-set-empty` above covers the MessageSet accept side.
expect_refusal("The descriptor path must refuse proto3_optional without a oneof"
    "must be a member of a one-field oneof"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/p3opt-no-oneof.pbs")
expect_refusal("The descriptor path must refuse proto3_optional in a proto2 file"
    "may only be set on proto3 fields"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/p3opt-proto2.pbs")
expect_refusal("The descriptor path must refuse proto3_optional in a two-field oneof"
    "must be a member of a one-field oneof"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/p3opt-two-field-oneof.pbs")
expect_refusal("The descriptor path must refuse a repeated proto3_optional field"
    "must be a member of a one-field oneof"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/p3opt-repeated.pbs")
expect_refusal("The descriptor path must refuse a synthetic oneof before a real one"
    "synthetic oneofs must be after all other oneofs"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/p3opt-synthetic-first.pbs")
expect_refusal("The descriptor path must refuse non-contiguous oneof members"
    "must be defined consecutively"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/oneof-not-contiguous.pbs")

# --- CR-46: optional names are not required fields -------------------------
#
# descriptor.proto marks every declaration name optional, so a `.pbs` with an
# absent name is a semantic defect, not a decode failure. The decoder used to
# throw "has no required field ..." and abort the whole set; the semantic name
# sweep now refuses the declaration with its own descriptor path. protoc also
# refuses these bytes (ValidateSymbolName, "Missing name."), so the verdict is
# parity in every case. The source grammar cannot spell a nameless
# declaration, hence descriptor-only fixtures.
expect_refusal("The descriptor path must refuse a field with no name"
    "field in .parity.M has no name"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/no-field-name.pbs")
expect_refusal("The descriptor path must refuse a message with no name"
    "message in .parity has no name"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/no-message-name.pbs")
expect_refusal("The descriptor path must refuse a service with no name"
    "service in .parity has no name"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/no-service-name.pbs")

# --- U-13/U-22: the option rules on both descriptor doors -------------------
#
# The descriptor model decodes the option values whose rules change a verdict,
# and the same rules answer on both doors. `jstype` on a 32-bit kind, either
# lazy flag on a scalar, and `verification = UNVERIFIED` beside a declaration
# are refused by protoc, by the source parser and (since R04) by the descriptor
# decoder's semantic pass. The accepted controls are in `paired_fixtures`
# above; each of them generates one header from both doors.
#
# An enumerator the pinned `descriptor.proto` does not declare is not a
# modeled value: protoc's proto2 decoder routes it to the unknown fields, so
# `jstype()` keeps its JS_NORMAL default. The schema must be accepted and its
# field generated normally; the `codegen.schema.semantics` unit test pins the
# stronger claim - the decoded option is absent, not a value the model keeps.
run_ok(jstype_unknown_out jstype_unknown_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/opt-jstype-unknown.pbs")
require_empty(jstype_unknown_err "The unknown-jstype stderr")
require_contains(jstype_unknown_out "::int64_t value"
    "The unknown-jstype header must carry the field")

function(expect_option_refusal name descriptor_text source_text)
    expect_refusal("The descriptor path must refuse ${name}" "${descriptor_text}"
        ${CODEGEN} --descriptor-set "${DATA_DIR}/${name}.pbs")
    if(FULL_BUILD)
        expect_refusal("The source path must refuse ${name}" "${source_text}"
            ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/${name}.proto")
    endif()
endfunction()

expect_option_refusal(opt-jstype-int32
    "jstype is only allowed on int64, uint64, sint64, fixed64 or sfixed64 fields"
    "jstype is only allowed on int64, uint64, sint64, fixed64 or sfixed64 fields")
expect_option_refusal(opt-lazy-int32
    "[lazy = true] can only be specified for submessage fields"
    "[lazy = true] can only be specified for submessage fields")
expect_option_refusal(opt-unverified-lazy-int32
    "[lazy = true] can only be specified for submessage fields"
    "[lazy = true] can only be specified for submessage fields")
expect_option_refusal(opt-extrange-unverified
    "cannot be marked UNVERIFIED while it has extension declarations"
    "cannot mark the extension range as UNVERIFIED")

# The declaration member rules are descriptor-only: the source door keeps the
# aggregate option body opaque (P1 exception E13), so a source file with a bad
# declaration is accepted today. protoc refuses all three shapes.
expect_refusal("The descriptor path must refuse a declaration outside its range"
    "names a number outside the range"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/opt-extrange-decl-bad-number.pbs")
expect_refusal("The descriptor path must refuse a number declared twice"
    "is declared more than once"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/opt-extrange-decl-duplicate.pbs")
expect_refusal("The descriptor path must refuse a declaration with one of full_name/type"
    "must set both \"full_name\" and \"type\", or be reserved"
    ${CODEGEN} --descriptor-set "${DATA_DIR}/opt-extrange-decl-partial.pbs")

# V-07: the four declaration clauses U-22 left out. Each shape is refused by
# protoc's `ValidateExtensionDeclaration` (descriptor.cc:8311-8375); the first
# keeps one full-name set across every range of one message. The accepted
# control is `opt-extrange-declaration` in `paired_fixtures` above.
expect_refusal("The descriptor path must refuse one full_name in two ranges"
    "Extension field name \".opt.ext\" is declared multiple times."
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/opt-extrange-decl-name-duplicate.pbs")
expect_refusal("The descriptor path must refuse a full_name without a leading dot"
    "\"opt.ext\" must have a leading dot to indicate the fully-qualified scope."
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/opt-extrange-decl-name-no-dot.pbs")
expect_refusal("The descriptor path must refuse a full_name with invalid identifiers"
    "\".opt ext\" contains invalid identifiers."
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/opt-extrange-decl-name-invalid.pbs")
expect_refusal("The descriptor path must refuse a named type with invalid identifiers"
    "\".opt Ext\" contains invalid identifiers."
    ${CODEGEN} --descriptor-set
    "${DATA_DIR}/opt-extrange-decl-type-invalid.pbs")

# --- W-68: the import include -------------------------------------------------
#
# The thin single-file descriptor names its imports but carries only the
# target. A header generated from it must still include the dependency's
# header - the include is what makes the header usable by a translation unit
# that needs the name - and it must equal the header the complete closure
# produces for the same schema.

run_ok(w68_dep_out w68_dep_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/w68-dep.pbs")
file(WRITE "${WORK_DIR}/w68/w68-dep.pb.hpp" "${w68_dep_out}")
require_empty(w68_dep_err "The W-68 dependency stderr")

# The thin set, compatibility mode. The missing import is a warning, not a
# refusal (the documented divergence), and the include is still emitted.
run_ok(w68_thin_out w68_thin_err
    ${CODEGEN} --descriptor-set "${DATA_DIR}/w68-root.pbs")
file(WRITE "${WORK_DIR}/w68/w68-root.pb.hpp" "${w68_thin_out}")
require_contains(w68_thin_err "not declared in the linked set"
    "The compatibility-mode warning for the W-68 dependency")

file(READ "${WORK_DIR}/w68/w68-root.pb.hpp" w68_thin_header)
require_contains(w68_thin_header "#include \"w68-dep.pb.hpp\""
    "The thin-set W-68 header must include the dependency's header")

# The complete closure, complete mode: the same schema with the dependency in
# the set. I2 byte parity is the promise that the include rewrite did not
# change the compatibility output.
run_ok(w68_closure_out w68_closure_err
    ${CODEGEN} --descriptor-set --target w68-root.proto
    "${DATA_DIR}/w68-root.pbs" "${DATA_DIR}/w68-dep.pbs")
require_empty(w68_closure_err "The W-68 complete-closure stderr")
if(NOT w68_closure_out STREQUAL w68_thin_out)
    message(FATAL_ERROR
        "The thin-set and complete-closure headers of W-68 differ")
endif()

# The header is self-contained once the dependency's header is beside it;
# the build's compile check includes exactly that pair.

# Complete mode refuses the thin set, because the import is genuinely absent.
expect_refusal("The descriptor path must refuse a missing import in complete mode"
    "missing import \"w68-dep.proto\""
    ${CODEGEN} --descriptor-set --target w68-root.proto
    "${DATA_DIR}/w68-root.pbs")

if(FULL_BUILD)
    run_ok(w68_src_out w68_src_err
        ${CODEGEN} -I "${DATA_DIR}" "${DATA_DIR}/w68-root.proto")
    require_empty(w68_src_err "The W-68 source path stderr")
    if(NOT w68_src_out STREQUAL w68_thin_out)
        message(FATAL_ERROR
            "The source path and the thin descriptor differ for W-68")
    endif()
endif()
