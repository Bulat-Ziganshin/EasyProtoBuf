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
#               accepted nesting is 31; Clang 22-23: their exponential cost
#               for a 31-level nested name - see tests/CMakeLists.txt)
#   SKIP_REASON the reason for COMPILE_ACCEPTED=0, quoted in the STATUS line
#               (default: "the compiler limit of this profile")

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
if(NOT DEFINED SKIP_REASON OR "${SKIP_REASON}" STREQUAL "")
    set(SKIP_REASON "the compiler limit of this profile")
endif()
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

# A refusal is a diagnostic, not a crash: the CLI reports every refusal with
# exit code 1 (main.cpp), so a crash - 0xC00000FD on Windows, a signal on
# POSIX - can never pass this check, and the stderr assertion below proves the
# sentence came from the refusal itself.
function(run_refused output_var error_var description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 1)
        message(FATAL_ERROR
            "${description}: expected the refusal exit code 1, got '${result}'"
            " (a crash is not a refusal)\nstdout:\n${output}\nstderr:\n${error}")
    endif()
    message(STATUS "${description}: refused with exit code ${result}")
    set(${output_var} "${output}" PARENT_SCOPE)
    set(${error_var} "${error}" PARENT_SCOPE)
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
# Two profiles cannot do this, each for its own reason: MSVC's compiler limit
# is 16 levels of nested struct definitions (C1046), and Clang 22.1.0-23.1.0
# resolves a 31-level nested name in exponential time. The limit itself does
# not move - it is the reference compiler's own, and the generator keeps
# printing the nesting the way it does - but these profiles cannot compile
# that header, so the compile and the runtime check below are the two steps
# they give up. Everything above and below them is compiler-free and still
# runs: the generation, the agreement of the two input paths, and the two
# refusals.
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
        "The compile and the runtime check are skipped for this profile "
        "(${SKIP_REASON}), so this run checks the generation, the agreement "
        "of the two input paths and the two refusals only.")
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

# --- the dependency-chain depth (CR-35) --------------------------------------
#
# 12 000 top-level messages, each holding the next one by value. The nesting
# depth is 1; the dependency graph is 12 000 edges deep, which is what used to
# end the process with STATUS_STACK_OVERFLOW (0xC00000FD) in the recursive
# cycle detector and topological sort, on both input paths. `run_ok` fails on
# that code, so a regression is a failed test rather than a silent crash. The
# accepted header is not compiled: it is a 9.5 MB declaration chain, and the
# claim under test is that the generator survives the walk, not that the
# compiler does.
set(chain dependency-chain)
run_ok(chain_desc_out chain_desc_err
    ${CODEGEN} --descriptor-set --out-dir "${WORK_DIR}/chain/descriptor"
    "${DATA_DIR}/${chain}.pbs")
require_empty(chain_desc_out "The chain descriptor stdout")
require_empty(chain_desc_err "The chain descriptor stderr")
file(READ "${WORK_DIR}/chain/descriptor/${chain}.pb.hpp" chain_desc_header)
if(chain_desc_header STREQUAL "")
    message(FATAL_ERROR "The chain descriptor path wrote an empty header")
endif()
# Both ends of the chain must be in the header: the walk is dependency-first,
# so the last declaration comes first and the first comes last.
string(FIND "${chain_desc_header}" "struct M11999" chain_last_pos)
string(FIND "${chain_desc_header}" "struct M0" chain_first_pos)
if(chain_first_pos EQUAL -1 OR chain_last_pos EQUAL -1)
    message(FATAL_ERROR
        "The chain header lacks a message at one end of the chain")
endif()

# Deterministic result: the same descriptor twice, two output roots, one
# header byte for byte.
run_ok(chain_desc2_out chain_desc2_err
    ${CODEGEN} --descriptor-set --out-dir "${WORK_DIR}/chain/descriptor-2"
    "${DATA_DIR}/${chain}.pbs")
require_empty(chain_desc2_out "The second chain descriptor stdout")
require_empty(chain_desc2_err "The second chain descriptor stderr")
file(READ "${WORK_DIR}/chain/descriptor-2/${chain}.pb.hpp" chain_desc2_header)
if(NOT chain_desc_header STREQUAL chain_desc2_header)
    message(FATAL_ERROR
        "CR-35: two runs of the chain produced different headers")
endif()

