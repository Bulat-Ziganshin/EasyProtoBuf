cmake_minimum_required(VERSION 3.10)
if(NOT DEFINED CODEGEN OR NOT DEFINED DATA_DIR OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "CODEGEN, DATA_DIR and OUTPUT_DIR are required")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
foreach(name names alpha beta external shadow module-package import-package)
    execute_process(
        COMMAND "${CODEGEN}" --descriptor-set "${DATA_DIR}/${name}.pbs"
        RESULT_VARIABLE result
        OUTPUT_FILE "${OUTPUT_DIR}/${name}.generated.hpp"
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Generating ${name}.generated.hpp failed: ${error}")
    endif()
endforeach()

# W-68: a compatibility-mode header names its direct dependencies, so the
# thin `external.pbs` root includes "other.pb.hpp". The dependency is a real
# generated header here, not a hand-written stand-in: `other.pbs` is the same
# `other.proto` fixture compiled alone, which is exactly what a consumer of
# `external.generated.hpp` has to provide.
execute_process(
    COMMAND "${CODEGEN}" --descriptor-set "${DATA_DIR}/other.pbs"
    RESULT_VARIABLE other_result
    OUTPUT_FILE "${OUTPUT_DIR}/other.pb.hpp"
    ERROR_VARIABLE other_error)
if(NOT other_result EQUAL 0)
    message(FATAL_ERROR "Generating other.pb.hpp failed: ${other_error}")
endif()

execute_process(
    COMMAND "${CODEGEN}" --descriptor-set --no-class "${DATA_DIR}/names.pbs"
    RESULT_VARIABLE no_class_result
    OUTPUT_FILE "${OUTPUT_DIR}/names.no-class.generated.hpp"
    ERROR_VARIABLE no_class_error)
if(NOT no_class_result EQUAL 0)
    message(FATAL_ERROR "Generating names.no-class.generated.hpp failed: ${no_class_error}")
endif()
