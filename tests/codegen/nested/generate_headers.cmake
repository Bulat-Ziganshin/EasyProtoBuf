if(NOT DEFINED CODEGEN OR NOT DEFINED DATA_DIR OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "CODEGEN, DATA_DIR and OUTPUT_DIR are required")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

function(generate input output)
    execute_process(
        COMMAND "${CODEGEN}" --descriptor-set "${input}"
        RESULT_VARIABLE result OUTPUT_VARIABLE generated ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Codegen failed for ${input}:\n${error}")
    endif()
    file(WRITE "${output}" "${generated}")
endfunction()

generate("${DATA_DIR}/nested-messages.pbs" "${OUTPUT_DIR}/nested-messages.generated.hpp")
generate("${DATA_DIR}/nested-forward.pbs" "${OUTPUT_DIR}/nested-forward.generated.hpp")
