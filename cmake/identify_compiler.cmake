# made by dottik again, wow, who would've thought?

message(STATUS "Attempting to guess the toolchain and the compiler...")

if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # MSVC front-end.
    set(CXX_TOOLCHAIN "win32")

    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(CXX_COMPILER_KIND "clang-cl")
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(CXX_COMPILER_KIND "msvc")
    else ()
        message(FATAL_ERROR "Unsupported compiler with an MSVC-like front-end; either support this case, or change to a supported flavor of MSVC front-end compilers.")
    endif ()
else ()
    # GNU front-end
    if (WIN32)
        # MinGW, since no fucking way lmao
        set(CXX_TOOLCHAIN "mingw")
    else ()
        set(CXX_TOOLCHAIN "posix")
    endif ()

    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(CXX_COMPILER_KIND "clang")
        if (WIN32)
            set(CXX_TOOLCHAIN "win32") # clang most likely runs with the MSVC ABI properly.
        endif ()
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CXX_COMPILER_KIND "gcc")
    else ()
        message(FATAL_ERROR "Unsupported compiler with an GNU-like front-end; either support this case, or change to a supported flavor of GNU front-end compilers.")
    endif ()
endif ()

message(STATUS "Determined compiler kind to be: ${CXX_COMPILER_KIND} with ABI ==> ${CXX_TOOLCHAIN}")
#[[
    This simple script detects the compiler between the most normal ones (and the ones that we will use for this project most likely)

    It sets a variable called CXX_COMPILER_KIND, which holds the name of the compiler in lower case.

    - Microsoft's Visual C/C++ Compiler ('msvc')
    - LLVM's Clang (with Visual C/C++ front-end) ('clang-cl')
    - LLVM's Clang (with GNU front-end) ('clang')
    - Gnu Compiler Collection's C compiler ('gcc')

    This also sets a CXX_TOOLCHAIN kind.
    - mingw (Cywin bs)
    - posix (Linux, because I really didn't get too creative)
    - win32 (Windows, native abi)
]]
#