if(FULL_BUILD)
    run_ok(chain_src_out chain_src_err
        ${CODEGEN} -I "${DATA_DIR}" --out-dir "${WORK_DIR}/chain/source"
        "${DATA_DIR}/${chain}.proto")
    require_empty(chain_src_out "The chain source stdout")
    require_empty(chain_src_err "The chain source stderr")
    file(READ "${WORK_DIR}/chain/source/${chain}.pb.hpp" chain_src_header)
    if(NOT chain_src_header STREQUAL chain_desc_header)
        message(FATAL_ERROR
            "CR-35: the source and descriptor chain headers disagree")
    endif()
endif()

# --- the import-chain bound (U-05, U-06) --------------------------------------
#
# The import graph is untrusted input. The loader used to recurse once per
# import edge: a chain of ~1 950 files ended the process with
# STATUS_STACK_OVERFLOW (0xC00000FD) and an empty stderr, while protoc compiles
# chains of 3 000-3 500 files. Both walks are heap worklists now, and the
# chain depth is bounded at 1 000 files (max_import_chain_depth(), owner
# decision 10) with a sentence that names the depth, the limit and the
# offending file. 1 000 files is accepted, 1 001 is refused; a 5 000-file
# chain - the review's crash length, five times the bound - must be refused
# with that sentence rather than end the process.
#
# The source halves are written here, because a 5 000-file chain is not a
# fixture a repository can carry as 5 000 tracked files.
#
# The accepted chain is the SAME schema the `public-chain-1000.pbs` fixture
# carries, down to the file names, so the two doors are required to produce one
# header byte for byte: `c<i>.proto` publicly imports `c<i+1>.proto` and
# declares nothing.
set(public_chain_length 1000)
set(public_chain_dir "${WORK_DIR}/public-chain-source")
if(FULL_BUILD)
    file(REMOVE_RECURSE "${public_chain_dir}")
    file(MAKE_DIRECTORY "${public_chain_dir}")
    math(EXPR public_chain_last "${public_chain_length} - 1")
    foreach(index RANGE 0 ${public_chain_last})
        if(index EQUAL public_chain_last)
            file(WRITE "${public_chain_dir}/c${index}.proto" "")
        else()
            math(EXPR public_chain_next "${index} + 1")
            file(WRITE "${public_chain_dir}/c${index}.proto"
                "import public \"c${public_chain_next}.proto\";\n")
        endif()
    endforeach()
endif()

# The deep chain is an ordinary import chain, five times the bound: the chain
# under the last-but-1000 root is exactly 1 000 files deep, the chain under the
# next one up is 1 001, and the chain under the first is the whole 5 000.
set(import_chain_length 5000)
set(import_chain_dir "${WORK_DIR}/import-chain")
if(FULL_BUILD)
    file(REMOVE_RECURSE "${import_chain_dir}")
    file(MAKE_DIRECTORY "${import_chain_dir}")
    math(EXPR import_chain_last "${import_chain_length} - 1")
    math(EXPR import_chain_first_refused
        "${import_chain_length} - 1000 - 1")
    foreach(index RANGE 0 ${import_chain_last})
        if(index EQUAL import_chain_last)
            file(WRITE "${import_chain_dir}/d${index}.proto" "")
        else()
            math(EXPR import_chain_next "${index} + 1")
            file(WRITE "${import_chain_dir}/d${index}.proto"
                "import \"d${import_chain_next}.proto\";\n")
        endif()
    endforeach()
endif()

if(FULL_BUILD)
    # 1 000 files deep: accepted, and the whole walk is iterative, so the depth
    # costs memory (a 1 000-frame worklist) instead of call frames.
    run_ok(chain_ok_out chain_ok_err
        ${CODEGEN} -I "${public_chain_dir}"
        --out-dir "${WORK_DIR}/import-chain-out/accepted"
        "${public_chain_dir}/c0.proto")
    require_empty(chain_ok_out "The accepted import-chain stdout")
    require_empty(chain_ok_err "The accepted import-chain stderr")
    file(READ "${WORK_DIR}/import-chain-out/accepted/c0.pb.hpp"
        chain_ok_header)
    if(chain_ok_header STREQUAL "")
        message(FATAL_ERROR "The accepted import chain wrote an empty header")
    endif()

    # One file more than the bound: refused with the bound's own sentence.
    run_refused(chain_deep_out chain_deep_err "The 1 001-file import chain"
        ${CODEGEN} -I "${import_chain_dir}"
        --out-dir "${WORK_DIR}/import-chain-out/refused"
        "${import_chain_dir}/d${import_chain_first_refused}.proto")
    require_empty(chain_deep_out "The 1 001-file import-chain stdout")
    require_contains(chain_deep_err
        "import chain depth 1001 exceeds the limit of 1000 files"
        "The 1 001-file import-chain refusal")
    require_contains(chain_deep_err "d${import_chain_last}.proto"
        "The 1 001-file import-chain refusal names the offending file")

    # The review's crash length: five times the bound. The process must refuse
    # it with the same sentence, not end with 0xC00000FD - `run_refused` fails
    # on any exit code but 1, and the empty stderr of a crash would fail the
    # assertion below as well.
    run_refused(chain_long_out chain_long_err "The 5 000-file import chain"
        ${CODEGEN} -I "${import_chain_dir}"
        --out-dir "${WORK_DIR}/import-chain-out/long"
        "${import_chain_dir}/d0.proto")
    require_empty(chain_long_out "The 5 000-file import-chain stdout")
    require_contains(chain_long_err
        "import chain depth 1001 exceeds the limit of 1000 files"
        "The 5 000-file import-chain refusal")
