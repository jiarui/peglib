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
        # Build a list of -Wno-* flags that suppress diagnostics originating
        # in system libstdc++ headers on bleeding-edge toolchains. These are
        # not issues in our code and don't appear on CI's Ubuntu 24.04 +
        # clang-22 setup, but would otherwise abort local tidy runs.
        #
        # -Wno-#warnings: GCC 16's <ciso646> emits a #warning about the
        #   header being removed in C++20. Recognized by all clang versions.
        #
        # -Wno-c2y-extensions: clang 22.1+ flags __COUNTER__ (used by doctest)
        #   as a C2y extension. This flag only exists on very recent clang
        #   builds, so we check for support before adding it — otherwise the
        #   unknown-option error itself aborts the tidy run.
        include(CheckCXXCompilerFlag)
        set(tidy_extra_args "--extra-arg=-Wno-#warnings")
        check_cxx_compiler_flag("-Wno-c2y-extensions" peglib_supports_wno_c2y_extensions)
        if(peglib_supports_wno_c2y_extensions)
            string(APPEND tidy_extra_args ";--extra-arg=-Wno-c2y-extensions")
        endif()
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE};--quiet;${tidy_extra_args}")
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
