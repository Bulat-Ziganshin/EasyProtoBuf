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
option(EASYPB_EXTENDED_TESTS
       "Build slower deterministic stress/property tests"
       OFF)

if(EASYPB_EXTENDED_TESTS AND NOT BUILD_TESTING)
    message(FATAL_ERROR
            "EASYPB_EXTENDED_TESTS requires BUILD_TESTING=ON")
endif()

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


# P-03: an unhandled enumerator must not compile.
# `outcome_code` in codegen/schema_semantics.cpp switches over an internal enum
# and has no `default:` label, so adding an outcome and forgetting its case has
# to break the build instead of silently reading as DIAGNOSTIC_GENERIC. The flag
# therefore sits on every target, the library included - the switch is library
# code, and a flag on the test executables alone would guard the one place the
# defect cannot be. `add_compile_options` is directory-scoped, so this governs
# EasyProtoBuf's own targets (library, code generator, examples, tests) and not
# a parent project that adds this directory as a subdirectory.
# GNU and Clang only. MSVC is deliberately not set here: no MSVC lane in this
# repository measures it, and the plan-4 package's verifier records that gap as
# a note instead of pretending it is covered. A future MSVC lane needs BOTH
# codes, because the two switch shapes differ: C4062 ("no default label that can
# catch it") is this flag's analogue for `outcome_code`, which has no
# `default:` label, while C4061 ("has a default case") covers a default-bearing
# switch such as the ImportInfo modifier switch in codegen/schema.cpp. Both are
# level 4 and off by default, so each needs its own /we (fix round r5, H-3).
include(CheckCXXCompilerFlag)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    check_cxx_compiler_flag("-Werror=switch" EASYPB_HAS_WERROR_SWITCH)
    if(EASYPB_HAS_WERROR_SWITCH)
        add_compile_options("-Werror=switch")
        message(STATUS "EasyProtoBuf: -Werror=switch is on (P-03: an unhandled enumerator fails the build)")
    else()
        message(WARNING "EasyProtoBuf: this compiler rejects -Werror=switch, so P-03's compile-time guarantee is not enforced")
    endif()
endif()


add_executable(decoder examples/decoder/decoder.cpp)
target_include_directories(decoder PRIVATE include)

add_executable(tutorial examples/tutorial/main.cpp)
target_include_directories(tutorial PRIVATE include)

add_executable(manual_example examples/manual/main.cpp)
target_include_directories(manual_example PRIVATE include)

# "codegen" became a reserved target name in CMake 3.31. Keep the produced
# executable name while using a non-reserved internal target name.
add_executable(easypb_codegen codegen/main.cpp codegen/cpp_names.cpp)
set_target_properties(easypb_codegen PROPERTIES OUTPUT_NAME codegen)
target_include_directories(easypb_codegen PRIVATE include 3rd-party/popl codegen)

add_library(easypb_schema STATIC codegen/schema.cpp codegen/schema_semantics.cpp codegen/logical_paths.cpp codegen/schema_linker.cpp)
target_include_directories(easypb_schema
    PUBLIC include codegen)

if(EASYPB_CODEGEN_WITH_PROTO_PARSER)
    add_library(easypb_proto_parser STATIC codegen/parser/proto_parser.cpp)
    target_include_directories(easypb_proto_parser
        PUBLIC include codegen codegen/parser)
    target_link_libraries(easypb_proto_parser PUBLIC easypb_schema)

    add_library(easypb_proto_loader STATIC codegen/proto_loader.cpp codegen/file_paths.cpp)
    target_include_directories(easypb_proto_loader
        PUBLIC include codegen codegen/parser)
    target_link_libraries(easypb_proto_loader PUBLIC easypb_proto_parser easypb_schema)

    target_sources(easypb_codegen PRIVATE
        codegen/parser/pretty_printer.cpp
        codegen/parser/parser_benchmark.cpp)
    target_link_libraries(easypb_codegen PRIVATE easypb_proto_parser easypb_schema)

    target_compile_definitions(easypb_codegen PRIVATE
        EASYPB_CODEGEN_WITH_PROTO_PARSER=1)
else()
    target_link_libraries(easypb_codegen PRIVATE easypb_schema)
    target_compile_definitions(easypb_codegen PRIVATE
        EASYPB_CODEGEN_WITH_PROTO_PARSER=0)
endif()

if(BUILD_TESTING OR EASYPB_BUILD_DIFFERENTIAL_TESTS)
    add_subdirectory(tests)
endif()