endif()

# The descriptor half: the same graph arrives whole in one `.pbs`, with no
# filesystem walk, so the linker owns the bound there. 1 000 `import public`
# links is the deepest accepted chain - the length whose export sets used to be
# copied per file - and 20 000 is the length the review measured the crash at.
run_ok(public_chain_out public_chain_err
    ${CODEGEN} --descriptor-set --target c0.proto
    --out-dir "${WORK_DIR}/public-chain-1000"
    "${DATA_DIR}/public-chain-1000.pbs")
require_empty(public_chain_out "The accepted public-chain stdout")
require_empty(public_chain_err "The accepted public-chain stderr")
file(READ "${WORK_DIR}/public-chain-1000/c0.pb.hpp" public_chain_header)
if(public_chain_header STREQUAL "")
    message(FATAL_ERROR "The accepted public chain wrote an empty header")
endif()

# Both doors, one header: the accepted chain is one schema, so the `.proto`
# files written above and the committed `.pbs` must produce the same bytes.
if(FULL_BUILD)
    if(NOT chain_ok_header STREQUAL public_chain_header)
        message(FATAL_ERROR
            "The source path and the descriptor path disagree on the accepted "
            "public-import chain")
    endif()
endif()

run_refused(public_chain_long_out public_chain_long_err
    "The 20 000-file public-import chain"
    ${CODEGEN} --descriptor-set --target c0.proto
    --out-dir "${WORK_DIR}/public-chain-20000"
    "${DATA_DIR}/public-chain-20000.pbs")
require_empty(public_chain_long_out "The 20 000-file public-chain stdout")
require_contains(public_chain_long_err
    "import chain depth 20000 exceeds the limit of 1000 files"
    "The 20 000-file public-chain refusal")
require_contains(public_chain_long_err "c0.proto"
    "The 20 000-file public-chain refusal names the offending file")

# --- the wide proto3 optional message (V-21, V-22) ---------------------------
#
# One message with 4 096 explicit `optional` scalar fields. Every field owns a
# synthetic one-field oneof, so the message is as wide in oneof declarations as
# in fields: the shape on which a per-field linear source scan and a per-oneof
# field scan are quadratic. The lane asks for the same verdict the reference
# compiler gives - accepted - and for one header from both doors; it never
# asserts a duration, because the finding is about the shape's cost, not about
# a threshold a slow machine could flake on.
set(wide_optional wide-optional)
run_ok(wide_desc_out wide_desc_err
    ${CODEGEN} --descriptor-set
    --out-dir "${WORK_DIR}/wide-optional/descriptor"
    "${DATA_DIR}/${wide_optional}.pbs")
require_empty(wide_desc_out "The wide-optional descriptor stdout")
require_empty(wide_desc_err "The wide-optional descriptor stderr")
file(READ "${WORK_DIR}/wide-optional/descriptor/${wide_optional}.pb.hpp"
    wide_desc_header)
if(wide_desc_header STREQUAL "")
    message(FATAL_ERROR "The wide-optional descriptor path wrote an empty header")
endif()
# Both ends of the field list are emitted: a truncated walk cannot pass.
string(FIND "${wide_desc_header}" "::int32_t f0 = 0;" wide_first_pos)
string(FIND "${wide_desc_header}" "::int32_t f4095 = 0;" wide_last_pos)
if(wide_first_pos EQUAL -1 OR wide_last_pos EQUAL -1)
    message(FATAL_ERROR
        "The wide-optional header lacks a field at one end of the message")
endif()

