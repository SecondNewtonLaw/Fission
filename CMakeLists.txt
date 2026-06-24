cmake_minimum_required(VERSION 3.31)
project(Fission)

include(CTest)


set(CMAKE_CXX_STANDARD 23)
option(USE_CCACHE "Use ccache as the compiler launcher when available." ON)

option(ALLOW_NON_STANDARD_MATH "Enable non-standard fast-math optimizations." ON)
option(DEBUG_INFORMATION_EXTENSIVE "Emit extensive debug information." ON)
option(DEBUG_NO_INLINING "Disable function inlining for Debug builds." ON)
option(USE_LTO "Enable link-time optimization." OFF)

option(PRODUCTION_BUILD "Remove selected internal information from production builds." OFF)

option(ENABLE_PGO_CLANG_INSTRUMENT "Enable Clang PGO instrumentation." OFF)
option(ENABLE_PGO_CLANG_USE "Enable Clang PGO usage." OFF)

option(ENABLE_ASAN "Enable AddressSanitizer." OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer." OFF)

if (CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    # Be lenient in C for dependency pointer types and function declarations.
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wno-error=incompatible-pointer-types>)
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>)
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wno-deprecated-declarations>)
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wno-unused-parameter>)
    if (CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang")
        add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wno-c2y-extensions>)
    endif ()
endif ()


if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    if (USE_LTO AND (ENABLE_ASAN OR ENABLE_UBSAN))
        message(FATAL_ERROR "You must disable Link Time Optimization to use sanitisers.")
    endif ()

    if (ENABLE_ASAN AND ENABLE_UBSAN)
        add_compile_options(
                -fsanitize=address,undefined
        )
        add_link_options(
                -fsanitize=address,undefined
        )
    elseif (ENABLE_ASAN)
        add_compile_options(
                -fsanitize=address
        )
        add_link_options(
                -fsanitize=address
        )
    elseif (ENABLE_UBSAN)
        add_compile_options(
                -fsanitize=undefined
        )
        add_link_options(
                -fsanitize=undefined
        )
    endif ()
