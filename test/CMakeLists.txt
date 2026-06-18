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
    negative_path_test.cpp)

target_link_libraries(peglib_test PRIVATE peglib peglib_test_main)
target_include_directories(peglib_test PRIVATE ${doctest_include_dir})

target_compile_definitions(peglib_test PRIVATE
    PEGLIB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    PEGLIB_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")

add_test(NAME peglib_test COMMAND peglib_test)

# ---------------------------------------------------------------------------
# Lua lexer/parser smoke tests (no external deps beyond doctest)
# ---------------------------------------------------------------------------
add_executable(peglib_lua_test
    lua_lex.cpp
    lua.cpp)

target_link_libraries(peglib_lua_test PRIVATE peglib peglib_test_main)
target_include_directories(peglib_lua_test PRIVATE ${doctest_include_dir})
target_compile_definitions(peglib_lua_test PRIVATE
    PEGLIB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME peglib_lua_test COMMAND peglib_lua_test)

# ---------------------------------------------------------------------------
# JSON grammar example tests
# ---------------------------------------------------------------------------
add_executable(peglib_json_test
    json_test.cpp)

target_link_libraries(peglib_json_test PRIVATE peglib peglib_test_main)
target_include_directories(peglib_json_test PRIVATE ${doctest_include_dir})
target_compile_definitions(peglib_json_test PRIVATE
    PEGLIB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME peglib_json_test COMMAND peglib_json_test)
