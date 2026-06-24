if (NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif ()

set(LIBASSERT_PATHS_CPP "${SOURCE_DIR}/src/paths.cpp")
file(READ "${LIBASSERT_PATHS_CPP}" LIBASSERT_PATHS_CPP_CONTENT)

if (NOT LIBASSERT_PATHS_CPP_CONTENT MATCHES "#include[ \t]+<algorithm>")
    string(FIND "${LIBASSERT_PATHS_CPP_CONTENT}" "#include \"common.hpp\"\n" INCLUDE_POSITION)
    if (INCLUDE_POSITION EQUAL -1)
        message(FATAL_ERROR "Could not find libassert paths.cpp include insertion point")
    endif ()

    string(REPLACE "#include \"common.hpp\"\n"
            "#include \"common.hpp\"\n#include <algorithm>\n"
            LIBASSERT_PATHS_CPP_CONTENT
            "${LIBASSERT_PATHS_CPP_CONTENT}")
    file(WRITE "${LIBASSERT_PATHS_CPP}" "${LIBASSERT_PATHS_CPP_CONTENT}")
endif ()
