cmake_minimum_required(VERSION 3.7...3.28)

project(EasyProtoBuf VERSION 0.0
                     DESCRIPTION "Single-header C++11 ProtoBuf library"
                     LANGUAGES CXX)
include(CTest)


add_executable(decoder examples/decoder/decoder.cpp)
include_directories(decoder PRIVATE include)

add_executable(tutorial examples/tutorial/main.cpp)
target_include_directories(tutorial PRIVATE include)

add_executable(codegen codegen/main.cpp)
target_include_directories(codegen PRIVATE include 3rd-party/popl)


add_test(NAME compare.decoded.data.with.original
         COMMAND tutorial)


# EASYPB_CXX_FLAGS is the special definition used by my CI script
# to explicitly specify extra compiler options (e.g. C++17 or C++11).
# If it's not specified, we just use the compiler settings
# recommended for each executable.
if(EASYPB_CXX_FLAGS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${EASYPB_CXX_FLAGS}")
else()
    set(CMAKE_CXX_STANDARD 11)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    target_compile_features(codegen PRIVATE cxx_std_17)
endif()

