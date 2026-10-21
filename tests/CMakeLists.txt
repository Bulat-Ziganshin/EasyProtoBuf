# Keep Windows test executable names free of installer-related substrings such
# as "install", "setup", "update", or "patch". Legacy 32-bit MinGW linkers do
# not embed an asInvoker manifest, so Windows installer detection may otherwise
# request elevation before CTest can start the executable. In particular, avoid
# "dispatch", which contains "patch".

set(easypb_test_include_dir ${PROJECT_SOURCE_DIR}/include)

if(BUILD_TESTING)
    add_test(NAME compare.decoded.data.with.original
             COMMAND tutorial)

    add_executable(generic_api_test generic_api.cpp)
    target_include_directories(generic_api_test PRIVATE
        ${easypb_test_include_dir})
    add_test(NAME generic.api COMMAND generic_api_test)

    add_executable(generic_scalar_routing_tests
        runtime/generic_scalar_routing.cpp)
    target_include_directories(generic_scalar_routing_tests PRIVATE
        ${easypb_test_include_dir})
    add_test(NAME generic.scalar.routing COMMAND generic_scalar_routing_tests)

    add_executable(generic_map_routing_tests
        runtime/generic_map_routing.cpp)
    target_include_directories(generic_map_routing_tests PRIVATE
        ${easypb_test_include_dir})
    add_test(NAME generic.map.routing COMMAND generic_map_routing_tests)

    set(generic_compile_fail_source
        ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/generic_api.cpp)
    set(generic_compile_fail_runner
        ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/run_compile_fail.cmake)

    function(easypb_add_compile_fail_test case_name case_definition diagnostic)
        set(target_name generic_compile_fail_${case_name})
        add_executable(${target_name} EXCLUDE_FROM_ALL
            ${generic_compile_fail_source})
        target_include_directories(${target_name} PRIVATE
            ${easypb_test_include_dir})
        target_compile_definitions(${target_name} PRIVATE ${case_definition})
        add_test(NAME generic.compile_fail.${case_name}
            COMMAND ${CMAKE_COMMAND}
                -DBUILD_DIR=${PROJECT_BINARY_DIR}
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
        runtime/wire_type_conversion.cpp)
    target_include_directories(wire_type_conversion_tests PRIVATE
        ${easypb_test_include_dir})
    add_test(NAME wire.type.conversions COMMAND wire_type_conversion_tests)

    add_executable(skip_unknown_groups_tests
        runtime/skip_unknown_groups.cpp)
    target_include_directories(skip_unknown_groups_tests PRIVATE
        ${easypb_test_include_dir})
    add_test(NAME wire.skip.unknown.groups COMMAND skip_unknown_groups_tests)

    add_executable(map_entry_semantics_tests
        runtime/map_entry_semantics.cpp)
    target_include_directories(map_entry_semantics_tests PRIVATE
        ${easypb_test_include_dir})
    add_test(NAME wire.map.entry.semantics COMMAND map_entry_semantics_tests)

    set(codegen_maps_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/maps)
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
    # The plan-5 helper publishes one header per target; the umbrella header
    # is the rule's OUTPUT and the three per-target headers are BYPRODUCTS.
    add_custom_command(
        OUTPUT ${codegen_maps_generated_header}
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DSCALAR_INPUT=${codegen_maps_data_dir}/scalar_maps.pbs
            -DENUM_INPUT=${codegen_maps_data_dir}/enum_map.pbs
            -DMESSAGE_INPUT=${codegen_maps_data_dir}/message_map.pbs
            -DOUTPUT=${codegen_maps_generated_header}
            -P ${codegen_maps_dir}/generate_runtime_header.cmake
        BYPRODUCTS
            ${codegen_maps_generated_dir}/scalar_maps.pb.hpp
            ${codegen_maps_generated_dir}/enum_map.pb.hpp
            ${codegen_maps_generated_dir}/message_map.pb.hpp
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
        ${easypb_test_include_dir} ${codegen_maps_generated_dir})
    add_test(NAME codegen.maps.runtime COMMAND codegen_maps_runtime_test)

    set(codegen_custom_types_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/custom_types)
    set(codegen_custom_types_data_dir ${codegen_custom_types_dir}/data)

    add_test(NAME codegen.custom_types
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${codegen_custom_types_data_dir}
            -DOPTIONS_PROTO=${PROJECT_SOURCE_DIR}/codegen/easypb/options.proto
            -DDESCRIPTOR_HPP=${PROJECT_SOURCE_DIR}/codegen/descriptor.pb.hpp
            -DDESCRIPTOR_ROOT=${codegen_custom_types_data_dir}/option-roots
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${codegen_custom_types_dir}/test_codegen_custom_types.cmake)

    set(codegen_custom_types_generated_dir
        ${CMAKE_CURRENT_BINARY_DIR}/generated/codegen-custom-types)
    set(codegen_custom_types_generated_header
        ${codegen_custom_types_generated_dir}/custom-types.generated.hpp)
    add_custom_command(
        OUTPUT ${codegen_custom_types_generated_header}
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DINPUT=${codegen_custom_types_data_dir}/custom_types.pbs
            -DOUTPUT=${codegen_custom_types_generated_header}
            -P ${codegen_custom_types_dir}/generate_runtime_header.cmake
        DEPENDS
            easypb_codegen
            ${codegen_custom_types_data_dir}/custom_types.pbs
            ${codegen_custom_types_dir}/generate_runtime_header.cmake
        VERBATIM)
    add_custom_target(codegen_custom_types_generated
        DEPENDS ${codegen_custom_types_generated_header})

    add_executable(codegen_custom_types_runtime_test
        ${codegen_custom_types_dir}/custom_types_runtime.cpp)
    add_dependencies(codegen_custom_types_runtime_test codegen_custom_types_generated)
    target_include_directories(codegen_custom_types_runtime_test PRIVATE
        ${easypb_test_include_dir} ${codegen_custom_types_generated_dir})
    add_test(NAME codegen.custom_types.runtime
        COMMAND codegen_custom_types_runtime_test)

    set(codegen_enums_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/enums)
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
        ${easypb_test_include_dir} ${codegen_enums_generated_dir})
    add_test(NAME codegen.enums.runtime COMMAND codegen_enums_runtime_test)

    # Plan 5 follow-up S1 (W-02, W-03, W-05, W-10, W-12): the emitter's value
    # and field-type layer. The lane generates every fixture through BOTH
    # input paths, compiles the generated headers with the tree's own
    # compiler, runs the runtime check of the default values, and drives the
    # refusals through the CLI. The descriptor path builds the headers the
    # runtime checker reads, because it is the one every build profile has;
    # the driver compares the source path's header with the descriptor path's
    # byte for byte.
    set(codegen_validity_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/validity)
    set(codegen_validity_data_dir ${codegen_validity_dir}/data)
    set(codegen_validity_generated_dir
        ${CMAKE_CURRENT_BINARY_DIR}/generated/codegen-validity)
    set(codegen_validity_descriptor_dir
        ${codegen_validity_generated_dir}/descriptor)
    set(codegen_validity_fixtures
        defaults-quote
        defaults-backslash
        defaults-newline
        defaults-nul
        defaults-quote-byte
        defaults-trigraph
        defaults-inf
        defaults-nan)

    foreach(name ${codegen_validity_fixtures})
        add_custom_command(
            OUTPUT ${codegen_validity_descriptor_dir}/${name}.pb.hpp
            COMMAND ${CMAKE_COMMAND}
                -DCODEGEN=$<TARGET_FILE:easypb_codegen>
                -DINPUT=${codegen_validity_data_dir}/${name}.pbs
                -DOUTPUT=${codegen_validity_descriptor_dir}/${name}.pb.hpp
                -P ${codegen_validity_dir}/generate_header.cmake
            DEPENDS
                easypb_codegen
                ${codegen_validity_data_dir}/${name}.pbs
                ${codegen_validity_dir}/generate_header.cmake
            VERBATIM)
    endforeach()
    add_custom_target(codegen_validity_generated
        DEPENDS
            ${codegen_validity_descriptor_dir}/defaults-quote.pb.hpp
            ${codegen_validity_descriptor_dir}/defaults-backslash.pb.hpp
            ${codegen_validity_descriptor_dir}/defaults-newline.pb.hpp
            ${codegen_validity_descriptor_dir}/defaults-nul.pb.hpp
            ${codegen_validity_descriptor_dir}/defaults-quote-byte.pb.hpp
            ${codegen_validity_descriptor_dir}/defaults-trigraph.pb.hpp
            ${codegen_validity_descriptor_dir}/defaults-inf.pb.hpp
            ${codegen_validity_descriptor_dir}/defaults-nan.pb.hpp)

    add_executable(codegen_validity_runtime_test
        ${codegen_validity_dir}/validity_runtime.cpp)
    add_dependencies(codegen_validity_runtime_test codegen_validity_generated)
    target_include_directories(codegen_validity_runtime_test PRIVATE
        ${easypb_test_include_dir} ${codegen_validity_descriptor_dir})

    # The tree's own standard: the C++11 profile passes its standard through
    # EASYPB_CXX_FLAGS, so CMAKE_CXX_STANDARD is empty there and the flags
    # carry it. The flags are split into a list, because the profile's
    # standard arrives as one string with a leading space, and a compiler
    # does not read " -std=c++11" as the option it is.
    if(CMAKE_CXX_STANDARD)
        set(codegen_validity_std_flag "-std=c++${CMAKE_CXX_STANDARD}")
    else()
        set(codegen_validity_std_flag "")
    endif()
    separate_arguments(codegen_validity_cxx_flags NATIVE_COMMAND
        "${CMAKE_CXX_FLAGS}")

    add_test(NAME codegen.validity
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${codegen_validity_data_dir}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/codegen-validity
            -DRUNTIME=$<TARGET_FILE:codegen_validity_runtime_test>
            -DCXX=${CMAKE_CXX_COMPILER}
            "-DCXX_FLAGS=${codegen_validity_cxx_flags}"
            -DCXX_STD_FLAG=${codegen_validity_std_flag}
            -DINCLUDE_DIR=${easypb_test_include_dir}
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${codegen_validity_dir}/test_validity.cmake)

    set(codegen_packages_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/packages)
    set(codegen_packages_data_dir ${codegen_packages_dir}/data)

    add_executable(codegen_cpp_names_test
        ${codegen_packages_dir}/cpp_names_test.cpp
        ${PROJECT_SOURCE_DIR}/codegen/cpp_names.cpp)
    target_include_directories(codegen_cpp_names_test PRIVATE
        ${easypb_test_include_dir}
        ${PROJECT_SOURCE_DIR}/codegen)
    add_test(NAME codegen.packages.naming COMMAND codegen_cpp_names_test)

    add_test(NAME codegen.packages
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${codegen_packages_data_dir}
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${codegen_packages_dir}/test_codegen_packages.cmake)

    set(codegen_packages_generated_dir
        ${CMAKE_CURRENT_BINARY_DIR}/generated/codegen-packages)
    set(codegen_packages_names_header
        ${codegen_packages_generated_dir}/names.generated.hpp)
    set(codegen_packages_alpha_header
        ${codegen_packages_generated_dir}/alpha.generated.hpp)
    set(codegen_packages_beta_header
        ${codegen_packages_generated_dir}/beta.generated.hpp)
    set(codegen_packages_external_header
        ${codegen_packages_generated_dir}/external.generated.hpp)
    set(codegen_packages_shadow_header
        ${codegen_packages_generated_dir}/shadow.generated.hpp)
    set(codegen_packages_module_header
        ${codegen_packages_generated_dir}/module-package.generated.hpp)
    set(codegen_packages_import_header
        ${codegen_packages_generated_dir}/import-package.generated.hpp)
    set(codegen_packages_no_class_header
        ${codegen_packages_generated_dir}/names.no-class.generated.hpp)
    add_custom_command(
        OUTPUT
            ${codegen_packages_names_header}
            ${codegen_packages_alpha_header}
            ${codegen_packages_beta_header}
            ${codegen_packages_external_header}
            ${codegen_packages_shadow_header}
            ${codegen_packages_module_header}
            ${codegen_packages_import_header}
            ${codegen_packages_no_class_header}
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${codegen_packages_data_dir}
            -DOUTPUT_DIR=${codegen_packages_generated_dir}
            -P ${codegen_packages_dir}/generate_headers.cmake
        DEPENDS
            easypb_codegen
            ${codegen_packages_data_dir}/names.pbs
            ${codegen_packages_data_dir}/alpha.pbs
            ${codegen_packages_data_dir}/beta.pbs
            ${codegen_packages_data_dir}/external.pbs
            ${codegen_packages_data_dir}/shadow.pbs
            ${codegen_packages_data_dir}/module-package.pbs
            ${codegen_packages_data_dir}/import-package.pbs
            ${codegen_packages_dir}/generate_headers.cmake
        VERBATIM)
    add_custom_target(codegen_packages_generated
        DEPENDS
            ${codegen_packages_names_header}
            ${codegen_packages_alpha_header}
            ${codegen_packages_beta_header}
            ${codegen_packages_external_header}
            ${codegen_packages_shadow_header}
            ${codegen_packages_module_header}
            ${codegen_packages_import_header}
            ${codegen_packages_no_class_header})

    add_executable(codegen_packages_runtime_test
        ${codegen_packages_dir}/package_runtime_main.cpp
        ${codegen_packages_dir}/package_runtime_helper.cpp)
    add_dependencies(codegen_packages_runtime_test codegen_packages_generated)
    target_include_directories(codegen_packages_runtime_test PRIVATE
        ${easypb_test_include_dir}
        ${codegen_packages_generated_dir}
        ${codegen_packages_dir})
    add_test(NAME codegen.packages.runtime COMMAND codegen_packages_runtime_test)

    add_executable(codegen_packages_no_class_runtime_test
        ${codegen_packages_dir}/no_class_runtime.cpp)
    add_dependencies(codegen_packages_no_class_runtime_test codegen_packages_generated)
    target_include_directories(codegen_packages_no_class_runtime_test PRIVATE
        ${easypb_test_include_dir} ${codegen_packages_generated_dir})
    add_test(NAME codegen.packages.no_class.runtime
        COMMAND codegen_packages_no_class_runtime_test)

    add_executable(codegen_packages_external_runtime_test
        ${codegen_packages_dir}/external_runtime.cpp)
    add_dependencies(codegen_packages_external_runtime_test codegen_packages_generated)
    target_include_directories(codegen_packages_external_runtime_test PRIVATE
        ${easypb_test_include_dir} ${codegen_packages_generated_dir})
    add_test(NAME codegen.packages.external.runtime
        COMMAND codegen_packages_external_runtime_test)

    set(codegen_nested_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/nested)
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
            -DDATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/codegen/parser/data
            -DOUTPUT_DIR=${codegen_nested_generated_dir}
            -P ${codegen_nested_dir}/generate_headers.cmake
        DEPENDS
            easypb_codegen
            ${CMAKE_CURRENT_SOURCE_DIR}/codegen/parser/data/nested-messages.pbs
            ${CMAKE_CURRENT_SOURCE_DIR}/codegen/parser/data/nested-forward.pbs
            ${codegen_nested_dir}/generate_headers.cmake
        VERBATIM)
    add_custom_target(codegen_nested_generated
        DEPENDS ${codegen_nested_messages_header} ${codegen_nested_forward_header})

    add_executable(codegen_nested_runtime_test
        ${codegen_nested_dir}/nested_runtime.cpp)
    add_dependencies(codegen_nested_runtime_test codegen_nested_generated)
    target_include_directories(codegen_nested_runtime_test PRIVATE
        ${easypb_test_include_dir} ${codegen_nested_generated_dir})
    add_test(NAME codegen.nested.runtime COMMAND codegen_nested_runtime_test)

    add_test(NAME codegen.modes
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/codegen/parser/data
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/codegen/parser/test_codegen_modes.cmake)

    # Self-recursive std::vector<T> is already part of Codegen's practical
    # compiler/library baseline: descriptor.pb.hpp itself contains
    # std::vector<DescriptorProto> nested_type. Compile the generated repeated
    # runtime test unconditionally so this remains an explicit regression test.
    #
    # Recursive std::map<K, T> has no equivalent standard guarantee, so keep
    # that runtime test conditional on a configure-time capability probe.
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles([=[
        #include <map>
        #include <string>
        struct MapNode { std::map<std::string, MapNode> children; };
        int main()
        {
            MapNode mapped;
            mapped.children["child"] = MapNode();
            return 0;
        }
    ]=] EASYPB_HAS_SELF_RECURSIVE_DEFAULT_MAP)

    set(codegen_recursive_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/recursive)
    set(codegen_recursive_generated_dir
        ${CMAKE_CURRENT_BINARY_DIR}/generated/codegen-recursive)
    set(codegen_recursive_repeated_header
        ${codegen_recursive_generated_dir}/recursive-repeated.generated.hpp)
    set(codegen_recursive_map_header
        ${codegen_recursive_generated_dir}/recursive-map.generated.hpp)

    add_custom_command(
        OUTPUT
            ${codegen_recursive_repeated_header}
            ${codegen_recursive_map_header}
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/codegen/parser/data
            -DOUTPUT_DIR=${codegen_recursive_generated_dir}
            -P ${codegen_recursive_dir}/generate_headers.cmake
        DEPENDS
            easypb_codegen
            ${CMAKE_CURRENT_SOURCE_DIR}/codegen/parser/data/nested-recursive-repeated.pbs
            ${CMAKE_CURRENT_SOURCE_DIR}/codegen/parser/data/nested-recursive-map.pbs
            ${codegen_recursive_dir}/generate_headers.cmake
        VERBATIM)
    add_custom_target(codegen_recursive_generated
        DEPENDS
            ${codegen_recursive_repeated_header}
            ${codegen_recursive_map_header})

    add_executable(codegen_recursive_repeated_runtime_test
        ${codegen_recursive_dir}/recursive_repeated_runtime.cpp)
    add_dependencies(codegen_recursive_repeated_runtime_test codegen_recursive_generated)
    target_include_directories(codegen_recursive_repeated_runtime_test PRIVATE
        ${easypb_test_include_dir} ${codegen_recursive_generated_dir})
    add_test(NAME codegen.recursive.repeated.runtime
        COMMAND codegen_recursive_repeated_runtime_test)

    if(EASYPB_HAS_SELF_RECURSIVE_DEFAULT_MAP)
        add_executable(codegen_recursive_map_runtime_test
            ${codegen_recursive_dir}/recursive_map_runtime.cpp)
        add_dependencies(codegen_recursive_map_runtime_test codegen_recursive_generated)
        target_include_directories(codegen_recursive_map_runtime_test PRIVATE
            ${easypb_test_include_dir} ${codegen_recursive_generated_dir})
        add_test(NAME codegen.recursive.map.runtime
            COMMAND codegen_recursive_map_runtime_test)
    endif()

    if(EASYPB_CODEGEN_WITH_PROTO_PARSER)
        add_executable(parser_tests codegen/parser/test_parser.cpp)
        target_include_directories(parser_tests PRIVATE
            ${easypb_test_include_dir}
            ${PROJECT_SOURCE_DIR}/codegen
            ${PROJECT_SOURCE_DIR}/codegen/parser)
        target_link_libraries(parser_tests PRIVATE easypb_proto_parser)
        add_test(NAME parser.unit COMMAND parser_tests)
    endif()

    add_executable(codegen_schema_storage_test codegen/schema/test_storage.cpp)
    target_link_libraries(codegen_schema_storage_test PRIVATE easypb_schema)
    add_test(NAME codegen.schema.storage COMMAND codegen_schema_storage_test)

    add_executable(codegen_schema_semantics_test codegen/schema/test_semantics.cpp)
    target_link_libraries(codegen_schema_semantics_test PRIVATE easypb_schema)
    add_test(NAME codegen.schema.semantics COMMAND codegen_schema_semantics_test)

    add_executable(codegen_imports_graph_test codegen/imports/test_graph.cpp)
    target_link_libraries(codegen_imports_graph_test PRIVATE easypb_schema)
    add_test(NAME codegen.imports.graph COMMAND codegen_imports_graph_test)

    # Descriptor-only linker suite: links easypb_schema alone, so it must keep
    # building and passing when the proto parser is off. The tests source
    # directory is a positional argument: the Step 7.4 corpus checkpoint links
    # the tracked `.pbs` fixtures, which live under several data directories.
    add_executable(codegen_imports_linker_test codegen/imports/test_linker.cpp)
    target_link_libraries(codegen_imports_linker_test PRIVATE easypb_schema)
    add_test(NAME codegen.imports.linker
        COMMAND codegen_imports_linker_test ${CMAKE_CURRENT_SOURCE_DIR})

    # Plan 5's descriptor-ingestion suite: the common adapter (owned copies,
    # interned strings) and the standalone `.pbs` wrapper, with the
    # buffer-destruction lifetime rows (D1/D2). Parser-free like the linker
    # suite above, so it runs in every profile. The tests source directory is
    # a positional argument (the tracked `.pbs` fixtures); the scratch
    # containers the suite writes for the shapes the corpus does not carry
    # live below the pinned working directory, never in the source tree.
    add_executable(codegen_imports_descriptor_test
        codegen/imports/test_descriptor_sets.cpp)
    target_link_libraries(codegen_imports_descriptor_test
        PRIVATE easypb_descriptor_loader easypb_codegen_core)
    add_test(NAME codegen.imports.descriptor
        COMMAND codegen_imports_descriptor_test ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(codegen.imports.descriptor PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    # Plan 5's CLI lane: the mode table, stdout cardinality, publication
    # snapshots (P1/P8/P9) and option validation, driven against the built
    # executable. Parser-dependent rows are guarded by FULL_BUILD inside the
    # script, so the lane runs in the lite profile too.
    add_test(NAME codegen.imports.cli
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/codegen/parser/data
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/codegen-cli
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/codegen/cli/test_codegen_cli.cmake)

    # Plan 5's output half, parser-free like the descriptor suite: the pure
    # logical mapping and the standalone publisher (P4-P10). P2's injected
    # failure is staged here, where a denied directory (POSIX) or a read-only
    # replacement target (Windows) is available. Scratch lives below the
    # pinned working directory, named after the process.
    add_executable(codegen_imports_output_test codegen/imports/test_output.cpp)
    target_link_libraries(codegen_imports_output_test
        PRIVATE easypb_header_output easypb_codegen_core)
    add_test(NAME codegen.imports.output COMMAND codegen_imports_output_test)
    set_tests_properties(codegen.imports.output PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    # Plan 5's in-memory lane: the owned adapter and the linked generator
    # through easypb_codegen_core alone (T4: no descriptor-file wrapper, no
    # native helper, no publisher). The driver prepares the strict `.pbs`
    # reference with the CLI, then runs the core-only binary from an empty
    # working directory and requires byte equality of names and content.
    add_executable(codegen_imports_in_memory_test
        codegen/imports/test_in_memory.cpp)
    target_link_libraries(codegen_imports_in_memory_test
        PRIVATE easypb_codegen_core)
    add_test(NAME codegen.imports.in_memory
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DIN_MEMORY=$<TARGET_FILE:codegen_imports_in_memory_test>
            -DTESTS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/codegen-in-memory
            -P ${CMAKE_CURRENT_SOURCE_DIR}/codegen/imports/test_in_memory.cmake)

    # Plan 5's include-planner lane (I2/I3/I4): the support-import exception,
    # its real-reference and public-re-export counterexamples, and the
    # completeness refusal, driven through the CLI on committed complete
    # `.pbs` closures. Descriptor-only, so every profile runs it.
    add_test(NAME codegen.imports.includes
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/codegen/imports/data/include_rules
            -DCUSTOM_TYPES_PBS=${CMAKE_CURRENT_SOURCE_DIR}/codegen/custom_types/data/custom_types.pbs
            -P ${CMAKE_CURRENT_SOURCE_DIR}/codegen/imports/test_includes.cmake)

    # Plan 5's runtime integration: two translation units sharing the
    # generated dependency header, both root and dependency headers in one
    # TU, and the round trips (imported nonzero enum default, packed enum,
    # nested message, message-valued map). Headers are generated independently
    # per target; under FULL_BUILD the source frontend must reproduce each
    # header byte for byte.
    set(codegen_imports_runtime_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/imports/runtime)
    set(codegen_imports_project_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/imports/data/project)
    set(codegen_imports_runtime_generated_dir
        ${CMAKE_CURRENT_BINARY_DIR}/generated/codegen-imports-runtime)
    add_custom_command(
        OUTPUT
            ${codegen_imports_runtime_generated_dir}/api/report.pb.hpp
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DPBS=${CMAKE_CURRENT_SOURCE_DIR}/codegen/imports/data/project.pbs
            -DSOURCE_ROOT=${codegen_imports_project_dir}
            -DOUTPUT_DIR=${codegen_imports_runtime_generated_dir}
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${codegen_imports_runtime_dir}/generate_runtime_headers.cmake
        BYPRODUCTS
            ${codegen_imports_runtime_generated_dir}/common/state.pb.hpp
            ${codegen_imports_runtime_generated_dir}/common/node.pb.hpp
        DEPENDS
            easypb_codegen
            ${CMAKE_CURRENT_SOURCE_DIR}/codegen/imports/data/project.pbs
            ${codegen_imports_project_dir}/common/state.proto
            ${codegen_imports_project_dir}/common/node.proto
            ${codegen_imports_project_dir}/api/report.proto
            ${codegen_imports_runtime_dir}/generate_runtime_headers.cmake
        VERBATIM)
    add_custom_target(codegen_imports_runtime_generated
        DEPENDS ${codegen_imports_runtime_generated_dir}/api/report.pb.hpp)
    add_executable(codegen_imports_runtime_test
        ${codegen_imports_runtime_dir}/two_tu_main.cpp
        ${codegen_imports_runtime_dir}/two_tu_shared.cpp)
    add_dependencies(codegen_imports_runtime_test
        codegen_imports_runtime_generated)
    target_include_directories(codegen_imports_runtime_test PRIVATE
        ${easypb_test_include_dir} ${codegen_imports_runtime_generated_dir})
    add_test(NAME codegen.imports.runtime COMMAND codegen_imports_runtime_test)

    # Step 11's two-way oracle parity lane, the descriptor half. Both halves
    # read codegen/imports/data/parity/parity-cases.tsv from this one source
    # file and assert every row's verdict; they are two registrations rather
    # than one because the A12 split makes "parser-free" and "parser-dependent"
    # mutually exclusive target classes (verify-package's `iso:parser-free
    # targets link nothing the parser owns`): a target outside the parser guard
    # must link `easypb_schema` alone in every tree. So the descriptor rows are
    # registered here, for every profile, and the binary NAMES each
    # source-origin row it does not read with the pack's `SKIP:` marker; the
    # source half sits inside the parser guard beside the other loader suites.
    add_executable(codegen_imports_parity_test codegen/imports/test_parity.cpp)
    target_link_libraries(codegen_imports_parity_test PRIVATE easypb_schema)
    target_compile_definitions(codegen_imports_parity_test
        PRIVATE EASYPB_PARITY_HALF_DESCRIPTOR=1)
    add_test(NAME codegen.imports.parity
        COMMAND codegen_imports_parity_test ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(codegen_imports_graph_exhaustive_test
        codegen/imports/test_graph_exhaustive.cpp)
    target_link_libraries(codegen_imports_graph_exhaustive_test PRIVATE easypb_schema)
    add_test(NAME codegen.imports.graph.exhaustive
        COMMAND codegen_imports_graph_exhaustive_test)
    set_tests_properties(codegen.imports.graph.exhaustive PROPERTIES
        LABELS "property")

    if(EASYPB_EXTENDED_TESTS)
        add_executable(codegen_imports_graph_extended_test
            codegen/imports/test_graph_extended.cpp)
        target_link_libraries(codegen_imports_graph_extended_test PRIVATE easypb_schema)
        add_test(NAME codegen.imports.graph.extended
            COMMAND codegen_imports_graph_extended_test)
        set_tests_properties(codegen.imports.graph.extended PROPERTIES
            LABELS "extended;property")
    endif()

    if(EASYPB_CODEGEN_WITH_PROTO_PARSER)
        add_executable(codegen_imports_loader_test codegen/imports/test_loader.cpp)
        target_include_directories(codegen_imports_loader_test PRIVATE
            ${easypb_test_include_dir}
            ${PROJECT_SOURCE_DIR}/codegen
            ${PROJECT_SOURCE_DIR}/codegen/parser)
        target_link_libraries(codegen_imports_loader_test PRIVATE easypb_proto_loader)
        if(WIN32)
            # The loader suite's Windows denial test reproduces EACCES
            # with a deny ACE, which needs the security APIs.
            target_link_libraries(codegen_imports_loader_test PRIVATE advapi32)
        endif()
        add_test(NAME codegen.imports.loader
            COMMAND codegen_imports_loader_test
                ${CMAKE_CURRENT_SOURCE_DIR}/codegen/imports/data/loader)
        # Pin the scratch directory explicitly: the test creates
        # imports-loader-work below its working directory.
        set_tests_properties(codegen.imports.loader PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

        # Source-driven linker suite: needs load_source_files and the parser
        # include directories, so it lives inside the parser guard. The data
        # directory is a positional argument and the scratch directory is
        # pinned, mirroring the loader suite above.
        add_executable(codegen_imports_source_linker_test
            codegen/imports/test_source_linker.cpp)
        target_include_directories(codegen_imports_source_linker_test PRIVATE
            ${easypb_test_include_dir}
            ${PROJECT_SOURCE_DIR}/codegen
            ${PROJECT_SOURCE_DIR}/codegen/parser)
        target_link_libraries(codegen_imports_source_linker_test
            PRIVATE easypb_proto_loader)
        # Step 8.3: the corpus directory and the oracle directory the
        # differential lane generates. The second argument is optional to the
        # binary - without it every accepting fixture prints a `SKIP:` line and
        # the run still succeeds on a machine that has not generated the sets.
        add_test(NAME codegen.imports.source
            COMMAND codegen_imports_source_linker_test
                ${CMAKE_CURRENT_SOURCE_DIR}/codegen/imports/data/linker
                ${PROJECT_SOURCE_DIR}/build/package-imports/oracle)
        set_tests_properties(codegen.imports.source PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

        # Step 11's parity lane, the source half: the same table's
        # source-origin rows through the loader's own path (the pack's
        # plan4/steps/06-oracle-parity.md, section 11.4). The descriptor half is
        # registered above, outside this guard, so the two may not share one
        # target (A12).
        add_executable(codegen_imports_parity_source_test
            codegen/imports/test_parity.cpp)
        target_include_directories(codegen_imports_parity_source_test PRIVATE
            ${easypb_test_include_dir}
            ${PROJECT_SOURCE_DIR}/codegen
            ${PROJECT_SOURCE_DIR}/codegen/parser)
        target_link_libraries(codegen_imports_parity_source_test
            PRIVATE easypb_proto_loader)
        target_compile_definitions(codegen_imports_parity_source_test
            PRIVATE EASYPB_PARITY_HALF_SOURCE=1)
        add_test(NAME codegen.imports.parity.source
            COMMAND codegen_imports_parity_source_test
                ${CMAKE_CURRENT_SOURCE_DIR})

    endif()
endif()

if(EASYPB_BUILD_DIFFERENTIAL_TESTS)
    if(NOT EASYPB_CODEGEN_WITH_PROTO_PARSER)
        message(FATAL_ERROR
            "EASYPB_BUILD_DIFFERENTIAL_TESTS requires EASYPB_CODEGEN_WITH_PROTO_PARSER")
    endif()
    add_executable(parser_differential_compare
        codegen/parser/differential/compare_with_protoc.cpp)
    target_include_directories(parser_differential_compare PRIVATE
        ${easypb_test_include_dir}
        ${PROJECT_SOURCE_DIR}/codegen
        ${PROJECT_SOURCE_DIR}/codegen/parser)
    target_link_libraries(parser_differential_compare PRIVATE easypb_proto_parser)

    # The corpus generator's idempotence gate: the corpus README claims that
    # tests/codegen/imports/data/parity is `generate.py`'s output, and this row
    # checks it mechanically (hash every owned file, regenerate, hash again,
    # fail naming every change). It is registered by name only when the
    # toolchain is actually present - an interpreter and `EASYPB_PROTOC` -
    # because a test that quietly skips is the failure mode the `SKIP:` rule
    # exists for; the configure log names the missing half instead.
    # Overridable: -DEASYPB_PYTHON=<path> wins over the search.
    find_program(EASYPB_PYTHON NAMES python python3)
    if(WIN32 AND EASYPB_PYTHON MATCHES "[/\\\\]usr[/\\\\]bin")
        # An MSYS2 `usr/bin` interpreter (a POSIX build, unlike the
        # project's ucrt64 toolchain) mishandles the `C:/...`
        # arguments this script and protoc are addressed with; the row
        # is registered only for a native Windows Python. On other
        # platforms `/usr/bin/python3` is the normal interpreter.
        set(EASYPB_PYTHON "EASYPB_PYTHON-NOTFOUND")
    endif()
    if(EASYPB_PYTHON AND EASYPB_PROTOC)
        add_test(NAME codegen.imports.parity.generator
            COMMAND ${EASYPB_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/codegen/imports/data/parity/check_generator.py
                --protoc ${EASYPB_PROTOC})
        # The gate's path contract: a relative --descriptor-proto-path belongs
        # to the directory the gate is invoked from (the generator itself runs
        # with the corpus as its cwd), and a refusal names the spelling the
        # caller typed. Reads and writes nothing.
        add_test(NAME codegen.imports.parity.generator.paths
            COMMAND ${EASYPB_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/codegen/imports/data/parity/check_generator_paths.py
                --protoc ${EASYPB_PROTOC})
    else()
        message(STATUS
            "EasyProtoBuf: codegen.imports.parity.generator[.paths] not "
            "registered (set EASYPB_PROTOC and put a native python on PATH)")
    endif()
endif()
