# Plan 5's runtime-suite generation step. One standalone process per target
# publishes `common/state.pb.hpp`, `common/node.pb.hpp` and `api/report.pb.hpp`
# into OUTPUT_DIR: independently generated root headers, not one batch, so no
# process may depend on what another generated. Under FULL_BUILD the source
# frontend must produce the byte-identical header for every target (the
# plan's source-versus-descriptor equality, with dependency includes,
# qualified names and defaults all in place).
#
# Required arguments:
#   CODEGEN     the standalone codegen executable
#   PBS         the committed complete descriptor set (project.pbs)
#   SOURCE_ROOT the committed .proto root (codegen/imports/data/project)
#   OUTPUT_DIR  the generated-header directory below the build tree
#   FULL_BUILD  1 when the proto parser is available

foreach(required_var CODEGEN PBS SOURCE_ROOT OUTPUT_DIR)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()
if(NOT DEFINED FULL_BUILD)
    set(FULL_BUILD 1)
endif()

set(targets
    common/state.proto
    common/node.proto
    api/report.proto)

file(MAKE_DIRECTORY "${OUTPUT_DIR}")

foreach(target ${targets})
    get_filename_component(target_dir "${target}" DIRECTORY)
    file(MAKE_DIRECTORY "${OUTPUT_DIR}/${target_dir}")
    execute_process(
        COMMAND "${CODEGEN}" --descriptor-set "${PBS}" --target "${target}"
                --out-dir "${OUTPUT_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE generated
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "Runtime header generation failed for ${target} (${result})\n"
            "stdout:\n${generated}\nstderr:\n${error}")
    endif()
    if(NOT "${generated}" STREQUAL "")
        message(FATAL_ERROR
            "Runtime header generation wrote to stdout for ${target}")
    endif()
endforeach()

if(FULL_BUILD)
    # The same headers through the source frontend, one process per target,
    # byte-identical: only the input origin differs.
    foreach(target ${targets})
        execute_process(
            COMMAND "${CODEGEN}" -I "${SOURCE_ROOT}"
                    "${SOURCE_ROOT}/${target}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE source_out
            ERROR_VARIABLE error)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR
                "Source runtime header generation failed for ${target} "
                "(${result})\nstderr:\n${error}")
        endif()
        # The logical-to-header mapping: a final `.proto` becomes `.pb.hpp`.
        string(REGEX REPLACE "[.]proto$" ".pb.hpp" header "${target}")
        file(READ "${OUTPUT_DIR}/${header}" descriptor_out)
        if(NOT source_out STREQUAL descriptor_out)
            message(FATAL_ERROR
                "Source and .pbs runtime headers differ for ${target}")
        endif()
    endforeach()
endif()

# The custom-command OUTPUT: the last generated header. The two dependency
# headers are BYPRODUCTS in tests/CMakeLists.txt.