elseif (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
#msvc flags.
    if (ENABLE_UBSAN)
        message(FATAL_ERROR "The Undefined Behaviour Sanitiser is not available on MSVC front-end compilers!")
    endif ()

    if (ENABLE_ASAN)
        add_compile_options(/fsanitize=address)
    endif ()
endif ()

if (USE_CCACHE)
    include(cmake/ccache.cmake)
endif ()

include(cmake/identify_compiler.cmake)
include(cmake/CPM.cmake)
include(CheckCXXCompilerFlag)

function(append_supported_cxx_compile_options out_var)
    foreach (flag IN LISTS ARGN)
        string(MAKE_C_IDENTIFIER "FISSION_CXX_SUPPORTS_${flag}" flag_supported_var)
        check_cxx_compiler_flag("${flag}" "${flag_supported_var}")
        if (${flag_supported_var})
            list(APPEND ${out_var} "${flag}")
        endif ()
    endforeach ()

    set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()

if (CXX_COMPILER_KIND STREQUAL "clang-cl")
#While clang - cl provides better diagnostics, it's still scuffed on some aspects
#and could cause previously compatible with MSVC code to not work anymore.
    MESSAGE(FATAL_ERROR "Clang (with MSVC front-end) is not supported! Please use clang with a GNU front-end, or the Visual C/C++ compiler instead.")
endif ()

CPMAddPackage(
        NAME libassert
        GITHUB_REPOSITORY jeremy-rifkin/libassert
        GIT_TAG v2.2.0
        # libassert v2.2.0 is missing <algorithm> in src/paths.cpp on newer toolchains.
        # Keep this idempotent patch until the dependency release includes that header.
        PATCH_COMMAND ${CMAKE_COMMAND}
            -D "SOURCE_DIR=<SOURCE_DIR>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/libassert-add-algorithm-include.cmake"
        OPTIONS
        "LIBASSERT_USE_MAGIC_ENUM ON"
)

CPMAddPackage(
        NAME Luau
        GITHUB_REPOSITORY luau-lang/luau
        GIT_TAG 0.718
)

set(BOOST_INCLUDE_LIBRARIES beast container CACHE STRING "Boost libraries required by Fission")

CPMAddPackage(
        NAME Boost
        VERSION 1.86.0 # Versions less than 1.85.0 may need patches for installation targets.
        URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-cmake.tar.xz
        URL_HASH SHA256=2c5ec5edcdff47ff55e27ed9560b0a0b94b07bd07ed9928b476150e16b0efc57
        OPTIONS "BOOST_ENABLE_CMAKE ON" "BOOST_SKIP_INSTALL_RULES ON" # Set `OFF` for installation
        "BUILD_SHARED_LIBS OFF"
)

if (TARGET boost_asio)
    target_compile_definitions(boost_asio INTERFACE BOOST_ASIO_DISABLE_BOOST_COROUTINE)
    get_target_property(BOOST_ASIO_LINK_LIBRARIES boost_asio INTERFACE_LINK_LIBRARIES)
    if (BOOST_ASIO_LINK_LIBRARIES)
        list(REMOVE_ITEM BOOST_ASIO_LINK_LIBRARIES Boost::context Boost::coroutine)
        set_target_properties(boost_asio PROPERTIES INTERFACE_LINK_LIBRARIES "${BOOST_ASIO_LINK_LIBRARIES}")
    endif ()
endif ()

set_target_properties(
        Luau.VM
        Luau.VM.Internals
        Luau.Compiler
        Luau.CodeGen
        Luau.Ast
        PROPERTIES
        INTERFACE_SYSTEM ON
)

if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
#gnu flags.
    set(CXX_COMPILE_OPTIONS -Werror -Wall -Wextra -Wpedantic -pedantic -funroll-loops -fstack-protector-strong -fstack-protector-all)
    set(CXX_LINK_OPTIONS)
    append_supported_cxx_compile_options(CXX_COMPILE_OPTIONS -ftree-vectorize)

    if (CXX_COMPILER_KIND STREQUAL "clang")
        append_supported_cxx_compile_options(CXX_COMPILE_OPTIONS -Wmicrosoft -fslp-vectorize -fvectorize)
    endif ()

    if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        list(APPEND CXX_COMPILE_OPTIONS -mavx)
    endif ()

    if (WIN32 AND CXX_TOOLCHAIN STREQUAL "mingw")
        list(APPEND CXX_COMPILE_OPTIONS -static)
        list(APPEND CXX_LINK_OPTIONS -static)
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND CXX_LINK_OPTIONS -static-libgcc -static-libstdc++)
    endif ()

    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND CXX_COMPILE_OPTIONS -Og)
        if (DEBUG_NO_INLINING)
            list(APPEND CXX_COMPILE_OPTIONS -fno-inline-functions)
        endif ()
    endif ()

    if (USE_LTO)
        if (CXX_COMPILER_KIND STREQUAL "clang")
            list(APPEND CXX_COMPILE_OPTIONS -flto=full -fwhole-program-vtables)
            list(APPEND CXX_LINK_OPTIONS -flto=full -fwhole-program-vtables)
        else ()
            list(APPEND CXX_COMPILE_OPTIONS -flto)
            list(APPEND CXX_LINK_OPTIONS -flto)
        endif ()
    endif ()

    if (ALLOW_NON_STANDARD_MATH)
        list(APPEND CXX_COMPILE_OPTIONS -ffast-math)
    endif ()

    if (DEBUG_INFORMATION_EXTENSIVE)
        list(APPEND CXX_COMPILE_OPTIONS -g3)
        list(APPEND CXX_LINK_OPTIONS -g3)
    endif ()

    if (ENABLE_PGO_CLANG_INSTRUMENT AND ENABLE_PGO_CLANG_USE)
        message(WARNING "PGO_INSTRUMENT and PGO_USE both are as ON, nothing will happen.")
    endif ()

    if (ENABLE_PGO_CLANG_INSTRUMENT AND NOT ENABLE_PGO_CLANG_USE)
        list(APPEND CXX_COMPILE_OPTIONS -fprofile-generate)
        list(APPEND CXX_LINK_OPTIONS -fprofile-generate)
    endif ()

    if (NOT ENABLE_PGO_CLANG_INSTRUMENT AND ENABLE_PGO_CLANG_USE)
        list(APPEND CXX_COMPILE_OPTIONS -fprofile-use=./rbxcli.profdata)
        list(APPEND CXX_LINK_OPTIONS -fprofile-use=./rbxcli.profdata)
    endif ()

