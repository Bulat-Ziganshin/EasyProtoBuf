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
    # input paths, runs the runtime check of the default values, and drives
    # the refusals through the CLI. The descriptor path builds the headers
    # the runtime checker reads, because it is the one every build profile
    # has; the driver compares the source path's header with the descriptor
    # path's byte for byte. Compiling the descriptor-path header is the
    # build's job (cl.exe started from a test script outside the Visual
    # Studio environment cannot find its own standard library headers), and
    # the equality check extends that compile to the source path.
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
        defaults-nan
        defaults-float-overflow
        defaults-bytes-upper-hex
        defaults-spellings
        defaults-float-keywords
        packages-deep-101
        enum-alias-same-number
        json-names-distinct
        json-name-custom-vs-default-proto2
        json-name-proto2-warning
        empty-syntax-descriptor
        plus-controls
        plus-descriptor-controls
        leading-space-control
        literals-escapes
        literals-lexemes
        literals-descriptor-octal)

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
            ${codegen_validity_descriptor_dir}/defaults-nan.pb.hpp
            ${codegen_validity_descriptor_dir}/defaults-float-overflow.pb.hpp
            ${codegen_validity_descriptor_dir}/defaults-bytes-upper-hex.pb.hpp
            ${codegen_validity_descriptor_dir}/defaults-spellings.pb.hpp
            ${codegen_validity_descriptor_dir}/defaults-float-keywords.pb.hpp
            ${codegen_validity_descriptor_dir}/packages-deep-101.pb.hpp
            ${codegen_validity_descriptor_dir}/enum-alias-same-number.pb.hpp
            ${codegen_validity_descriptor_dir}/json-names-distinct.pb.hpp
            ${codegen_validity_descriptor_dir}/json-name-custom-vs-default-proto2.pb.hpp
            ${codegen_validity_descriptor_dir}/json-name-proto2-warning.pb.hpp
            ${codegen_validity_descriptor_dir}/empty-syntax-descriptor.pb.hpp
            ${codegen_validity_descriptor_dir}/plus-controls.pb.hpp
            ${codegen_validity_descriptor_dir}/plus-descriptor-controls.pb.hpp
            ${codegen_validity_descriptor_dir}/leading-space-control.pb.hpp
            ${codegen_validity_descriptor_dir}/literals-escapes.pb.hpp
            ${codegen_validity_descriptor_dir}/literals-lexemes.pb.hpp
            ${codegen_validity_descriptor_dir}/literals-descriptor-octal.pb.hpp)

    add_executable(codegen_validity_runtime_test
        ${codegen_validity_dir}/validity_runtime.cpp)
    add_dependencies(codegen_validity_runtime_test codegen_validity_generated)
    target_include_directories(codegen_validity_runtime_test PRIVATE
        ${easypb_test_include_dir} ${codegen_validity_descriptor_dir})

    add_test(NAME codegen.validity
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${codegen_validity_data_dir}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/codegen-validity
            -DRUNTIME=$<TARGET_FILE:codegen_validity_runtime_test>
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${codegen_validity_dir}/test_validity.cmake)

    # R1 / B3: the default-value pipeline family's second option set. The
    # same two fixtures are generated with `--no-has-fields`; the driver
    # compares source and descriptor headers byte for byte for that set too.
    set(codegen_validity_nohas_fixtures
        defaults-spellings
        defaults-float-keywords)
    foreach(name ${codegen_validity_nohas_fixtures})
        add_custom_command(
            OUTPUT ${codegen_validity_descriptor_dir}/${name}-nohas.pb.hpp
            COMMAND ${CMAKE_COMMAND}
                -DCODEGEN=$<TARGET_FILE:easypb_codegen>
                -DINPUT=${codegen_validity_data_dir}/${name}.pbs
                -DOUTPUT=${codegen_validity_descriptor_dir}/${name}-nohas.pb.hpp
                -DOPTIONS=--no-has-fields
                -P ${codegen_validity_dir}/generate_header.cmake
            DEPENDS
                easypb_codegen
                ${codegen_validity_data_dir}/${name}.pbs
                ${codegen_validity_dir}/generate_header.cmake
            VERBATIM)
    endforeach()
    add_custom_target(codegen_validity_nohas_generated
        DEPENDS
            ${codegen_validity_descriptor_dir}/defaults-spellings-nohas.pb.hpp
            ${codegen_validity_descriptor_dir}/defaults-float-keywords-nohas.pb.hpp)

    # P3 for the default family: every header the lane accepts must be
    # formally well-formed C++11, not merely accepted by the host compiler.
    # The source header is byte-identical to the descriptor header (the
    # driver asserts it), so one compile per fixture and option set covers
    # both input paths.
    set(codegen_validity_pedantic_dir
        ${CMAKE_CURRENT_BINARY_DIR}/generated/codegen-validity-pedantic)
    file(MAKE_DIRECTORY ${codegen_validity_pedantic_dir})
    set(codegen_validity_pedantic_sources)
    foreach(header
            defaults-spellings
            defaults-spellings-nohas
            defaults-float-keywords
            defaults-float-keywords-nohas)
        set(source ${codegen_validity_pedantic_dir}/compile-${header}.cpp)
        file(WRITE ${source} "#include \"${header}.pb.hpp\"\n")
        list(APPEND codegen_validity_pedantic_sources ${source})
    endforeach()
    add_library(codegen_validity_pedantic_check STATIC
        ${codegen_validity_pedantic_sources})
    add_dependencies(codegen_validity_pedantic_check
        codegen_validity_generated codegen_validity_nohas_generated)
    set_target_properties(codegen_validity_pedantic_check PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(codegen_validity_pedantic_check PRIVATE
        ${easypb_test_include_dir} ${codegen_validity_descriptor_dir})
    target_compile_options(codegen_validity_pedantic_check PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-pedantic-errors>)
    add_dependencies(codegen_validity_runtime_test
        codegen_validity_pedantic_check)

    # Plan 5 follow-up S2 (W-08, W-09): the emitter's name layer. The lane
    # generates every fixture through BOTH input paths, checks that every
    # escaped name still carries the data the schema names, and drives the
    # presence-flag collision through the CLI. The descriptor path builds the
    # headers the runtime checker reads, because it is the one every build
    # profile has; the driver compares the source path's header with the
    # descriptor path's byte for byte. Compiling the descriptor-path header
    # is the build's job (cl.exe started from a test script outside the
    # Visual Studio environment cannot find its own standard library
    # headers), and the equality check extends that compile to the source
    # path.
    set(codegen_names_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/names)
    set(codegen_names_data_dir ${codegen_names_dir}/data)
    set(codegen_names_generated_dir
        ${CMAKE_CURRENT_BINARY_DIR}/generated/codegen-names)
    set(codegen_names_descriptor_dir
        ${codegen_names_generated_dir}/descriptor)
    set(codegen_names_fixtures
        keyword-field
        keyword-message
        keyword-enum
        keyword-enum-value
        keyword-nested)

    foreach(name ${codegen_names_fixtures})
        add_custom_command(
            OUTPUT ${codegen_names_descriptor_dir}/${name}.pb.hpp
            COMMAND ${CMAKE_COMMAND}
                -DCODEGEN=$<TARGET_FILE:easypb_codegen>
                -DINPUT=${codegen_names_data_dir}/${name}.pbs
                -DOUTPUT=${codegen_names_descriptor_dir}/${name}.pb.hpp
                -P ${codegen_names_dir}/generate_header.cmake
            DEPENDS
                easypb_codegen
                ${codegen_names_data_dir}/${name}.pbs
                ${codegen_names_dir}/generate_header.cmake
            VERBATIM)
    endforeach()
    add_custom_target(codegen_names_generated
        DEPENDS
            ${codegen_names_descriptor_dir}/keyword-field.pb.hpp
            ${codegen_names_descriptor_dir}/keyword-message.pb.hpp
            ${codegen_names_descriptor_dir}/keyword-enum.pb.hpp
            ${codegen_names_descriptor_dir}/keyword-enum-value.pb.hpp
            ${codegen_names_descriptor_dir}/keyword-nested.pb.hpp)

    add_executable(codegen_names_runtime_test
        ${codegen_names_dir}/names_runtime.cpp)
    add_dependencies(codegen_names_runtime_test codegen_names_generated)
    target_include_directories(codegen_names_runtime_test PRIVATE
        ${easypb_test_include_dir} ${codegen_names_descriptor_dir})

    add_test(NAME codegen.names
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${codegen_names_data_dir}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/codegen-names
            -DRUNTIME=$<TARGET_FILE:codegen_names_runtime_test>
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${codegen_names_dir}/test_names.cmake)

    # Plan 5 follow-up S3 (W-01, W-30, W-32): the boundaries of untrusted
    # input. The lane drives the nesting limit through BOTH input paths (the
    # 31 levels the reference compiler accepts, the 32 it refuses),
    # compiles the accepted header with the tree's own compiler and standard,
    # checks that the accepted nesting carries the schema's data, and drives
    # the refusals through the CLI. The descriptor path builds the header the
    # runtime checker reads, because it is the one every build profile has;
    # the driver compares the source path's header with the descriptor path's
    # byte for byte.
    set(codegen_limits_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/limits)
    set(codegen_limits_data_dir ${codegen_limits_dir}/data)
    set(codegen_limits_generated_dir
        ${CMAKE_CURRENT_BINARY_DIR}/generated/codegen-limits)
    set(codegen_limits_descriptor_dir
        ${codegen_limits_generated_dir}/descriptor)

    # MSVC cannot compile the accepted header. Its compiler limit is 16
    # levels of nested struct definitions - C1046, "compiler limit: struct
    # nested too deeply" - and the accepted nesting is 31, so the limit bites
    # at the 17th struct. The header is a chain of nested definitions:
    #
    #     struct M0 { struct M1 { ... struct M30 { ... }; }; };
    #
    # Every MSVC lane of 2026-10-22 therefore died in the build of the
    # runtime checker, and with it the whole Windows half of the CI: the
    # build failure ends the job before ctest runs, so no Windows lane
    # executed a single test.
    #
    # Clang 22.1.0 through 23.1.0 has the other reason to skip. A declaration
    # whose type is spelled as a long nested-name-specifier costs exponential
    # time there - roughly twice the work for every level past ~20 - so one
    # `M0::M1::...::M30 v;` alone dwarfs the whole tree. Neither warnings, nor
    # -fsyntax-only instead of -c, nor removing <easypb.hpp> moves it; the
    # struct definitions alone compile in milliseconds, it is the mention of
    # the deep type that explodes. The generated header carries 62 such
    # declarations, so every clang 22/23 lane of 2026-10-22 paid ~8 minutes
    # for the runtime checker's TU and ~6 more for the driver's -fsyntax-only
    # per build type, and seven lanes were canceled at their job timeout.
    # GCC and Clang 20/21 stay flat at depth 28 (tens of milliseconds;
    # Clang 21.1.0 measured clean on the Compiler Explorer machines), and
    # LLVM main already resolves the same source in ~5 ms, so this gate
    # lifts with Clang 24.
    #
    # The limit itself does not move. It stays the reference compiler's own
    # (31 accepted, the 32nd refused - the driver's own verdict below), and
    # the generator keeps printing the nesting the way it does. What these
    # profiles give up is the compile of that one header and the runtime
    # check that reads it; the lane's other three things need no compiler
    # and stay - both input paths agree byte for byte, the 32nd level is
    # refused, the deep name is refused. Nested types themselves remain
    # covered on MSVC by the nested lane, which is shallower.
    set(codegen_limits_skip_reason "the compiler limit of this profile")
    if(MSVC)
        set(codegen_limits_compiles_accepted 0)
        set(codegen_limits_skip_reason
            "MSVC refuses a chain of 31 nested struct definitions (C1046)")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 22
            AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 24)
        # AppleClang is deliberately outside this branch: it has its own
        # numbering and its macOS lanes ran this test in under 2 s.
        set(codegen_limits_compiles_accepted 0)
        set(codegen_limits_skip_reason
            "Clang 22-23 grow a 31-level nested name exponentially (fixed in LLVM 24)")
    else()
        set(codegen_limits_compiles_accepted 1)
    endif()

    if(codegen_limits_compiles_accepted)
        add_custom_command(
            OUTPUT ${codegen_limits_descriptor_dir}/nesting-31.pb.hpp
            COMMAND ${CMAKE_COMMAND}
                -DCODEGEN=$<TARGET_FILE:easypb_codegen>
                -DINPUT=${codegen_limits_data_dir}/nesting-31.pbs
                -DOUTPUT=${codegen_limits_descriptor_dir}/nesting-31.pb.hpp
                -P ${codegen_limits_dir}/generate_header.cmake
            DEPENDS
                easypb_codegen
                ${codegen_limits_data_dir}/nesting-31.pbs
                ${codegen_limits_dir}/generate_header.cmake
            VERBATIM)
        add_custom_target(codegen_limits_generated
            DEPENDS ${codegen_limits_descriptor_dir}/nesting-31.pb.hpp)

        add_executable(codegen_limits_runtime_test
            ${codegen_limits_dir}/limits_runtime.cpp)
        add_dependencies(codegen_limits_runtime_test codegen_limits_generated)
        target_include_directories(codegen_limits_runtime_test PRIVATE
            ${easypb_test_include_dir} ${codegen_limits_descriptor_dir})
    endif()

    # The tree's own standard: the C++11 profile passes its standard through
    # EASYPB_CXX_FLAGS, so CMAKE_CXX_STANDARD is empty there and the flags
    # carry it. The flags are split into a list, because the profile's
    # standard arrives as one string with a leading space, and a compiler
    # does not read " -std=c++11" as the option it is.
    if(CMAKE_CXX_STANDARD)
        set(codegen_limits_std_flag "-std=c++${CMAKE_CXX_STANDARD}")
    else()
        set(codegen_limits_std_flag "")
    endif()
    separate_arguments(codegen_limits_cxx_flags NATIVE_COMMAND
        "${CMAKE_CXX_FLAGS}")

    # The driver compiles the accepted header and runs the checker that reads
    # it - the two steps the skip profiles (MSVC, Clang 22-23) cannot do. It
    # is told which profile it runs in and why the steps are skipped, and the
    # compiler arguments and the runtime checker are passed only
    # where they exist.
    #
    # The two calls below are spelled out instead of sharing one argument
    # list: the compiler flags reach the driver as ONE argument, with the
    # semicolons that separate them, and a list would split that argument
    # into "-Wall", "-Wextra" and the rest - the driver would then call the
    # compiler without the flags it is supposed to use.
    if(codegen_limits_compiles_accepted)
        add_test(NAME codegen.limits
            COMMAND ${CMAKE_COMMAND}
                -DCODEGEN=$<TARGET_FILE:easypb_codegen>
                -DDATA_DIR=${codegen_limits_data_dir}
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/codegen-limits
                -DRUNTIME=$<TARGET_FILE:codegen_limits_runtime_test>
                -DCXX=${CMAKE_CXX_COMPILER}
                "-DCXX_FLAGS=${codegen_limits_cxx_flags}"
                -DCXX_STD_FLAG=${codegen_limits_std_flag}
                -DINCLUDE_DIR=${easypb_test_include_dir}
                -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
                -DCOMPILE_ACCEPTED=1
                -P ${codegen_limits_dir}/test_limits.cmake)
    else()
        add_test(NAME codegen.limits
            COMMAND ${CMAKE_COMMAND}
                -DCODEGEN=$<TARGET_FILE:easypb_codegen>
                -DDATA_DIR=${codegen_limits_data_dir}
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/codegen-limits
                -DINCLUDE_DIR=${easypb_test_include_dir}
                "-DSKIP_REASON=${codegen_limits_skip_reason}"
                -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
                -DCOMPILE_ACCEPTED=0
                -P ${codegen_limits_dir}/test_limits.cmake)
    endif()

    # Plan 5 follow-up S5 (W-16, W-36, W-37, W-44, W-45, W-68): one rule,
    # two input paths. The lane drives every accepted fixture through BOTH
    # fronts and requires one header byte for byte, and requires every
    # refusal the source path makes on the descriptor path too. The
    # descriptor halves are committed, so the lane never calls protoc.
    #
    # The accepted headers are compiled by the build, not by the driver:
    # cl.exe started from a test script outside the Visual Studio
    # environment cannot find its own standard library headers (C1083 on
    # <cstdint>). One compile unit per fixture is generated here because
    # several fixtures declare the same package-scope name (parity::M), and
    # the driver's byte-for-byte comparison extends the compile to the
    # source path.
    set(codegen_parity_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/parity)
    set(codegen_parity_data_dir ${codegen_parity_dir}/data)
    set(codegen_parity_generated_dir
        ${CMAKE_CURRENT_BINARY_DIR}/generated/codegen-parity)
    file(MAKE_DIRECTORY ${codegen_parity_generated_dir})

    set(codegen_parity_compile_fixtures
        methods-ok
        map-ok
        enum-ok
        ext-ok
        ext-max
        message-set-empty
        w36-same
        w68-root
        w68-dep)
    set(codegen_parity_compile_sources)
    set(codegen_parity_headers)
    foreach(name ${codegen_parity_compile_fixtures})
        # The merged W-36 header is the header of a single identical
        # container; the test proves the merge produces those same bytes.
        if("${name}" STREQUAL "w36-same")
            set(codegen_parity_input
                ${codegen_parity_data_dir}/w36-same-a.pbs)
        else()
            set(codegen_parity_input ${codegen_parity_data_dir}/${name}.pbs)
        endif()
        set(codegen_parity_header
            ${codegen_parity_generated_dir}/${name}.pb.hpp)
        add_custom_command(
            OUTPUT ${codegen_parity_header}
            COMMAND ${CMAKE_COMMAND}
                -DCODEGEN=$<TARGET_FILE:easypb_codegen>
                -DINPUT=${codegen_parity_input}
                -DOUTPUT=${codegen_parity_header}
                -P ${codegen_parity_dir}/generate_header.cmake
            DEPENDS
                easypb_codegen
                ${codegen_parity_input}
                ${codegen_parity_dir}/generate_header.cmake
            VERBATIM)
        file(WRITE
            ${codegen_parity_generated_dir}/compile-${name}.cpp
            "#include \"${name}.pb.hpp\"\n")
        list(APPEND codegen_parity_compile_sources
            ${codegen_parity_generated_dir}/compile-${name}.cpp)
        list(APPEND codegen_parity_headers ${codegen_parity_header})
    endforeach()
    add_custom_target(codegen_parity_generated
        DEPENDS ${codegen_parity_headers})
    add_library(codegen_parity_compile_check STATIC
        ${codegen_parity_compile_sources})
    add_dependencies(codegen_parity_compile_check codegen_parity_generated)
    target_include_directories(codegen_parity_compile_check PRIVATE
        ${easypb_test_include_dir} ${codegen_parity_generated_dir})

    add_test(NAME codegen.parity
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${codegen_parity_data_dir}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/codegen-parity
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${codegen_parity_dir}/test_parity.cmake)

    # CR-25 is accepted, not fixed: extension declarations are neither
    # validated nor decoded. This test asserts the current direction (both
    # doors accept a shape protoc refuses), so it must FAIL when that changes;
    # the failure message names the decision that has to be re-evaluated. It is
    # a separate ctest name because the accepted register cites it by name;
    # `codegen.parity` above stays the property gate.
    add_test(NAME codegen.parity.extensions_not_validated
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DDATA_DIR=${codegen_parity_data_dir}
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${codegen_parity_dir}/test_extensions_not_validated.cmake)

    # Plan 5 follow-up S6 (W-11, W-47, W-48, W-58): the library API. A caller
    # that links easypb_codegen_core must get the CLI's answer: the entry
    # points finalize a copy of the options, the compatibility link checks a
    # default for its declared kind, ingest normalizes the file's name and
    # package presence bits, and the link result says one thing (`local_only`).
    # The lane links only the core, so it is parser-free and runs in the lite
    # profile; the `.proto` control is guarded by FULL_BUILD. The W-11 header
    # is compiled by the build through the binary's emit mode (a compiler
    # started from a test script outside the Visual Studio environment cannot
    # find its own standard library headers), and the driver compares the
    # library's header with the CLI's byte for byte.
    set(codegen_library_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/codegen/library)
    set(codegen_library_data_dir ${codegen_library_dir}/data)
    set(codegen_library_generated_dir
        ${CMAKE_CURRENT_BINARY_DIR}/generated/codegen-library)
    file(MAKE_DIRECTORY ${codegen_library_generated_dir})

    add_executable(codegen_library_test codegen/library/test_library.cpp)
    target_link_libraries(codegen_library_test PRIVATE easypb_codegen_core)

    set(codegen_library_options_header
        ${codegen_library_generated_dir}/options-default.pb.hpp)
    add_custom_command(
        OUTPUT ${codegen_library_options_header}
        COMMAND $<TARGET_FILE:codegen_library_test>
            --emit-options-default ${codegen_library_options_header}
            ${codegen_library_data_dir}
        DEPENDS
            codegen_library_test
            ${codegen_library_data_dir}/options-default.pbs
        VERBATIM)
    add_custom_target(codegen_library_options_generated
        DEPENDS ${codegen_library_options_header})

    set(codegen_library_compile_source
        ${codegen_library_generated_dir}/compile-options-default.cpp)
    file(WRITE ${codegen_library_compile_source}
        "#include \"options-default.pb.hpp\"\n")
    add_library(codegen_library_compile_check STATIC
        ${codegen_library_compile_source})
    add_dependencies(codegen_library_compile_check
        codegen_library_options_generated)
    set_source_files_properties(${codegen_library_compile_source} PROPERTIES
        OBJECT_DEPENDS ${codegen_library_options_header})
    target_include_directories(codegen_library_compile_check PRIVATE
        ${easypb_test_include_dir} ${codegen_library_generated_dir})

    add_test(NAME codegen.library
        COMMAND ${CMAKE_COMMAND}
            -DCODEGEN=$<TARGET_FILE:easypb_codegen>
            -DLIBRARY_TEST=$<TARGET_FILE:codegen_library_test>
            -DDATA_DIR=${codegen_library_data_dir}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/codegen-library
            -DFULL_BUILD=$<BOOL:${EASYPB_CODEGEN_WITH_PROTO_PARSER}>
            -P ${codegen_library_dir}/test_library.cmake)

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
            ${codegen_packages_generated_dir}/other.pb.hpp
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
            ${codegen_packages_data_dir}/other.pbs
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
            ${codegen_packages_no_class_header}
            ${codegen_packages_generated_dir}/other.pb.hpp)

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
            -DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/codegen/cli/data
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

    # W-31's natural working-directory failure cannot be staged through Win32,
    # so this test includes the unmodified file_paths.cpp with
    # GetCurrentDirectoryW stubbed. It is standalone: linking
    # easypb_header_output would define the module's symbols twice. It needs
    # easypb_schema for max_logical_path_components only.
    if(WIN32)
        add_executable(codegen_imports_output_cwd_test
            codegen/imports/test_cwd_failure.cpp)
        target_include_directories(codegen_imports_output_cwd_test PRIVATE
            ${PROJECT_SOURCE_DIR}/codegen)
        target_link_libraries(codegen_imports_output_cwd_test
            PRIVATE easypb_schema)
        add_test(NAME codegen.imports.output.cwd
            COMMAND codegen_imports_output_cwd_test)
        set_tests_properties(codegen.imports.output.cwd PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endif()

    # W-33's counter-example is POSIX-only. The test includes file_paths.cpp
    # with a wrapped write to inject EINTR and a zero return; the schema
    # library is linked for max_logical_path_components only.
    if(NOT WIN32)
        add_executable(codegen_imports_output_posix_writer_test
            codegen/imports/test_posix_writer.cpp)
        target_include_directories(codegen_imports_output_posix_writer_test
            PRIVATE ${PROJECT_SOURCE_DIR}/codegen)
        target_link_libraries(codegen_imports_output_posix_writer_test
            PRIVATE easypb_schema)
        add_test(NAME codegen.imports.output.posix-writer
            COMMAND codegen_imports_output_posix_writer_test)
        set_tests_properties(codegen.imports.output.posix-writer PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endif()

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
