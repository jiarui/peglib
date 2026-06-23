cmake_minimum_required(VERSION 3.22)

project(peglib
    VERSION 0.1.0
    DESCRIPTION "Header-only Parsing Expression Grammar (PEG) library in C++20"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_EXPORT_COMPILE_COMMANDS)
    # Available for clangd / IDE integration.
endif()

# ---------------------------------------------------------------------------
# Library target (header-only / INTERFACE)
# ---------------------------------------------------------------------------
add_library(peglib INTERFACE)
target_include_directories(peglib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_features(peglib INTERFACE cxx_std_20)

# MSVC treats the standard C runtime functions (fopen, etc.) as deprecated
# and escalates the C4996 warning to an error under /WX. _CRT_SECURE_NO_WARNINGS
# silences these for consumers of the header-only library (FileSource uses
# std::fopen, which is portable and correct). The define is a no-op on
# GCC/Clang, so this only affects MSVC builds.
if(MSVC)
    target_compile_definitions(peglib INTERFACE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PEGLIB_BUILD_TESTS "Build peglib unit tests" ON)
option(PEGLIB_BUILD_LUA_EXAMPLES "Build Lua lexer/parser examples (requires doctest only)" ON)
option(PEGLIB_COVERAGE "Enable coverage instrumentation (GCC/Clang)" OFF)
option(PEGLIB_ENABLE_CLANG_TIDY "Run clang-tidy during build" OFF)
option(PEGLIB_ENABLE_SANITIZERS "Enable ASan/UBSan (GCC/Clang)" OFF)

if(PEGLIB_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizers only supported on GCC/Clang")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(PEGLIB_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Coverage only supported on GCC/Clang")
    endif()
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

if(PEGLIB_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE};--quiet")
    else()
        message(WARNING "clang-tidy not found")
    endif()
endif()

# ---------------------------------------------------------------------------
# Testing
# ---------------------------------------------------------------------------
if(PEGLIB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
