# R09 (U-44) regression guard.
#
# `OptionFieldKind` must not regain the write-only `repeated` member that R09
# deleted. The first guard was a C++ `static_assert` built on a
# `decltype((void)T::repeated)` partial specialization; GCC 4.7.4 does not
# SFINAE that specialization out, so the assertion failed on a member that no
# longer existed in the full-matrix job (run 40600916122, job 122528516148,
# "GCC 4.7.4 / C++11", tests/codegen/parser/test_parser.cpp:52). There is no
# plain compile-time member-absence check, and a second SFINAE form would make
# the same compiler-version bet, so this guard reads the declaration instead.
# It runs as `parser.unit.option_field_kind` in the same CTest suite as the
# behavioural parser tests, which are unchanged.

if(NOT DEFINED HEADER)
    message(FATAL_ERROR "HEADER is required")
endif()
if(NOT EXISTS "${HEADER}")
    message(FATAL_ERROR "OptionFieldKind header not found: ${HEADER}")
endif()

file(READ "${HEADER}" header)

string(FIND "${header}" "struct OptionFieldKind" struct_start)
if(struct_start EQUAL -1)
    message(FATAL_ERROR
        "${HEADER} no longer declares 'struct OptionFieldKind'; update "
        "parser.unit.option_field_kind")
endif()

# The declaration runs from the struct keyword to the first `};` after it.
# Strip line comments first, so prose that names the removed flag cannot trip
# the guard.
string(SUBSTRING "${header}" ${struct_start} -1 declaration)
string(REGEX REPLACE "//[^\n]*" "" declaration "${declaration}")
string(FIND "${declaration}" "};" struct_end)
if(struct_end EQUAL -1)
    message(FATAL_ERROR "unterminated 'struct OptionFieldKind' in ${HEADER}")
endif()
string(SUBSTRING "${declaration}" 0 ${struct_end} declaration)

# No type check: the flag was a bool, but any write-only member of this name is
# the same defect.
string(FIND "${declaration}" "repeated" repeated_member)
if(NOT repeated_member EQUAL -1)
    message(FATAL_ERROR
        "OptionFieldKind declares a 'repeated' member again. R09 (U-44) "
        "removed it because the parser wrote it and no rule read it; a "
        "write-only flag must not return. The compile-time detector that used "
        "to catch this failed the GCC 4.7 full matrix (run 40600916122, job "
        "122528516148) and was replaced by this textual check.")
endif()

message(STATUS "OptionFieldKind has no 'repeated' member")