elseif (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
#msvc flags.
    set(CXX_COMPILE_OPTIONS /arch:AVX2 /WX /W4 /permissive- /Zc:preprocessor /Ob3 /O2 /Ot
            /wd4127 #[==[ conditional expression is constant ]==]
            /wd4068 #[==[ unknown pragma '...' ]==]
            /wd4324 #[==[ THIS IS A REALLY STUPID WARNING, MSVC WHY? ]==]
            /wd4820 #[==[ THIS IS A REALLY STUPID WARNING, MSVC WHY? ]==]
            /wd5045 #[==[ Compiler will insert Spectre mitigation for memory load if /Qspectre switch specified -- Why the fuck would you warn for this? ]==]
            /wd4365 #[==[ 'argument': conversion from '...' to '...', signed/unsigned mismatch ]==]
            /wd4244 #[==[ conversion from '...' to '...', possible loss of data ]==]
            /wd4267 #[==[ conversion from 'size_t' to '...', possible loss of data ]==]
            /wd5219 #[==[ implicit conversion from '...' to '...', possible loss of data ]==]
            /wd5039 #[==[ pointer or reference to potentially throwing function passed to 'extern "C"' function under -EHc. Undefined behavior may occur if this function throws an exception. ]==]
            /wd4625 #[==[ blah blah copy implcitly deleted blah blah ]==]
            /wd5026 #[==[ blah blah copy implcitly deleted blah blah ]==]
            /wd4626 #[==[ blah blah copy implcitly deleted blah blah ]==]
            /wd5027 #[==[ blah blah copy implcitly deleted blah blah ]==]
            /wd5267 #[==[ definition of implicit copy constructor for '...' is deprecated because it has a user-provided destructor ]==]
            /wd4702 #[==[ unreachable code (it is not.) ]==]
            /wd4668 #[==[ '...' is not defined as a preprocessor macro, replacing with '0' for '#if/#elif' ]==]
            /wd4464 #[==[ relative include path contains '..' ]==]
    )

    if (USE_LTO)
        list(APPEND CXX_LINK_OPTIONS /LTCG /GL)
    endif ()

    if (DEBUG_INFORMATION_EXTENSIVE)
        list(APPEND CXX_COMPILE_OPTIONS /Z7)
        list(APPEND CXX_LINK_OPTIONS /Z7)
    endif ()

endif ()

add_library(Fission.Common STATIC
        Fission.Common/src/InstructionDecoder.cpp

        Fission.Common/include/InstructionDecoder.hpp
        Fission.Common/include/BinaryReader.hpp
)
target_include_directories(Fission.Common PUBLIC Fission.Common/include)
#target_precompile_headers(Fission.Common PUBLIC Fission.Common / include / pch.hpp)
target_compile_options(Fission.Common PRIVATE ${CXX_COMPILE_OPTIONS})
target_link_options(Fission.Common PRIVATE ${CXX_LINK_OPTIONS})

add_library(Fission.Decompiler STATIC
        Fission.Decompiler/src/Deserializer.cpp
        Fission.Decompiler/include/Deserializer.hpp
        Fission.Decompiler/src/BytecodeLifter.cpp
        Fission.Decompiler/include/ControlFlowAnalyzer.hpp
        Fission.Decompiler/src/ControlFlowAnalyzer.cpp
        Fission.Decompiler/include/DenominatorAnalysis.hpp
        Fission.Decompiler/src/DenominatorAnalysis.cpp
        Fission.Decompiler/include/SSABuilder.hpp
        Fission.Decompiler/src/SSABuilder.cpp
        Fission.Decompiler/include/Analysis/RobloxTypeInferer.hpp
        Fission.Decompiler/src/Analysis/RobloxTypeInferer.cpp
        Fission.Decompiler/src/Decompiler.cpp
        Fission.Decompiler/include/SafetyGuard.hpp
        Fission.Decompiler/include/SourceGenerator/Generator.hpp
        Fission.Decompiler/include/Rewriters/ASTRewriter.hpp
        Fission.Decompiler/include/Rewriters/IfChainSimplifier.hpp
        Fission.Decompiler/include/Rewriters/ShortCircuitFolder.hpp
        Fission.Decompiler/include/AbstractSyntaxTree/ASTNode.hpp
        Fission.Decompiler/include/AbstractSyntaxTree/Nodes/RootNode.hpp
        Fission.Decompiler/include/AbstractSyntaxTree/Visitor.hpp
        Fission.Decompiler/include/AbstractSyntaxTree/Nodes/CommentNode.hpp
        Fission.Decompiler/src/ASTLifter.cpp
)

target_include_directories(Fission.Decompiler PUBLIC Fission.Decompiler/include)
target_compile_options(Fission.Decompiler PRIVATE ${CXX_COMPILE_OPTIONS})
target_link_options(Fission.Decompiler PRIVATE ${CXX_LINK_OPTIONS})
target_link_libraries(Fission.Decompiler PUBLIC Fission.Common libassert::assert Luau.Ast Luau.Compiler Luau.VM Luau.VM.Internals Boost::container)

add_executable(Fission.CLI
        Fission.CLI/src/main.cpp
        Fission.Decompiler/include/BytecodeLifter.hpp
        Fission.Decompiler/include/Decompiler.hpp
        Fission.Decompiler/include/ASTLifter.hpp
)
target_link_libraries(Fission.CLI PUBLIC Fission.Decompiler)
target_compile_options(Fission.CLI PRIVATE ${CXX_COMPILE_OPTIONS})
target_link_options(Fission.CLI PRIVATE ${CXX_LINK_OPTIONS})

add_executable(Fission.Server
        Fission.Server/src/main.cpp
)
target_link_libraries(Fission.Server PUBLIC Fission.Decompiler Boost::beast)
if (WIN32)
    target_link_libraries(Fission.Server PRIVATE ws2_32)
    target_compile_definitions(Fission.Server PRIVATE _WIN32_WINNT=0x0601)
endif ()
target_compile_options(Fission.Server PRIVATE ${CXX_COMPILE_OPTIONS})
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Fission.Server PRIVATE -Wno-error=deprecated-literal-operator)
    if (WIN32)
        target_compile_options(Fission.Server PRIVATE
                -Wno-error=language-extension-token
                -Wno-error=gnu-anonymous-struct
                -Wno-error=nested-anon-types
                -Wno-error=microsoft-cpp-macro
        )
    endif ()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(Fission.Server PRIVATE /wd5311)
