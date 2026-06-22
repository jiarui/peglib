# ---------------------------------------------------------------------------
# Vendored doctest (single header, no network needed at build time)
# ---------------------------------------------------------------------------
set(doctest_include_dir ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# Shared doctest main() — defines DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN once
# and is linked into every test executable so the other TUs only need
# `#include "doctest.h"`.
add_library(peglib_test_main OBJECT doctest_main.cpp)
target_include_directories(peglib_test_main PRIVATE ${doctest_include_dir})

# ---------------------------------------------------------------------------
# Warning flags for the TEST targets only (not the INTERFACE peglib library,
# so consumers of peglib are not forced into -Werror). GCC/Clang get the full
# -Wall -Wextra -Wpedantic -Werror treatment; MSVC uses /W4 /WX. This catches
# issues in both the tests and the header-only library as compiled here.
# ---------------------------------------------------------------------------
add_library(peglib_test_warnings INTERFACE)
target_compile_options(peglib_test_warnings INTERFACE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:
        -Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>)

# ---------------------------------------------------------------------------
# Core library tests
# ---------------------------------------------------------------------------
add_executable(peglib_test
    main.cpp
    rule_test.cpp
    grammar_test.cpp
    parser_test.cpp
    context_test.cpp
    file_source_test.cpp
    sourcemap_test.cpp
    parse_tree_test.cpp
    error_test.cpp
    dynexpr_test.cpp
    meta_grammar_test.cpp
    self_parse_test.cpp
    from_string_test.cpp
    recursive_leak_test.cpp
    negative_path_test.cpp
    char32_smoke_test.cpp
    skipper_test.cpp
    to_dot_test.cpp)

target_link_libraries(peglib_test PRIVATE peglib peglib_test_main peglib_test_warnings)
target_include_directories(peglib_test PRIVATE ${doctest_include_dir})

target_compile_definitions(peglib_test PRIVATE
    PEGLIB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    PEGLIB_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")

add_test(NAME peglib_test COMMAND peglib_test)

# ---------------------------------------------------------------------------
# Lua lexer/parser smoke tests (no external deps beyond doctest).
# Gated on PEGLIB_BUILD_LUA_EXAMPLES so they can be disabled independently
# of the core test suite (they're real-world examples, not library regressions).
# ---------------------------------------------------------------------------
if(PEGLIB_BUILD_LUA_EXAMPLES)
add_executable(peglib_lua_test
    lua_lex.cpp
    lua.cpp)

target_link_libraries(peglib_lua_test PRIVATE peglib peglib_test_main peglib_test_warnings)
target_include_directories(peglib_lua_test PRIVATE ${doctest_include_dir})
target_compile_definitions(peglib_lua_test PRIVATE
    PEGLIB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME peglib_lua_test COMMAND peglib_lua_test)
endif()

# ---------------------------------------------------------------------------
# JSON grammar example tests
# ---------------------------------------------------------------------------
add_executable(peglib_json_test
    json_test.cpp)

target_link_libraries(peglib_json_test PRIVATE peglib peglib_test_main peglib_test_warnings)
target_include_directories(peglib_json_test PRIVATE ${doctest_include_dir})
target_compile_definitions(peglib_json_test PRIVATE
    PEGLIB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME peglib_json_test COMMAND peglib_json_test)

# ---------------------------------------------------------------------------
# JSON-with-auto-skip comparison test. A separate target so the two JSON
# grammars (manual ws threading vs set_skipper) live in independent TUs and
# can be compared directly.
# ---------------------------------------------------------------------------
add_executable(peglib_json_skipper_test
    json_skipper_test.cpp)

target_link_libraries(peglib_json_skipper_test PRIVATE peglib peglib_test_main peglib_test_warnings)
target_include_directories(peglib_json_skipper_test PRIVATE ${doctest_include_dir})
target_compile_definitions(peglib_json_skipper_test PRIVATE
    PEGLIB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME peglib_json_skipper_test COMMAND peglib_json_skipper_test)
