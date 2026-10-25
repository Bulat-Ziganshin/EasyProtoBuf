# The accepted CR-36 xfail (triage register E7).
#
# A physical `--out-dir` deeper than 100 components is refused, although every
# directory already exists and the output name below it is shallow. The bound
# belongs to the logical output name (`header_paths` already enforces it
# there); applying it to the caller's own root is the accepted edge-case
# limitation, and this test asserts the CURRENT direction. It passes while the
# limitation is present and fails when the bound moves out of the publisher.
#
# A failure here is not "the product broke": it means a deep physical root is
# now accepted. Re-evaluate the E7 exception in property-catalogue.md, the
# CR-36 row and `codegen.limits`, then retire or invert this test. The failure
# message says so, because the next reader will not have the triage in front of
# them.
#
# The tree is built with the file system's long-path handling: CMake's
# file(MAKE_DIRECTORY) reaches beyond MAX_PATH on Windows (measured at 310
# characters) and POSIX has no such limit. The control is a deep-but-accepted
# root (60 components) that must still publish the header, so a failure of the
# refused row cannot be blamed on the scratch tree or the fixture.
#
# Required arguments:
#   CODEGEN   the standalone codegen executable
#   DATA_DIR  the limits fixture directory
#   WORK_DIR  the scratch directory

foreach(required_var CODEGEN DATA_DIR WORK_DIR)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

function(require_contains variable_name expected description)
    string(FIND "${${variable_name}}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${description}: missing '${expected}' in ${variable_name}")
    endif()
endfunction()

function(require_empty variable_name description)
    if(NOT "${${variable_name}}" STREQUAL "")
        message(FATAL_ERROR "${description}: expected empty output, got '${${variable_name}}'")
    endif()
endfunction()

function(make_deep_root root component_count output_var)
    set(path "${root}")
    math(EXPR last "${component_count} - 1")
    foreach(index RANGE ${last})
        string(APPEND path "/d")
    endforeach()
    file(MAKE_DIRECTORY "${path}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR
            "the scratch tree could not be created: ${path}")
    endif()
    set(${output_var} "${path}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(fixture "${DATA_DIR}/nesting-31.pbs")
if(NOT EXISTS "${fixture}")
    message(FATAL_ERROR "the fixture is missing: ${fixture}")
endif()

# --- the control: a deep but accepted physical root -------------------------

make_deep_root("${WORK_DIR}/accepted" 60 accepted_root)
set(accepted_header "${accepted_root}/nesting-31.pb.hpp")
file(REMOVE "${accepted_header}")

execute_process(
    COMMAND ${CODEGEN} --descriptor-set --out-dir "${accepted_root}" "${fixture}"
    RESULT_VARIABLE accepted_result
    OUTPUT_VARIABLE accepted_output
    ERROR_VARIABLE accepted_error)
if(NOT accepted_result EQUAL 0)
    message(FATAL_ERROR
        "the 60-component control root must be accepted (exit ${accepted_result}).\n"
        "stdout:\n${accepted_output}\nstderr:\n${accepted_error}")
endif()
require_empty(accepted_output "the control root's stdout")
require_empty(accepted_error "the control root's stderr")
if(NOT EXISTS "${accepted_header}")
    message(FATAL_ERROR
        "the control root did not receive nesting-31.pb.hpp")
endif()
file(REMOVE_RECURSE "${accepted_root}")

# --- the limitation: a physical root over the 100-component bound -----------

make_deep_root("${WORK_DIR}/refused" 110 refused_root)
set(refused_header "${refused_root}/nesting-31.pb.hpp")
file(REMOVE "${refused_header}")

execute_process(
    COMMAND ${CODEGEN} --descriptor-set --out-dir "${refused_root}" "${fixture}"
    RESULT_VARIABLE refused_result
    OUTPUT_VARIABLE refused_output
    ERROR_VARIABLE refused_error)
if(refused_result EQUAL 0)
    message(FATAL_ERROR
        "CR-36/E7 changed direction: a 110-component physical --out-dir is now ACCEPTED.\n"
        "The bound moved out of the publisher, so the accepted exception, the\n"
        "CR-36 row and this xfail must be re-evaluated (property-catalogue.md E7).")
endif()
require_empty(refused_output "the refused root's stdout")
require_contains(refused_error "components, more than the 100 this tool accepts"
    "the refused root names the component bound")
require_contains(refused_error "${refused_root}: cannot create directory"
    "the refused root is labeled once with its spelling")
if(EXISTS "${refused_header}")
    message(FATAL_ERROR
        "the refused run wrote ${refused_header}")
endif()

file(REMOVE_RECURSE "${refused_root}")
