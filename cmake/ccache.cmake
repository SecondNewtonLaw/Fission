# written by dottik (yes i did write this garbage)

find_program(CCACHE_EXECUTABLE
        NAMES ccache ccache.exe
        HINTS
        "$ENV{ProgramFiles}/ccache"
        "C:/ProgramData/chocolatey/bin" # Chocolatey

        # macOS/ Linux
        "/usr/local/bin"
        "/usr/bin"
        "/opt/homebrew/bin" # homebrew on apple
        "/opt/local/bin"    # macports
)

if (CCACHE_EXECUTABLE)
    message(STATUS "found ccache executable @ ${CCACHE_EXECUTABLE}")

    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_EXECUTABLE}" CACHE STRING "C compiler launcher")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_EXECUTABLE}" CACHE STRING "C++ compiler launcher")

    if (MSVC)
        message(STATUS "Identified Visual C++ compiler. Enabling embedded debugging information for better results with ccache...")
        if (CMAKE_GENERATOR MATCHES "Visual Studio")
            message(WARNING "ccache with the Visual Studio generator may not be fully supported. Consider using the Ninja generator (-G Ninja).")
        endif ()

        # MSVC requires ccache version 4.6 or newer; better results are produced with embedded dbg info.
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded" CACHE STRING "MSVC debug information format")
    endif()

    set(CCACHE_FOUND ON)
else ()
    message(WARNING "ccache not found. Building without compiler caching.")
    message(STATUS "ccache can be easily obtained @ https://ccache.dev/ and you will likely benefit from a hefty speed up in compilation afterward!")
endif ()