if(FULL_BUILD)
    run_ok(wide_src_out wide_src_err
        ${CODEGEN} -I "${DATA_DIR}"
        --out-dir "${WORK_DIR}/wide-optional/source"
        "${DATA_DIR}/${wide_optional}.proto")
    require_empty(wide_src_out "The wide-optional source stdout")
    require_empty(wide_src_err "The wide-optional source stderr")
    file(READ "${WORK_DIR}/wide-optional/source/${wide_optional}.pb.hpp"
        wide_src_header)
    if(NOT wide_src_header STREQUAL wide_desc_header)
        message(FATAL_ERROR
            "The source path and the descriptor path disagree on the wide "
            "proto3 optional message")
    endif()
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

# --- W-46: the long-path prefix is a transport spelling -----------------------
#
# The same input and the same out-dir, written once plainly and once with an
# explicit prefix. The prefix must not become a UNC host ("//?/C:") and must
# not leak into the concatenation: the three runs produce the same header,
# byte for byte. The Windows-only section runs here; the input half needs the
# source frontend.
if(WIN32)
    set(prefix_dir "${WORK_DIR}/w46")
    file(MAKE_DIRECTORY "${prefix_dir}")

    run_ok(plain_out plain_err
        ${CODEGEN} --descriptor-set --out-dir "${prefix_dir}/plain"
        "${DATA_DIR}/${accepted}.pbs")
    require_empty(plain_out "The plain out-dir stdout")
    require_empty(plain_err "The plain out-dir stderr")

    run_ok(backslash_out backslash_err
        ${CODEGEN} --descriptor-set --out-dir "\\\\?\\${prefix_dir}/prefixed"
        "${DATA_DIR}/${accepted}.pbs")
    require_empty(backslash_out "The prefixed out-dir stdout")
    require_empty(backslash_err "The prefixed out-dir stderr")

    run_ok(forward_out forward_err
        ${CODEGEN} --descriptor-set --out-dir "//?/${prefix_dir}/forward"
        "${DATA_DIR}/${accepted}.pbs")
    require_empty(forward_out "The forward-slash out-dir stdout")
    require_empty(forward_err "The forward-slash out-dir stderr")

    file(READ "${prefix_dir}/plain/${accepted}.pb.hpp" plain_header)
    file(READ "${prefix_dir}/prefixed/${accepted}.pb.hpp" prefixed_header)
    file(READ "${prefix_dir}/forward/${accepted}.pb.hpp" forward_header)
    if(NOT plain_header STREQUAL prefixed_header)
        message(FATAL_ERROR "W-46: the \\\\?\\ out-dir changed the header")
    endif()
    if(NOT plain_header STREQUAL forward_header)
        message(FATAL_ERROR "W-46: the //?/ out-dir changed the header")
    endif()

    if(FULL_BUILD)
        # The input half: -I, the .proto argument and the .pbs argument all
        # carry the prefix. Each run must produce the plain descriptor header.
        run_ok(source_plain_out source_plain_err
            ${CODEGEN} --out-dir "${prefix_dir}/source-plain"
            -I "${DATA_DIR}" "${DATA_DIR}/${accepted}.proto")
        require_empty(source_plain_out "The source out-dir stdout")
        require_empty(source_plain_err "The source out-dir stderr")

        run_ok(source_prefixed_out source_prefixed_err
            ${CODEGEN} --out-dir "${prefix_dir}/source-prefixed"
            -I "\\\\?\\${DATA_DIR}"
            "\\\\?\\${DATA_DIR}/${accepted}.proto")
        require_empty(source_prefixed_out "The prefixed source stdout")
        require_empty(source_prefixed_err "The prefixed source stderr")

        run_ok(pbs_prefixed_out pbs_prefixed_err
            ${CODEGEN} --descriptor-set
            --out-dir "${prefix_dir}/pbs-prefixed"
            "\\\\?\\${DATA_DIR}/${accepted}.pbs")
        require_empty(pbs_prefixed_out "The prefixed descriptor stdout")
        require_empty(pbs_prefixed_err "The prefixed descriptor stderr")

        file(READ "${prefix_dir}/source-plain/${accepted}.pb.hpp"
            source_plain_header)
        file(READ "${prefix_dir}/source-prefixed/${accepted}.pb.hpp"
            source_prefixed_header)
        file(READ "${prefix_dir}/pbs-prefixed/${accepted}.pb.hpp"
            pbs_prefixed_header)
        if(NOT source_plain_header STREQUAL plain_header)
            message(FATAL_ERROR
                "W-46: the source path disagrees with the descriptor path")
        endif()
        if(NOT source_prefixed_header STREQUAL plain_header)
            message(FATAL_ERROR
                "W-46: the prefixed source path changed the header")
        endif()
        if(NOT pbs_prefixed_header STREQUAL plain_header)
            message(FATAL_ERROR
                "W-46: the prefixed .pbs argument changed the header")
        endif()
    endif()
endif()
