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

function(require_before variable_name first second description)
    string(FIND "${${variable_name}}" "${first}" first_position)
    string(FIND "${${variable_name}}" "${second}" second_position)
    if(first_position EQUAL -1 OR second_position EQUAL -1 OR
       NOT first_position LESS second_position)
        message(FATAL_ERROR
            "${description}: expected '${first}' before '${second}'")
    endif()
endfunction()

function(normalize_source_comment input_var output_var)
    string(REGEX REPLACE "// Source: [^\n]*\n"
        "// Source: <normalized>\n" normalized "${${input_var}}")
    set(${output_var} "${normalized}" PARENT_SCOPE)
endfunction()

set(enum_proto "${DATA_DIR}/enum_codegen.proto")
set(enum_pbs "${DATA_DIR}/enum_codegen.pbs")
set(open_enum_proto "${DATA_DIR}/open_enum.proto")
set(open_enum_pbs "${DATA_DIR}/open_enum.pbs")

run_ok(enum_out enum_err ${CODEGEN} --descriptor-set ${enum_pbs})
run_ok(open_enum_out open_enum_err ${CODEGEN} --descriptor-set ${open_enum_pbs})

require_contains(enum_out
    "enum Status : int32_t"
    "Fixed-width top-level enum declaration")
require_contains(enum_out
    "FAILED = -1"
    "Negative enum value")
require_contains(enum_out
    "MIN_VALUE = -2147483648"
    "Minimum int32 enum value")
require_contains(enum_out
    "STARTED = 1,\n    ACTIVE = 1"
    "Aliased enum values")
require_contains(enum_out
    "enum Priority : int32_t"
    "Fixed-width nested enum declaration")
require_before(enum_out
    "enum Status : int32_t" "struct Job"
    "Top-level enum declaration order")
require_before(enum_out
    "enum Priority" "Status status"
    "Nested enum declaration order")
require_before(enum_out
    "struct Job" "struct Audit"
    "Nested enum owner declaration order")

require_contains(enum_out
    "Status status = Status::STARTED;"
    "Singular enum field and default")
require_contains(enum_out
    "Job::Priority priority = Job::Priority::HIGH;"
    "Nested enum field and default")
require_contains(enum_out
    "std::vector<Status> history;"
    "Repeated enum field")
require_contains(enum_out
    "std::vector<Status> packed_history;"
    "Packed repeated enum field")
require_contains(enum_out
    "std::map<std::string,Status> names;"
    "Enum-valued map")
require_contains(enum_out
    "Status current = Status::UNKNOWN;"
    "Implicit top-level enum default")
require_contains(enum_out
    "NonZero value = NonZero::FIVE;"
    "Implicit non-zero enum default")
require_contains(enum_out
    "Top top = Top::X;"
    "Enum default protected from nested enumerator shadowing")
require_absent(enum_out
    "Top top = X;"
    "Unqualified shadowed enum default")
require_contains(enum_out
    "Job::Priority priority = Job::Priority::HIGH;"
    "Qualified forward nested enum default")
require_contains(enum_out
    "std::vector<Job::Priority> priorities;"
    "Forward repeated nested enum field")
require_contains(enum_out
    "std::map<std::string,Job::Priority> priorities_by_name;"
    "Forward nested enum map value")
require_absent(enum_out
    "int32_t status"
    "Obsolete singular enum representation")

require_contains(open_enum_out
    "enum OpenStatus : int32_t"
    "Fixed-width open proto3 enum declaration")
require_contains(open_enum_out
    "OpenStatus status = OpenStatus::OPEN_ZERO;"
    "Open proto3 enum implicit default")

require_contains(enum_out
    "pb.put_enum(1, x.status);"
    "Singular enum encoder")
require_contains(enum_out
    "pb.put_repeated_enum(3, x.history);"
    "Unpacked repeated enum encoder")
require_contains(enum_out
    "pb.put_packed_enum(4, x.packed_history);"
    "Packed repeated enum encoder")
require_contains(enum_out
    "pb.put_map_string_enum(5, x.names);"
    "Enum map encoder")
require_contains(enum_out
    "pb.get_enum(&x.status, &x.has_status);"
    "Singular enum decoder")
require_contains(enum_out
    "pb.get_repeated_enum(&x.history);"
    "Repeated enum decoder")
require_contains(enum_out
    "pb.get_map_string_enum(&x.names);"
    "Enum map decoder")

run_ok(no_defaults_out no_defaults_err
    ${CODEGEN} --descriptor-set --no-default-values ${enum_pbs})
require_contains(no_defaults_out
    "Status status = Status::UNKNOWN;"
    "Implicit enum default when explicit defaults are disabled")
require_contains(no_defaults_out
    "Job::Priority priority = Job::Priority::LOW;"
    "Implicit local nested enum default when explicit defaults are disabled")
require_contains(no_defaults_out
    "Job::Priority priority = Job::Priority::LOW;"
    "Implicit foreign nested enum default when explicit defaults are disabled")
require_absent(no_defaults_out
    "Status status = Status::STARTED;"
    "Disabled explicit top-level enum default")
require_absent(no_defaults_out
    "Job::Priority priority = Job::Priority::HIGH;"
    "Disabled explicit local nested enum default")

run_ok(no_class_out no_class_err
    ${CODEGEN} --descriptor-set --no-class ${enum_pbs})
require_absent(no_class_out
    "enum Status"
    "Top-level enum in --no-class output")
require_absent(no_class_out
    "enum Priority"
    "Nested enum in --no-class output")
require_absent(no_class_out
    "struct Job"
    "Message structure in --no-class output")
require_contains(no_class_out
    "const Job &x"
    "Enum message codecs in --no-class output")

if(FULL_BUILD)
    run_ok(proto_out proto_err ${CODEGEN} ${enum_proto})
    normalize_source_comment(enum_out pbs_normalized)
    normalize_source_comment(proto_out proto_normalized)
    if(NOT proto_normalized STREQUAL pbs_normalized)
        message(FATAL_ERROR
            "Enum .proto and .pbs generated outputs differ")
    endif()

    run_ok(open_proto_out open_proto_err ${CODEGEN} ${open_enum_proto})
    normalize_source_comment(open_enum_out open_pbs_normalized)
    normalize_source_comment(open_proto_out open_proto_normalized)
    if(NOT open_proto_normalized STREQUAL open_pbs_normalized)
        message(FATAL_ERROR
            "Open enum .proto and .pbs generated outputs differ")
    endif()
endif()
