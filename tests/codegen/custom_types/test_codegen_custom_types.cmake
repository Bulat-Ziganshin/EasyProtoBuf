if(NOT DEFINED CODEGEN)
    message(FATAL_ERROR "CODEGEN is required")
endif()
if(NOT DEFINED DATA_DIR)
    message(FATAL_ERROR "DATA_DIR is required")
endif()
if(NOT DEFINED OPTIONS_PROTO)
    message(FATAL_ERROR "OPTIONS_PROTO is required")
endif()
if(NOT DEFINED DESCRIPTOR_HPP)
    message(FATAL_ERROR "DESCRIPTOR_HPP is required")
endif()
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
        message(FATAL_ERROR "${description}: missing '${expected}'")
    endif()
endfunction()

function(require_absent variable_name forbidden description)
    string(FIND "${${variable_name}}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "${description}: found '${forbidden}'")
    endif()
endfunction()

function(normalize_source_comment input_var output_var)
    string(REGEX REPLACE "// Source: [^\n]*\n"
        "// Source: <normalized>\n" normalized "${${input_var}}")
    set(${output_var} "${normalized}" PARENT_SCOPE)
endfunction()

set(proto "${DATA_DIR}/custom_types.proto")
set(pbs "${DATA_DIR}/custom_types.pbs")
set(no_type_cpp_pbs "${DATA_DIR}/cpp_group_without_type.pbs")
set(empty_cpp_type_pbs "${DATA_DIR}/empty_cpp_type.pbs")
set(foreign_51000_pbs "${DATA_DIR}/foreign_51000.pbs")
set(template_cases_proto "${DATA_DIR}/template_cases.proto")
set(template_cases_pbs "${DATA_DIR}/template_cases.pbs")

# options.proto and the trimmed descriptor decoder must use exactly the same
# extension number.  This guard also makes the future switch to an assigned
# public number a single, mechanically checked change.
file(READ "${OPTIONS_PROTO}" options_proto_text)
string(REGEX MATCH "CppFieldOptions[ \t]+cpp[ \t]*=[ \t]*([0-9]+)"
    options_proto_match "${options_proto_text}")
set(options_proto_number "${CMAKE_MATCH_1}")
file(READ "${DESCRIPTOR_HPP}" descriptor_hpp_text)
string(REGEX MATCH "EASYPB_CPP_FIELD_OPTIONS_NUMBER[ \t]*=[ \t]*([0-9]+)"
    descriptor_hpp_match "${descriptor_hpp_text}")
set(descriptor_hpp_number "${CMAKE_MATCH_1}")
if(options_proto_number STREQUAL "" OR descriptor_hpp_number STREQUAL "")
    message(FATAL_ERROR "Could not extract EasyProtoBuf C++ field-option extension numbers")
endif()
if(NOT options_proto_number STREQUAL descriptor_hpp_number)
    message(FATAL_ERROR
        "EasyProtoBuf C++ field-option extension mismatch: options.proto=${options_proto_number}, descriptor.pb.hpp=${descriptor_hpp_number}")
endif()

run_ok(default_out default_err ${CODEGEN} --descriptor-set ${pbs})
require_contains(default_out "::std::string ordinary_name;" "Default string type")
require_contains(default_out "::std::vector< ::std::string> ordinary_aliases;" "Default repeated type")
require_contains(default_out "::std::map< ::std::string,::easypb::test::CustomTypes::Item> ordinary_items;" "Default map type")
require_contains(default_out "FixedString<64> custom_name;" "Literal singular override")
require_contains(default_out "MessageBox< ::easypb::test::CustomTypes::Item> item;" "Singular message placeholder")
require_contains(default_out "SmallVector< ::std::string, 4> aliases;" "Repeated string component substitution")
require_contains(default_out "std::deque< ::easypb::test::CustomTypes::Item> items;" "Repeated message override")
require_contains(default_out "std::unordered_map< ::std::string, ::easypb::test::CustomTypes::Item> items_by_name;" "Map override")
require_contains(default_out "WrappedInt scalar = 0;" "Literal scalar override")
require_contains(default_out "EnumBox< ::easypb::test::CustomTypes::Kind> kind = ::easypb::test::CustomTypes::Kind::ZERO;" "Enum placeholder")
require_contains(default_out "OpaqueRepeated opaque_values;" "Per-field repeated literal is not abbreviated")
require_contains(default_out "OpaqueMap opaque_map;" "Per-field map literal is not abbreviated")
require_absent(default_out "OpaqueRepeated< ::int32_t>" "Per-field repeated abbreviation")
require_absent(default_out "OpaqueMap< ::std::string" "Per-field map abbreviation")

require_contains(default_out "pb.put_string(4, x.custom_name);" "String codec remains protobuf-driven")
require_contains(default_out "pb.put_message(5, x.item);" "Message codec remains protobuf-driven")
require_contains(default_out "pb.put_repeated_string(6, x.aliases);" "Repeated codec remains protobuf-driven")
require_contains(default_out "pb.put_map_string_message(8, x.items_by_name);" "Map codec remains protobuf-driven")
require_contains(default_out "pb.put_int32(9, x.scalar);" "Scalar codec remains protobuf-driven")
require_contains(default_out "pb.put_enum(10, x.kind);" "Enum codec remains protobuf-driven")

run_ok(global_out global_err ${CODEGEN} --descriptor-set
    --string-type MyString
    --repeated-type GlobalVec
    --map-type GlobalMap
    ${pbs})
require_contains(global_out "MyString ordinary_name;" "Global string override")
require_contains(global_out "GlobalVec<MyString> ordinary_aliases;" "Global repeated abbreviation")
require_contains(global_out "GlobalMap<MyString,::easypb::test::CustomTypes::Item> ordinary_items;" "Global map abbreviation")
require_contains(global_out "FixedString<64> custom_name;" "Per-field singular overrides global string")
require_contains(global_out "SmallVector<MyString, 4> aliases;" "Per-field repeated keeps global string component")
require_contains(global_out "std::unordered_map<MyString, ::easypb::test::CustomTypes::Item> items_by_name;" "Per-field map keeps global string component")
require_contains(global_out "OpaqueRepeated opaque_values;" "Per-field repeated literal stays literal with globals")
require_contains(global_out "OpaqueMap opaque_map;" "Per-field map literal stays literal with globals")

# The grouped cpp extension may be present without its type member. This must
# not count as a per-field type override, including when the group contains
# fields from a future schema version that this decoder does not know yet.
run_ok(no_type_cpp_out no_type_cpp_err ${CODEGEN} --descriptor-set ${no_type_cpp_pbs})
require_contains(no_type_cpp_out
    "::std::vector< ::std::string> empty_cpp;"
    "Empty cpp group does not override the repeated field type")
require_contains(no_type_cpp_out
    "::std::vector< ::std::string> future_cpp;"
    "Cpp group without type remains non-overriding when unknown members are present")

run_ok(no_type_cpp_global_out no_type_cpp_global_err ${CODEGEN} --descriptor-set
    --string-type MyString
    --repeated-type GlobalVec
    ${no_type_cpp_pbs})
require_contains(no_type_cpp_global_out
    "GlobalVec<MyString> empty_cpp;"
    "Empty cpp group preserves global string/repeated customization")
require_contains(no_type_cpp_global_out
    "GlobalVec<MyString> future_cpp;"
    "Cpp group without type preserves global customization with unknown members")

# An explicitly present but empty type in a descriptor retains proto2 presence
# and must be rejected, just like an empty direct-source option.
run_fail(empty_cpp_pbs_out empty_cpp_pbs_err
    ${CODEGEN} --descriptor-set ${empty_cpp_type_pbs})
if(NOT empty_cpp_pbs_err MATCHES "must not be empty")
    message(FATAL_ERROR "Empty descriptor C++ type diagnostic is unclear: ${empty_cpp_pbs_err}")
endif()

# 51000 is intentionally occupied by a foreign FieldOptions extension in the
# parser differential corpus.  It must remain unrelated to EasyProtoBuf after
# changing our temporary extension number.
run_ok(foreign_51000_out foreign_51000_err
    ${CODEGEN} --descriptor-set ${foreign_51000_pbs})
require_contains(foreign_51000_out
    "::std::string value;"
    "Foreign FieldOptions extension 51000 remains ignored")

# Valid numbered/reordered placeholders, bytes/string layering, and a
# scalar/scalar map all use the same shared template expander.
run_ok(template_cases_out template_cases_err
    ${CODEGEN} --descriptor-set ${template_cases_pbs})
require_contains(template_cases_out
    "BytesBox< ::std::string> data;"
    "Bytes field placeholder uses the default string component type")
require_contains(template_cases_out
    "NumberedVec< ::int32_t> values;"
    "Repeated field accepts explicit {0}")
require_contains(template_cases_out
    "ReorderedMap< ::std::string, ::int32_t> swapped;"
    "Map field accepts reordered {1}, {0} placeholders")

run_ok(template_cases_global_out template_cases_global_err ${CODEGEN} --descriptor-set
    --string-type MyString
    ${template_cases_pbs})
require_contains(template_cases_global_out
    "BytesBox<MyString> data;"
    "Bytes per-field template preserves global string customization")
require_contains(template_cases_global_out
    "ReorderedMap<MyString, ::int32_t> swapped;"
    "Scalar/string map placeholders preserve global string customization")

if(FULL_BUILD)
    run_ok(source_out source_err ${CODEGEN} ${proto})
    normalize_source_comment(default_out pbs_normalized)
    normalize_source_comment(source_out source_normalized)
    if(NOT source_normalized STREQUAL pbs_normalized)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/custom-types-source.hpp" "${source_normalized}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/custom-types-pbs.hpp" "${pbs_normalized}")
        message(FATAL_ERROR ".proto and .pbs custom-type outputs differ")
    endif()

    # template_cases.proto deliberately uses the valid fully-qualified
    # `(.easypb.cpp).type` spelling for one field.  It must normalize to the
    # same descriptor semantics as `(easypb.cpp).type`.
    run_ok(template_source_out template_source_err ${CODEGEN} ${template_cases_proto})
    normalize_source_comment(template_source_out template_source_normalized)
    normalize_source_comment(template_cases_out template_pbs_normalized)
    if(NOT template_source_normalized STREQUAL template_pbs_normalized)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/template-cases-source.hpp" "${template_source_normalized}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/template-cases-pbs.hpp" "${template_pbs_normalized}")
        message(FATAL_ERROR ".proto and .pbs numbered/leading-dot custom-type outputs differ")
    endif()

    set(tmp_dir "${CMAKE_CURRENT_BINARY_DIR}/custom-type-invalid")
    file(MAKE_DIRECTORY "${tmp_dir}")

    file(WRITE "${tmp_dir}/duplicate.proto"
        "syntax=\"proto3\"; message M { int32 x=1 [(easypb.cpp).type=\"A\", (easypb.cpp).type=\"B\"]; }\n")
    run_fail(duplicate_out duplicate_err ${CODEGEN} "${tmp_dir}/duplicate.proto")
    if(NOT duplicate_err MATCHES "duplicate EasyProtoBuf C\\+\\+ type option")
        message(FATAL_ERROR "Duplicate custom type diagnostic is unclear: ${duplicate_err}")
    endif()

    file(WRITE "${tmp_dir}/non-string.proto"
        "syntax=\"proto3\"; message M { int32 x=1 [(easypb.cpp).type=123]; }\n")
    run_fail(non_string_out non_string_err ${CODEGEN} "${tmp_dir}/non-string.proto")
    if(NOT non_string_err MATCHES "must be a string literal")
        message(FATAL_ERROR "Non-string custom type diagnostic is unclear: ${non_string_err}")
    endif()

    file(WRITE "${tmp_dir}/empty.proto"
        "syntax=\"proto3\"; message M { int32 x=1 [(easypb.cpp).type=\"\"]; }\n")
    run_fail(empty_out empty_err ${CODEGEN} "${tmp_dir}/empty.proto")
    if(NOT empty_err MATCHES "must not be empty")
        message(FATAL_ERROR "Empty custom type diagnostic is unclear: ${empty_err}")
    endif()

    file(WRITE "${tmp_dir}/unknown.proto"
        "syntax=\"proto3\"; message M { int32 x=1 [(easypb.cpp).typo=\"A\"]; }\n")
    run_fail(unknown_out unknown_err ${CODEGEN} "${tmp_dir}/unknown.proto")
    if(NOT unknown_err MATCHES "unknown EasyProtoBuf C\\+\\+ field option")
        message(FATAL_ERROR "Unknown EasyProtoBuf sub-option diagnostic is unclear: ${unknown_err}")
    endif()

    file(WRITE "${tmp_dir}/unknown-leading-dot.proto"
        "syntax=\"proto3\"; message M { int32 x=1 [(.easypb.cpp).typo=\"A\"]; }\n")
    run_fail(unknown_dot_out unknown_dot_err ${CODEGEN} "${tmp_dir}/unknown-leading-dot.proto")
    if(NOT unknown_dot_err MATCHES "unknown EasyProtoBuf C\\+\\+ field option")
        message(FATAL_ERROR "Leading-dot EasyProtoBuf sub-option diagnostic is unclear: ${unknown_dot_err}")
    endif()

    file(WRITE "${tmp_dir}/aggregate.proto"
        "syntax=\"proto3\"; message M { int32 x=1 [(easypb.cpp)={ type: \"AggregateWrapped<{}>\" }]; }\n")
    run_fail(aggregate_out aggregate_err ${CODEGEN} "${tmp_dir}/aggregate.proto")
    string(FIND "${aggregate_err}" "use (easypb.cpp).type" aggregate_hint_pos)
    if(aggregate_hint_pos EQUAL -1)
        message(FATAL_ERROR "Grouped EasyProtoBuf option diagnostic is unclear: ${aggregate_err}")
    endif()

    file(WRITE "${tmp_dir}/other.proto"
        "syntax=\"proto3\"; message M { int32 x=1 [(other.pkg).type=\"A\"]; }\n")
    run_ok(other_out other_err ${CODEGEN} "${tmp_dir}/other.proto")
    require_contains(other_out "::int32_t x = 0;" "Unrelated custom option remains ignored")

    # Per-field and global templates intentionally share myformat(), including
    # the same out-of-range placeholder failure.
    file(WRITE "${tmp_dir}/bad-field-placeholder.proto"
        "syntax=\"proto3\"; message M { repeated int32 x=1 [(easypb.cpp).type=\"Bad<{1}>\"]; }\n")
    run_fail(field_placeholder_out field_placeholder_err
        ${CODEGEN} "${tmp_dir}/bad-field-placeholder.proto")
    if(NOT field_placeholder_err MATCHES "Not enough arguments for myformat")
        message(FATAL_ERROR "Per-field placeholder diagnostic is unexpected: ${field_placeholder_err}")
    endif()

    file(WRITE "${tmp_dir}/bad-global-placeholder.proto"
        "syntax=\"proto3\"; message M { repeated int32 x=1; }\n")
    run_fail(global_placeholder_out global_placeholder_err
        ${CODEGEN} --repeated-type "Bad<{1}>" "${tmp_dir}/bad-global-placeholder.proto")
    if(NOT global_placeholder_err MATCHES "Not enough arguments for myformat")
        message(FATAL_ERROR "Global placeholder diagnostic is unexpected: ${global_placeholder_err}")
    endif()

    # A custom C++ spelling must not change recursive-message dependency
    # semantics. Singular recursion is still rejected, and repeated recursion
    # still requires the existing opt-in flag.
    file(WRITE "${tmp_dir}/custom-singular-recursive.proto"
        "syntax=\"proto3\"; message Node { Node next=1 [(easypb.cpp).type=\"Ptr<{}>\"]; }\n")
    run_fail(singular_recursive_out singular_recursive_err
        ${CODEGEN} "${tmp_dir}/custom-singular-recursive.proto")
    if(NOT singular_recursive_err MATCHES "recursive message value dependency")
        message(FATAL_ERROR "Custom singular recursion unexpectedly changed dependency semantics: ${singular_recursive_err}")
    endif()

    file(WRITE "${tmp_dir}/custom-repeated-recursive.proto"
        "syntax=\"proto3\"; message Node { repeated Node children=1 [(easypb.cpp).type=\"RecursiveVec<{}>\"]; }\n")
    run_fail(repeated_recursive_out repeated_recursive_err
        ${CODEGEN} "${tmp_dir}/custom-repeated-recursive.proto")
    if(NOT repeated_recursive_err MATCHES "recursive message value dependency")
        message(FATAL_ERROR "Custom repeated recursion was accepted without opt-in: ${repeated_recursive_err}")
    endif()
    run_ok(repeated_recursive_allowed repeated_recursive_allowed_err
        ${CODEGEN} --allow-self-recursive-containers "${tmp_dir}/custom-repeated-recursive.proto")
    require_contains(repeated_recursive_allowed
        "RecursiveVec< ::Node> children;"
        "Custom repeated recursion with explicit opt-in")
endif()
