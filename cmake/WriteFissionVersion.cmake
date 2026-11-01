# Regenerate FissionVersion.hpp during builds. Run via cmake -P from FissionVersionHeader.
if (NOT DEFINED FISSION_SOURCE_DIR OR NOT DEFINED FISSION_VERSION_OVERRIDE_FILE OR NOT DEFINED FISSION_OUTPUT)
    message(FATAL_ERROR "WriteFissionVersion.cmake requires FISSION_SOURCE_DIR, FISSION_VERSION_OVERRIDE_FILE, and FISSION_OUTPUT")
endif ()

set(version "")
if (EXISTS "${FISSION_VERSION_OVERRIDE_FILE}")
    file(READ "${FISSION_VERSION_OVERRIDE_FILE}" version)
endif ()

if (version STREQUAL "")
    if (FISSION_GIT_EXECUTABLE AND EXISTS "${FISSION_GIT_EXECUTABLE}")
        # Only use git when the discovered worktree is this source tree, not a parent repository.
        execute_process(
                COMMAND "${FISSION_GIT_EXECUTABLE}" rev-parse --show-toplevel
                WORKING_DIRECTORY "${FISSION_SOURCE_DIR}"
                OUTPUT_VARIABLE git_toplevel
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE toplevel_result
                ERROR_QUIET
        )
        if (toplevel_result EQUAL 0 AND NOT "${git_toplevel}" STREQUAL "")
            file(REAL_PATH "${FISSION_SOURCE_DIR}" source_real)
            file(REAL_PATH "${git_toplevel}" toplevel_real)
            if (source_real STREQUAL toplevel_real)
                execute_process(
                        COMMAND "${FISSION_GIT_EXECUTABLE}" describe --tags --always --dirty
                        WORKING_DIRECTORY "${FISSION_SOURCE_DIR}"
                        OUTPUT_VARIABLE describe_result_text
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE describe_result
                        ERROR_QUIET
                )
                if (describe_result EQUAL 0 AND NOT "${describe_result_text}" STREQUAL "")
                    set(version "${describe_result_text}")
                endif ()
            endif ()
        endif ()
    endif ()
    if (version STREQUAL "")
        set(version "0.0.0-dev")
    endif ()
endif ()

string(REPLACE "\\" "\\\\" FISSION_VERSION_ESCAPED "${version}")
string(REPLACE "\"" "\\\"" FISSION_VERSION_ESCAPED "${FISSION_VERSION_ESCAPED}")
get_filename_component(output_dir "${FISSION_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
set(temporary "${FISSION_OUTPUT}.tmp")
configure_file("${FISSION_SOURCE_DIR}/cmake/FissionVersion.hpp.in" "${temporary}" @ONLY)
execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${temporary}" "${FISSION_OUTPUT}"
        RESULT_VARIABLE copy_result
)
file(REMOVE "${temporary}")
if (NOT copy_result EQUAL 0)
    message(FATAL_ERROR "Failed to update ${FISSION_OUTPUT}")
endif ()
