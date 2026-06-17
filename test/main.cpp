// ---------------------------------------------------------------------------
// peglib core test driver.
//
// Individual test cases live in their per-header TUs:
//   rule_test.cpp        - operator DSL (>>, |, *, +, -, !, &, recursion)
//   parser_test.cpp      - low-level ParsingExpr + cut semantics
//   context_test.cpp     - Context state/position/cut lifecycle
//   file_source_test.cpp - FileSource streaming IO
//   error_test.cpp       - error reporting (Phase 1 placeholder)
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
    g["grammar"] = terminal('a');
    CHECK(g.parse("grammar", context));
    CHECK(context.ended());
}
