// ---------------------------------------------------------------------------
// peglib core test driver.
//
// Individual test cases live in their per-header TUs (rule_test, parser_test,
// context_test, file_source_test, sourcemap_test, error_test, parse_tree_test,
// dynexpr_test, meta_grammar_test, self_parse_test, from_string_test,
// recursive_leak_test, negative_path_test, char32_smoke_test, skipper_test,
// to_dot_test).
//
// doctest main() is provided by doctest_main.cpp (shared object).
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

// Smoke test: the absolute minimal grammar parses on the most trivial input.
// If this fails, the whole library is fundamentally broken.
TEST_CASE("[smoke] minimal-terminal-parse")
{
    std::string input = "a";
    Context context(input);
    Grammar<> g;
    g["grammar"] = g.terminal('a');
    CHECK(g.parse("grammar", context));
    CHECK(context.ended());
}
