if(NOT DEFINED CODEGEN)
    message(FATAL_ERROR "CODEGEN is required")
endif()
if(NOT DEFINED DATA_DIR)
    message(FATAL_ERROR "DATA_DIR is required")
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
        message(FATAL_ERROR "${description}: found obsolete '${forbidden}'")
    endif()
endfunction()

function(normalize_source_comment input_var output_var)
    string(REGEX REPLACE "// Source: [^\n]*\n"
        "// Source: <normalized>\n" normalized "${${input_var}}")
    set(${output_var} "${normalized}" PARENT_SCOPE)
endfunction()

set(scalar_proto "${DATA_DIR}/scalar_maps.proto")
set(scalar_pbs "${DATA_DIR}/scalar_maps.pbs")
set(enum_proto "${DATA_DIR}/enum_map.proto")
set(enum_pbs "${DATA_DIR}/enum_map.pbs")
set(message_proto "${DATA_DIR}/message_map.proto")
set(message_pbs "${DATA_DIR}/message_map.pbs")
set(malformed_pbs "${DATA_DIR}/malformed_map.pbs")

run_ok(scalar_out scalar_err ${CODEGEN} --descriptor-set ${scalar_pbs})
require_contains(scalar_out
    "std::map<std::string,int32_t> counts;"
    "Scalar map C++ type")
require_contains(scalar_out
    "std::map<int64_t,std::string> payloads;"
    "Bytes map C++ type")
require_contains(scalar_out
    "pb.put_map_string_int32(1, x.counts);"
    "Scalar map encoder")
require_contains(scalar_out
    "pb.get_map_string_int32(&x.counts);"
    "Scalar map decoder")
require_contains(scalar_out
    "pb.put_packed_int32(3, x.samples);"
    "Packed repeated-field encoder")
require_contains(scalar_out
    "inline void encode(easypb::Encoder &pb, const ScalarMaps &x)"
    "Scalar map ADL encoder")
require_contains(scalar_out
    "inline void decode(easypb::Decoder pb, ScalarMaps &x)"
    "Scalar map ADL decoder")
require_absent(scalar_out
    "void encode(easypb::Encoder &pb) const;"
    "Scalar map generated code")

run_ok(enum_out enum_err ${CODEGEN} --descriptor-set ${enum_pbs})
require_contains(enum_out
    "std::map<std::string,int32_t> statuses;"
    "Enum map C++ type")
require_contains(enum_out
    "pb.put_map_string_enum(1, x.statuses);"
    "Enum map encoder")
require_contains(enum_out
    "pb.get_map_string_enum(&x.statuses);"
    "Enum map decoder")
require_contains(enum_out
    "inline void encode(easypb::Encoder &pb, const EnumMap &x)"
    "Enum map ADL encoder")
require_contains(enum_out
    "inline void decode(easypb::Decoder pb, EnumMap &x)"
    "Enum map ADL decoder")
require_absent(enum_out
    "void encode(easypb::Encoder &pb) const;"
    "Enum map generated code")

run_ok(custom_out custom_err
    ${CODEGEN} --descriptor-set --map-type=custom_map ${scalar_pbs})
require_contains(custom_out
    "custom_map<std::string,int32_t> counts;"
    "Custom scalar map container")
require_contains(custom_out
    "custom_map<int64_t,std::string> payloads;"
    "Custom bytes map container")

run_ok(message_out message_err ${CODEGEN} --descriptor-set ${message_pbs})
require_contains(message_out
    "std::map<std::string,Item> items;"
    "Message map C++ type")
require_contains(message_out
    "pb.put_map_string_message(1, x.items);"
    "Message map encoder")
require_contains(message_out
    "pb.get_map_string_message(&x.items);"
    "Message map decoder")

run_fail(malformed_out malformed_err
    ${CODEGEN} --descriptor-set ${malformed_pbs})
require_contains(malformed_err
    "Malformed map entry .easypb.test.BrokenMap.ItemsEntry: fields 1 and 2 are required"
    "Malformed map-entry rejection")

if(FULL_BUILD)
    run_ok(scalar_proto_out scalar_proto_err ${CODEGEN} ${scalar_proto})
    normalize_source_comment(scalar_out scalar_pbs_normalized)
    normalize_source_comment(scalar_proto_out scalar_proto_normalized)
    if(NOT scalar_proto_normalized STREQUAL scalar_pbs_normalized)
        message(FATAL_ERROR
            "Scalar map .proto and .pbs generated outputs differ")
    endif()

    run_ok(enum_proto_out enum_proto_err ${CODEGEN} ${enum_proto})
    normalize_source_comment(enum_out enum_pbs_normalized)
    normalize_source_comment(enum_proto_out enum_proto_normalized)
    if(NOT enum_proto_normalized STREQUAL enum_pbs_normalized)
        message(FATAL_ERROR
            "Enum map .proto and .pbs generated outputs differ")
    endif()

    run_ok(message_proto_out message_proto_err ${CODEGEN} ${message_proto})
    normalize_source_comment(message_out message_pbs_normalized)
    normalize_source_comment(message_proto_out message_proto_normalized)
    if(NOT message_proto_normalized STREQUAL message_pbs_normalized)
        message(FATAL_ERROR
            "Message map .proto and .pbs generated outputs differ")
    endif()
endif()
