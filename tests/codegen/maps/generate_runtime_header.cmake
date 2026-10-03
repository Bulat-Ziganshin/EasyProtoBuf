foreach(required_var CODEGEN SCALAR_INPUT ENUM_INPUT MESSAGE_INPUT OUTPUT)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

# Plan 5 retired the old "one process concatenates three descriptor sets into
# stdout" workflow: several loaded files now require --target, and several
# selected targets require --out-dir. All three containers are named single-
# file sets, so one process selects all three targets and publishes one header
# per target; this script writes the small test-only umbrella header the
# runtime test includes.
execute_process(
    COMMAND "${CODEGEN}" --descriptor-set
            "${SCALAR_INPUT}" "${ENUM_INPUT}" "${MESSAGE_INPUT}"
            --target scalar_maps.proto
            --target enum_map.proto
            --target message_map.proto
            --out-dir "${output_dir}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE generated
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "Map runtime generation failed (${result})\nstdout:\n${generated}\nstderr:\n${error}")
endif()

file(WRITE "${OUTPUT}"
    "#pragma once\n"
    "\n"
    "#include \"scalar_maps.pb.hpp\"\n"
    "#include \"enum_map.pb.hpp\"\n"
    "#include \"message_map.pb.hpp\"\n")
