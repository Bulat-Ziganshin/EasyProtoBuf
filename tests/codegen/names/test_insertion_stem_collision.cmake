# The accepted CR-44 xfail: the insertion-macro stem is not injective.
#
# `A_B` is a top-level message; `A::B` is nested inside `A`. The documented
# flattening drops packages and joins lexical components with underscores, so
# both messages answer the same four `EASYPB_A_B_*` macros. The reference
# compiler has no insertion points at all, so there is no `protoc` output to
# compare against; the artifact is the generator's own compatibility contract,
# recorded in GENERATED_CODE.md ("Insertion points") and pinned by
# `tests/codegen/packages/cpp_names_test.cpp`.
#
# The test asserts the CURRENT direction: one schema generates two messages
# whose stems are equal. It passes while the artifact is present and FAILS when
# the stems become distinct. A failure here is not "the product broke": it
# means insertion points gained a versioned or injective format, so the
# accepted decision, the CR-44 row in triage.csv and the "Outside the property
# matrix" paragraph in property-catalogue.md must be re-evaluated and this test
# retired (or inverted). The message says so, because the next reader will not
# have the triage in front of them.
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

function(require_contains variable_name expected description)
    string(FIND "${${variable_name}}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${description}: missing '${expected}' in ${variable_name}")
    endif()
endfunction()

# Count one guard's occurrences: each message (or codec function) guards its
# insertion point once, so a shared stem guards twice; distinct stems guard
# once each.
function(require_occurrences variable_name stem expected description)
    string(REGEX MATCHALL "${stem}" matches "${${variable_name}}")
    list(LENGTH matches count)
    if(NOT count EQUAL expected)
        message(FATAL_ERROR
            "${description}: '${stem}' occurs ${count} time(s), expected ${expected}.\n"
            "CR-44 is registered as accepted: the insertion-macro stem flattens "
            "`A::B` and `A_B` to one spelling (triage.csv, accepted register). "
            "If the stems are now distinct, the artifact changed: re-decide "
            "CR-44, remove it from the accepted register, and retire or invert "
            "this xfail.")
    endif()
endfunction()

function(check_header description header_text)
    require_contains(header_text "struct A_B" "${description}")
    require_contains(header_text "struct A" "${description}")
    # Two messages share each stem: A_B and A::B both guard with EASYPB_A_B_*.
    require_occurrences(header_text "#ifdef EASYPB_A_B_EXTRA_FIELDS"
        2 "${description}")
    require_occurrences(header_text "#ifdef EASYPB_A_B_EXTRA_ENCODING"
        2 "${description}")
    require_occurrences(header_text "#ifdef EASYPB_A_B_EXTRA_DECODING"
        2 "${description}")
    require_occurrences(header_text "#ifdef EASYPB_A_B_EXTRA_POST_DECODING"
        2 "${description}")
    # The non-nested A keeps its own, distinct stem.
    require_occurrences(header_text "#ifdef EASYPB_A_EXTRA_FIELDS"
        1 "${description}")
endfunction()

execute_process(
    COMMAND ${CODEGEN} --descriptor-set "${DATA_DIR}/insertion-stem-collision.pbs"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE descriptor_header
    ERROR_VARIABLE descriptor_error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "The descriptor path now REFUSES the insertion-stem fixture (exit ${result}).\n"
        "CR-44 is registered as accepted, not fixed." "${descriptor_error}")
endif()
check_header("The descriptor-path header" "${descriptor_header}")

if(FULL_BUILD)
    execute_process(
        COMMAND ${CODEGEN} -I "${DATA_DIR}"
            "${DATA_DIR}/insertion-stem-collision.proto"
        RESULT_VARIABLE source_result
        OUTPUT_VARIABLE source_header
        ERROR_VARIABLE source_error)
    if(NOT source_result EQUAL 0)
        message(FATAL_ERROR
            "The source path now REFUSES the insertion-stem fixture (exit ${source_result}).\n"
            "CR-44 is registered as accepted, not fixed.\n${source_error}")
    endif()
    check_header("The source-path header" "${source_header}")
    if(NOT "${source_header}" STREQUAL "${descriptor_header}")
        message(FATAL_ERROR
            "The two input paths disagree on the insertion-stem fixture")
    endif()
endif()
