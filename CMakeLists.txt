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
