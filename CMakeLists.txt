cmake_minimum_required(VERSION 3.10)

project(EasyProtoBuf
        VERSION 0.0
        DESCRIPTION "Single-header C++11 ProtoBuf library"
        LANGUAGES CXX)

include(CTest)


# EASYPB_CXX_FLAGS is used by CI, and may also be supplied by users,
# to select an exact language mode and add compiler options.
# When it is set, CMake does not add its own C++ standard option.
set(EASYPB_CXX_FLAGS "" CACHE STRING
    "Extra C++ compiler flags; overrides automatic C++ standard selection")
option(EASYPB_CODEGEN_WITH_PROTO_PARSER
       "Build .proto parser, descriptor printer and parser benchmark into codegen"
       ON)
option(EASYPB_BUILD_DIFFERENTIAL_TESTS
       "Build the optional parser comparator used with an external protoc"
       OFF)

if(EASYPB_CXX_FLAGS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${EASYPB_CXX_FLAGS}")
    message(STATUS "EasyProtoBuf: using explicit C++ flags: ${EASYPB_CXX_FLAGS}")
else()
    # Prefer C++17, whose std::string_view may be used by the library and
    # code generator. Fall back to the project's required minimum, C++11.
    if("cxx_std_17" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        set(CMAKE_CXX_STANDARD 17)
    elseif("cxx_std_11" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        set(CMAKE_CXX_STANDARD 11)
    else()
        message(FATAL_ERROR
                "EasyProtoBuf requires a compiler with C++11 support")
    endif()

    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    message(STATUS "EasyProtoBuf: using C++${CMAKE_CXX_STANDARD}")
endif()


add_executable(decoder examples/decoder/decoder.cpp)
target_include_directories(decoder PRIVATE include)

add_executable(tutorial examples/tutorial/main.cpp)
target_include_directories(tutorial PRIVATE include)

if(EASYPB_CODEGEN_WITH_PROTO_PARSER)
    add_library(easypb_proto_parser STATIC codegen/parser/proto_parser.cpp)
    target_include_directories(easypb_proto_parser
        PUBLIC include codegen codegen/parser)
endif()

# "codegen" became a reserved target name in CMake 3.31. Keep the produced
# executable name while using a non-reserved internal target name.
add_executable(easypb_codegen codegen/main.cpp)
set_target_properties(easypb_codegen PROPERTIES OUTPUT_NAME codegen)
target_include_directories(easypb_codegen PRIVATE include 3rd-party/popl codegen)
if(EASYPB_CODEGEN_WITH_PROTO_PARSER)
    target_sources(easypb_codegen PRIVATE
        codegen/parser/pretty_printer.cpp
        codegen/parser/parser_benchmark.cpp)
    target_link_libraries(easypb_codegen PRIVATE easypb_proto_parser)
    target_compile_definitions(easypb_codegen PRIVATE
        EASYPB_CODEGEN_WITH_PROTO_PARSER=1)
else()
    target_compile_definitions(easypb_codegen PRIVATE
        EASYPB_CODEGEN_WITH_PROTO_PARSER=0)
endif()

if(BUILD_TESTING)
    add_test(NAME compare.decoded.data.with.original
             COMMAND tutorial)

    add_executable(generic_api_test tests/generic_api.cpp)
    target_include_directories(generic_api_test PRIVATE include)
    add_test(NAME generic.api COMMAND generic_api_test)

    add_executable(generic_scalar_routing_tests
        tests/runtime/generic_scalar_routing.cpp)
    target_include_directories(generic_scalar_routing_tests PRIVATE include)
    add_test(NAME generic.scalar.routing COMMAND generic_scalar_routing_tests)

    add_executable(generic_map_routing_tests
        tests/runtime/generic_map_routing.cpp)
    target_include_directories(generic_map_routing_tests PRIVATE include)
    add_test(NAME generic.map.routing COMMAND generic_map_routing_tests)

    set(generic_compile_fail_source
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/generic_api.cpp)
    set(generic_compile_fail_runner
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/run_compile_fail.cmake)

    function(easypb_add_compile_fail_test case_name case_definition diagnostic)
        set(target_name generic_compile_fail_${case_name})
        add_executable(${target_name} EXCLUDE_FROM_ALL
            ${generic_compile_fail_source})
        target_include_directories(${target_name} PRIVATE include)
        target_compile_definitions(${target_name} PRIVATE ${case_definition})
        add_test(NAME generic.compile_fail.${case_name}
            COMMAND ${CMAKE_COMMAND}
                -DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DTARGET_NAME=${target_name}
                -DBUILD_CONFIG=$<CONFIG>
                "-DEXPECTED_DIAGNOSTIC=${diagnostic}"
                -P ${generic_compile_fail_runner})
        set_tests_properties(generic.compile_fail.${case_name}
            PROPERTIES RUN_SERIAL TRUE)
    endfunction()

    easypb_add_compile_fail_test(
        map_float_put EASYPB_CASE_MAP_FLOAT_PUT
        "Invalid protobuf map key type")
    easypb_add_compile_fail_test(
        map_double_put EASYPB_CASE_MAP_DOUBLE_PUT
        "Invalid protobuf map key type")
    easypb_add_compile_fail_test(
        map_bytes_put EASYPB_CASE_MAP_BYTES_PUT
        "Invalid protobuf map key type")
    easypb_add_compile_fail_test(
        map_enum_put EASYPB_CASE_MAP_ENUM_PUT
        "Invalid protobuf map key type")
    easypb_add_compile_fail_test(
        map_message_put EASYPB_CASE_MAP_MESSAGE_PUT
        "Invalid protobuf map key type")
    easypb_add_compile_fail_test(
        map_float_get EASYPB_CASE_MAP_FLOAT_GET
        "Invalid protobuf map key type")
    easypb_add_compile_fail_test(
        map_double_get EASYPB_CASE_MAP_DOUBLE_GET
        "Invalid protobuf map key type")
    easypb_add_compile_fail_test(
        map_bytes_get EASYPB_CASE_MAP_BYTES_GET
        "Invalid protobuf map key type")
    easypb_add_compile_fail_test(
        map_enum_get EASYPB_CASE_MAP_ENUM_GET
        "Invalid protobuf map key type")
    easypb_add_compile_fail_test(
        map_message_get EASYPB_CASE_MAP_MESSAGE_GET
        "Invalid protobuf map key type")
    easypb_add_compile_fail_test(
        packed_string EASYPB_CASE_PACKED_STRING
        "This protobuf type cannot be packed")
    easypb_add_compile_fail_test(
        packed_bytes EASYPB_CASE_PACKED_BYTES
        "This protobuf type cannot be packed")
    easypb_add_compile_fail_test(
        packed_message EASYPB_CASE_PACKED_MESSAGE
        "PB_MESSAGE cannot be packed")
    easypb_add_compile_fail_test(
        get_message_value EASYPB_CASE_GET_MESSAGE_VALUE
        "PB_MESSAGE requires an output object")

    add_executable(wire_type_conversion_tests
        tests/runtime/wire_type_conversion.cpp)
    target_include_directories(wire_type_conversion_tests PRIVATE include)
    add_test(NAME wire.type.conversions COMMAND wire_type_conversion_tests)

    add_executable(skip_unknown_groups_tests
        tests/runtime/skip_unknown_groups.cpp)
    target_include_directories(skip_unknown_groups_tests PRIVATE include)
    add_test(NAME wire.skip.unknown.groups COMMAND skip_unknown_groups_tests)

    add_executable(map_entry_semantics_tests
        tests/runtime/map_entry_semantics.cpp)
    target_include_directories(map_entry_semantics_tests PRIVATE include)
    add_test(NAME wire.map.entry.semantics COMMAND map_entry_semantics_tests)

    set(codegen_maps_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen/maps)
    set(codegen_maps_data_dir ${codegen_maps_dir}/data)

    add_test(NAME codegen.maps
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${codegen_maps_data_dir}
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${codegen_maps_dir}/test_codegen_maps.cmake)

    set(codegen_maps_generated_dir
        ${CMAKE_CURRENT_BINARY_DIR}/generated/codegen-maps)
    set(codegen_maps_generated_header
        ${codegen_maps_generated_dir}/maps.generated.hpp)
    add_custom_command(
        OUTPUT ${codegen_maps_generated_header}
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DSCALAR_INPUT=${codegen_maps_data_dir}/scalar_maps.pbs
            -DENUM_INPUT=${codegen_maps_data_dir}/enum_map.pbs
            -DMESSAGE_INPUT=${codegen_maps_data_dir}/message_map.pbs
            -DOUTPUT=${codegen_maps_generated_header}
            -P ${codegen_maps_dir}/generate_runtime_header.cmake
        DEPENDS
            easypb_codegen
            ${codegen_maps_data_dir}/scalar_maps.pbs
            ${codegen_maps_data_dir}/enum_map.pbs
            ${codegen_maps_data_dir}/message_map.pbs
            ${codegen_maps_dir}/generate_runtime_header.cmake
        VERBATIM)
    add_custom_target(codegen_maps_generated
        DEPENDS ${codegen_maps_generated_header})

    add_executable(codegen_maps_runtime_test
        ${codegen_maps_dir}/map_runtime.cpp)
    add_dependencies(codegen_maps_runtime_test codegen_maps_generated)
    target_include_directories(codegen_maps_runtime_test PRIVATE
        include ${codegen_maps_generated_dir})
    add_test(NAME codegen.maps.runtime COMMAND codegen_maps_runtime_test)

    set(codegen_enums_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen/enums)
    set(codegen_enums_data_dir ${codegen_enums_dir}/data)

    add_test(NAME codegen.enums
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${codegen_enums_data_dir}
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${codegen_enums_dir}/test_codegen_enums.cmake)

    set(codegen_enums_generated_dir
        ${CMAKE_CURRENT_BINARY_DIR}/generated/codegen-enums)
    set(codegen_enums_generated_header
        ${codegen_enums_generated_dir}/enums.generated.hpp)
    set(codegen_open_enums_generated_header
        ${codegen_enums_generated_dir}/open-enums.generated.hpp)
    add_custom_command(
        OUTPUT ${codegen_enums_generated_header}
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DINPUT=${codegen_enums_data_dir}/enum_codegen.pbs
            -DOUTPUT=${codegen_enums_generated_header}
            -P ${codegen_enums_dir}/generate_runtime_header.cmake
        DEPENDS
            easypb_codegen
            ${codegen_enums_data_dir}/enum_codegen.pbs
            ${codegen_enums_dir}/generate_runtime_header.cmake
        VERBATIM)
    add_custom_command(
        OUTPUT ${codegen_open_enums_generated_header}
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DINPUT=${codegen_enums_data_dir}/open_enum.pbs
            -DOUTPUT=${codegen_open_enums_generated_header}
            -P ${codegen_enums_dir}/generate_runtime_header.cmake
        DEPENDS
            easypb_codegen
            ${codegen_enums_data_dir}/open_enum.pbs
            ${codegen_enums_dir}/generate_runtime_header.cmake
        VERBATIM)
    add_custom_target(codegen_enums_generated
        DEPENDS
            ${codegen_enums_generated_header}
            ${codegen_open_enums_generated_header})

    add_executable(codegen_enums_runtime_test
        ${codegen_enums_dir}/enum_runtime.cpp)
    add_dependencies(codegen_enums_runtime_test codegen_enums_generated)
    target_include_directories(codegen_enums_runtime_test PRIVATE
        include ${codegen_enums_generated_dir})
    add_test(NAME codegen.enums.runtime COMMAND codegen_enums_runtime_test)

    set(codegen_nested_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen/nested)
    set(codegen_nested_generated_dir
        ${CMAKE_CURRENT_BINARY_DIR}/generated/codegen-nested)
    set(codegen_nested_messages_header
        ${codegen_nested_generated_dir}/nested-messages.generated.hpp)
    set(codegen_nested_forward_header
        ${codegen_nested_generated_dir}/nested-forward.generated.hpp)
    add_custom_command(
        OUTPUT ${codegen_nested_messages_header} ${codegen_nested_forward_header}
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen/parser/data
            -DOUTPUT_DIR=${codegen_nested_generated_dir}
            -P ${codegen_nested_dir}/generate_headers.cmake
        DEPENDS
            easypb_codegen
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen/parser/data/nested-messages.pbs
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen/parser/data/nested-forward.pbs
            ${codegen_nested_dir}/generate_headers.cmake
        VERBATIM)
    add_custom_target(codegen_nested_generated
        DEPENDS ${codegen_nested_messages_header} ${codegen_nested_forward_header})

    add_executable(codegen_nested_runtime_test
        ${codegen_nested_dir}/nested_runtime.cpp)
    add_dependencies(codegen_nested_runtime_test codegen_nested_generated)
    target_include_directories(codegen_nested_runtime_test PRIVATE
        include ${codegen_nested_generated_dir})
    add_test(NAME codegen.nested.runtime COMMAND codegen_nested_runtime_test)

    add_test(NAME codegen.modes
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen/parser/data
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen/parser/test_codegen_modes.cmake)

    if(EASYPB_CODEGEN_WITH_PROTO_PARSER)
        add_executable(parser_tests tests/codegen/parser/test_parser.cpp)
        target_include_directories(parser_tests PRIVATE include codegen codegen/parser)
        target_link_libraries(parser_tests PRIVATE easypb_proto_parser)
        add_test(NAME parser.unit COMMAND parser_tests)
    endif()
endif()

if(EASYPB_BUILD_DIFFERENTIAL_TESTS)
    if(NOT EASYPB_CODEGEN_WITH_PROTO_PARSER)
        message(FATAL_ERROR
            "EASYPB_BUILD_DIFFERENTIAL_TESTS requires EASYPB_CODEGEN_WITH_PROTO_PARSER")
    endif()
    add_executable(parser_differential_compare
        tests/codegen/parser/differential/compare_with_protoc.cpp)
    target_include_directories(parser_differential_compare PRIVATE
        include codegen codegen/parser)
    target_link_libraries(parser_differential_compare PRIVATE easypb_proto_parser)
endif()
