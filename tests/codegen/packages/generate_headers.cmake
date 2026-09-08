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

execute_process(
    COMMAND "${CODEGEN}" --descriptor-set --no-class "${DATA_DIR}/names.pbs"
    RESULT_VARIABLE no_class_result
    OUTPUT_FILE "${OUTPUT_DIR}/names.no-class.generated.hpp"
    ERROR_VARIABLE no_class_error)
if(NOT no_class_result EQUAL 0)
    message(FATAL_ERROR "Generating names.no-class.generated.hpp failed: ${no_class_error}")
endif()
