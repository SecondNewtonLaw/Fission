cmake_minimum_required(VERSION 3.31)
project(Fission.BinaryNinja CXX)

# Binary Ninja install and API checkout. The API revision must match the installed
# core, otherwise the plugin is rejected at load time.
if(WIN32)
    set(_bn_install_default "$ENV{LOCALAPPDATA}/Programs/Vector35/BinaryNinja")
    set(_bn_plugins_default "$ENV{APPDATA}/Binary Ninja/plugins")
elseif(APPLE)
    set(_bn_install_default "/Applications/Binary Ninja.app/Contents/MacOS")
    set(_bn_plugins_default "$ENV{HOME}/Library/Application Support/Binary Ninja/plugins")
else()
    set(_bn_install_default "$ENV{HOME}/binaryninja")
    set(_bn_plugins_default "$ENV{HOME}/.binaryninja/plugins")
endif()

set(BN_INSTALL_DIR "${_bn_install_default}" CACHE PATH "Binary Ninja installation directory")
set(BN_USER_PLUGINS_DIR "${_bn_plugins_default}" CACHE PATH "Binary Ninja user plugin directory")
set(BN_API_PATH "" CACHE PATH "Local binaryninja-api checkout matching the installed Binary Ninja")
set(BN_API_REVISION "775c3b5eed8598217b20e1087afe7aa092a9b4b1" CACHE STRING
    "binaryninja-api revision matching BN_INSTALL_DIR/api_REVISION.txt")

if(NOT EXISTS "${BN_INSTALL_DIR}")
    message(FATAL_ERROR "Binary Ninja installation not found at '${BN_INSTALL_DIR}'. Set BN_INSTALL_DIR.")
endif()

# UI headers are not required for an architecture/view plugin, and the headless
# build keeps the dependency surface small.
set(HEADLESS ON CACHE BOOL "" FORCE)

if(BN_API_PATH AND EXISTS "${BN_API_PATH}/CMakeLists.txt")
    message(STATUS "Using local binaryninja-api checkout: ${BN_API_PATH}")
    add_subdirectory("${BN_API_PATH}" "${CMAKE_BINARY_DIR}/binaryninja-api" EXCLUDE_FROM_ALL)
else()
    CPMAddPackage(
        NAME binaryninjaapi
        GITHUB_REPOSITORY Vector35/binaryninja-api
        GIT_TAG ${BN_API_REVISION}
        EXCLUDE_FROM_ALL YES
    )
endif()

# Small compiler driver used to build realistic test samples. It is deliberately
# independent of Binary Ninja so it can run in CI or on a developer machine.
add_executable(Fission.LuauCompile tools/LuauCompile.cpp)
target_link_libraries(Fission.LuauCompile PRIVATE Luau.Compiler Luau.Ast)
target_compile_features(Fission.LuauCompile PRIVATE cxx_std_23)
set_target_properties(Fission.LuauCompile PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/bin")

add_executable(Fission.LuauRun tools/LuauRun.cpp)
target_link_libraries(Fission.LuauRun PRIVATE Luau.VM)
target_compile_features(Fission.LuauRun PRIVATE cxx_std_23)
set_target_properties(Fission.LuauRun PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/bin")

# Bytecode parsing/image layer, independent of Binary Ninja so it can be tested
# in CI and from the command line.
add_library(Fission.BinaryNinja.Core STATIC
    src/Image.cpp
    src/Intrinsics.cpp
    include/Image.hpp
    include/Intrinsics.hpp
)
target_include_directories(Fission.BinaryNinja.Core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(Fission.BinaryNinja.Core PUBLIC Fission.Decompiler)
target_compile_features(Fission.BinaryNinja.Core PUBLIC cxx_std_23)

add_executable(Fission.BinaryNinja.Verify tools/Verify.cpp)
target_link_libraries(Fission.BinaryNinja.Verify PRIVATE Fission.BinaryNinja.Core)
target_compile_features(Fission.BinaryNinja.Verify PRIVATE cxx_std_23)
set_target_properties(Fission.BinaryNinja.Verify PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/bin")

# Per-proto decompilation probe. Kept out of the default build; used to debug
# function-level output when a nested proto is decompiled in isolation.
add_executable(Fission.BinaryNinja.DumpProto EXCLUDE_FROM_ALL tools/DumpProto.cpp)
target_link_libraries(Fission.BinaryNinja.DumpProto PRIVATE Fission.BinaryNinja.Core)
target_compile_features(Fission.BinaryNinja.DumpProto PRIVATE cxx_std_23)
set_target_properties(Fission.BinaryNinja.DumpProto PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/bin")

add_custom_target(Fission.BinaryNinja.Check
    COMMAND "$<TARGET_FILE:Fission.BinaryNinja.Verify>" "${CMAKE_CURRENT_SOURCE_DIR}/samples/kitchen_sink.luac"
    COMMAND "$<TARGET_FILE:Fission.BinaryNinja.Verify>" "${CMAKE_CURRENT_SOURCE_DIR}/samples/kitchen_sink_O0.luac"
    COMMAND "$<TARGET_FILE:Fission.BinaryNinja.Verify>" "${CMAKE_CURRENT_SOURCE_DIR}/samples/stress.luac"
    COMMAND "$<TARGET_FILE:Fission.BinaryNinja.Verify>" "${CMAKE_CURRENT_SOURCE_DIR}/samples/stress_O0.luac"
    COMMAND "$<TARGET_FILE:Fission.BinaryNinja.Verify>" "${CMAKE_CURRENT_SOURCE_DIR}/samples/renamers.luac"
    DEPENDS Fission.BinaryNinja.Verify
    COMMENT "Verifying checked-in Luau bytecode fixtures"
    VERBATIM)

add_library(Fission.BinaryNinja SHARED
    src/Architecture.cpp
    src/Plugin.cpp
    src/View.cpp
    include/Architecture.hpp
    include/View.hpp
    README.md
)
target_include_directories(Fission.BinaryNinja PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(Fission.BinaryNinja PRIVATE Fission.BinaryNinja.Core binaryninjaapi)
target_compile_features(Fission.BinaryNinja PRIVATE cxx_std_23)
if(WIN32)
    target_compile_definitions(Fission.BinaryNinja PRIVATE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
set_target_properties(Fission.BinaryNinja PROPERTIES
    PREFIX ""
    OUTPUT_NAME "Fission.BinaryNinja"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Install automatically on build so a developer does not have to copy the DLL by
# hand after every iteration.
add_custom_command(TARGET Fission.BinaryNinja POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BN_USER_PLUGINS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Fission.BinaryNinja>" "${BN_USER_PLUGINS_DIR}/"
    COMMENT "Installing $<TARGET_FILE_NAME:Fission.BinaryNinja> -> ${BN_USER_PLUGINS_DIR}"
    VERBATIM)

install(TARGETS Fission.BinaryNinja
    RUNTIME DESTINATION "${BN_USER_PLUGINS_DIR}"
    LIBRARY DESTINATION "${BN_USER_PLUGINS_DIR}")
