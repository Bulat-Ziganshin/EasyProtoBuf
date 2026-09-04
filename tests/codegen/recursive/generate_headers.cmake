if(NOT DEFINED CODEGEN OR NOT DEFINED DATA_DIR OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "CODEGEN, DATA_DIR and OUTPUT_DIR are required")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")

function(generate_recursive_header input output)
    execute_process(
        COMMAND ${CODEGEN}
            --descriptor-set
            --allow-self-recursive-containers
            ${input}
        RESULT_VARIABLE result
        OUTPUT_FILE ${output}
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "Codegen failed (${result}) for ${input}:\n${error}")
    endif()
endfunction()

generate_recursive_header(
    "${DATA_DIR}/nested-recursive-repeated.pbs"
    "${OUTPUT_DIR}/recursive-repeated.generated.hpp")
generate_recursive_header(
    "${DATA_DIR}/nested-recursive-map.pbs"
    "${OUTPUT_DIR}/recursive-map.generated.hpp")
