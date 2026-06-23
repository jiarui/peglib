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
        # clang-tidy is scoped to a single TU that includes the umbrella
        # header (peglib.h), which pulls in every library header. This avoids
        # running tidy on test code, where doctest's macros produce
        # false-positives (bugprone-unchecked-optional-access can't model
        # REQUIRE) and version-specific noise (__COUNTER__ flagged as a C2y
        # extension on clang 22+). The library headers are the public API —
        # that's where tidy catches real bugs.
        #
        # -Wno-#warnings suppresses a #warning from GCC 16's <ciso646> header
        # (removed in C++20) that would abort tidy on bleeding-edge libstdc++.
        # It's harmless on CI's clang-22. No -Wno-c2y-extensions is needed
        # because the tidy TU doesn't include doctest.
        set(peglib_clang_tidy
            "${CLANG_TIDY_EXE};--quiet;--extra-arg=-Wno-#warnings")

        add_executable(peglib_tidy_check "${CMAKE_CURRENT_SOURCE_DIR}/test/tidy_check.cpp")
        target_link_libraries(peglib_tidy_check PRIVATE peglib)
        set_target_properties(peglib_tidy_check PROPERTIES
            CXX_CLANG_TIDY "${peglib_clang_tidy}")
        # Not a test — it has no test assertions, it exists solely so tidy
        # has a TU to analyze. Don't register it with ctest.
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