endif ()
target_link_options(Fission.Server PRIVATE ${CXX_LINK_OPTIONS})

if (BUILD_TESTING)
    CPMAddPackage(
            NAME Catch2
            GITHUB_REPOSITORY catchorg/Catch2
            GIT_TAG v3.14.0
    )

    add_executable(Fission.Tests
            Fission.Tests/TestLibassertHandler.cpp
            Fission.Tests/Decoding/BytecodeV6Decoding.cpp
            Fission.Tests/CFG/CFGCoherence.cpp
            Fission.Tests/Decompiler/DecompilationRoundtrip.cpp
            Fission.Tests/Decompiler/LiftingSemantics.cpp
            Fission.Tests/Decompiler/ControlFlowRegressions.cpp
            Fission.Tests/Decompiler/ComplexControlFlow.cpp
            Fission.Tests/Decompiler/TypeInference.cpp
    )

    target_link_libraries(Fission.Tests PRIVATE
            Fission.Common
            Fission.Decompiler
            Catch2::Catch2WithMain
            Catch2
    )

    target_compile_options(Fission.Tests PRIVATE ${CXX_COMPILE_OPTIONS})
    target_link_options(Fission.Tests PRIVATE ${CXX_LINK_OPTIONS})

    list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")

    include(Catch)
    catch_discover_tests(Fission.Tests)
endif()
