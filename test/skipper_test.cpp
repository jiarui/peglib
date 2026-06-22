// ---------------------------------------------------------------------------
// Phase 3 auto-skip (Grammar::set_skipper + lexeme) — permanent test suite.
//
// Covers:
//   - Basic inter-token whitespace elimination between adjacent sequence
//     children and between repetition iterations.
//   - Pest-style leading whitespace at the Grammar::parse boundary.
//   - "No skipper" fallback (strict adjacency) preserved.
//   - clear_skipper restoring strict behaviour.
//   - lexeme() locally disabling skip for token-internal adjacency.
//   - Nested lexeme (idempotent flag save/restore).
//   - Backtracking does not leak the skipper's position advance.
//   - Recursive rule + skipper (memoization key consistency).
//   - Dyn-sequence path (GrammarCompiler-compiled grammar + set_skipper).
//   - char32_t context: the skipper is not char-specific.
//
// See json_skipper_test.cpp for a real-world grammar comparison and
// to_dot_test.cpp for the Phase 5 slice that ships alongside this.
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

namespace
{
// Whitespace: space, tab, CR, LF. Returned as a freshly-built rule body so
// the caller can assign it to g["ws"] (or any name) in their own Grammar.
auto ws_body()
{
    return *terminal<char>([](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    });
}
} // namespace

// ===========================================================================
// Basic behaviour
// ===========================================================================

TEST_CASE("skipper: sequence children separated by whitespace")
{
    Grammar<char> g;
    g["ws"] = ws_body();
    g["abc"] = terminal('a') >> terminal('b') >> terminal('c');
    g.set_start("abc");
    g.set_skipper(g["ws"]);

    CHECK(g.parse_string("abc"));
    CHECK(g.parse_string("a b c"));
    CHECK(g.parse_string("a\tb\nc"));
    CHECK(g.parse_string("  a   b c  "));
}

TEST_CASE("skipper: leading whitespace consumed at grammar boundary")
{
    // Pest-style: Grammar::parse runs the skipper once before the start
    // rule, so leading whitespace does not need an explicit `ws >>` prefix.
    Grammar<char> g;
    g["ws"] = ws_body();
    g["ab"] = terminal('a') >> terminal('b');
    g.set_start("ab");
    g.set_skipper(g["ws"]);

    CHECK(g.parse_string("ab"));
    CHECK(g.parse_string("  ab"));
    CHECK(g.parse_string("\t\n  a b"));
}

TEST_CASE("skipper: no skipper configured => strict adjacency")
{
    Grammar<char> g;
    g["abc"] = terminal('a') >> terminal('b') >> terminal('c');
    g.set_start("abc");
    // Deliberately no set_skipper.
    CHECK(g.parse_string("abc"));
    CHECK_FALSE(g.parse_string("a b c"));
    CHECK_FALSE(g.parse_string(" ab"));
}

TEST_CASE("skipper: clear_skipper restores strict behaviour")
{
    Grammar<char> g;
    g["ws"] = ws_body();
    g["abc"] = terminal('a') >> terminal('b') >> terminal('c');
    g.set_start("abc");
    g.set_skipper(g["ws"]);
    CHECK(g.parse_string("a b c"));

    g.clear_skipper();
    CHECK_FALSE(g.parse_string("a b c"));
    CHECK(g.parse_string("abc"));
    CHECK_FALSE(g.has_skipper());
}

// ===========================================================================
// Repetition + skipper
// ===========================================================================

TEST_CASE("skipper: repetition iterations separated by whitespace")
{
    Grammar<char> g;
    g["ws"] = ws_body();
    g["list"] = *terminal('a') >> terminal(';');
    g.set_start("list");
    g.set_skipper(g["ws"]);

    CHECK(g.parse_string("aaa;"));
    CHECK(g.parse_string("a a a ;"));
    CHECK(g.parse_string("  a   a  a  ;"));
    CHECK(g.parse_string("a;a;a")); // partial match: matches first "a;"
}

TEST_CASE("skipper: has_skipper reflects configuration")
{
    Grammar<char> g;
    CHECK_FALSE(g.has_skipper());
    g["ws"] = ws_body();
    g.set_skipper(g["ws"]);
    CHECK(g.has_skipper());
    g.clear_skipper();
    CHECK_FALSE(g.has_skipper());
}

// ===========================================================================
// lexeme()
// ===========================================================================

TEST_CASE("skipper: lexeme suppresses skip for token body")
{
    Grammar<char> g;
    g["ws"] = ws_body();
    // A number is a contiguous run of digits — internal whitespace splits it.
    g["number"] = lexeme(+terminal('0', '9'));
    g["two_nums"] = g["number"] >> g["number"];
    g.set_start("two_nums");
    g.set_skipper(g["ws"]);

    // "12 34" -> two numbers separated by whitespace.
    CHECK(g.parse_string("12 34"));
    // "1234" -> ONE number "1234" (greedy), then no second number at EOF.
    // The whole rule (two numbers) fails — correct.
    CHECK_FALSE(g.parse_string("1234"));
    // "1 2 3 4" -> partial match returns true after the first two numbers.
    CHECK(g.parse_string("1 2 3 4"));
}

TEST_CASE("skipper: nested lexeme is safe")
{
    Grammar<char> g;
    g["ws"] = ws_body();
    g["word"] = lexeme(lexeme(+terminal('a', 'z')));
    g["two_words"] = g["word"] >> g["word"];
    g.set_start("two_words");
    g.set_skipper(g["ws"]);

    CHECK(g.parse_string("hello world"));
    CHECK_FALSE(g.parse_string("hello")); // one word, then no second -> FAIL
}

TEST_CASE("skipper: lexeme forwarding collect_rule_refs")
{
    // lexeme should not hide rule references from to_dot / unreachable_rules.
    Grammar<char> g;
    g["inner"] = terminal('x');
    g["outer"] = lexeme(g["inner"]);
    g.set_start("outer");

    auto dot = g.to_dot();
    CHECK(dot.find("outer") != std::string::npos);
    CHECK(dot.find("inner") != std::string::npos);
    CHECK(dot.find("outer\" -> \"inner") != std::string::npos);
}

// ===========================================================================
// Backtracking & memoization interaction
// ===========================================================================

TEST_CASE("skipper: failed branch backtracks to pre-skip position")
{
    Grammar<char> g;
    g["ws"] = ws_body();
    // First alt: 'a' matches, skip advances past whitespace, 'b' must fail
    // if the next non-ws char is not 'b'. Backtracking must restore to the
    // position BEFORE 'a' (not the post-skip position).
    g["root"] = (terminal('a') >> terminal('b')) | terminal('x');
    g.set_start("root");
    g.set_skipper(g["ws"]);

    // "a x" — alt1: a@0 ok, skip to @2 (x), b@2 fails, rollback to @0,
    // alt2: x@0 fails (a is there). Whole rule fails.
    CHECK_FALSE(g.parse_string("a x"));
    // "x" — alt1: a fails, alt2: x@0 ok.
    CHECK(g.parse_string("x"));
    // "ab" — alt1: a@0 ok, b@1 ok.
    CHECK(g.parse_string("ab"));
}

TEST_CASE("skipper: recursive rule preserves memoization invariants")
{
    // list := elem (';' list)?  with skipper for the ';'
    // Memoization keys by (start_pos, NonTerminal*). The skipper must not
    // corrupt the key: each NonTerminal::parse takes its start_pos AFTER
    // any skip, so re-parses at the same position see the same memo entry.
    Grammar<char> g;
    g["ws"] = ws_body();
    g["elem"] = terminal('a', 'z');
    g["list"] = g["elem"] >> -(terminal(';') >> g["list"]);
    g.set_start("list");
    g.set_skipper(g["ws"]);

    CHECK(g.parse_string("a;b;c"));
    CHECK(g.parse_string("a ; b ; c"));
    CHECK(g.parse_string("  x ;  y ;z"));
    CHECK_FALSE(g.parse_string(";a")); // empty list not allowed (elem required)
}

// ===========================================================================
// Compiled-grammar path (DynSequenceExpr)
// ===========================================================================

TEST_CASE("skipper: compiled grammar (GrammarCompiler) honours set_skipper")
{
    auto g = GrammarCompiler::from_string(
        "Expr  <- Term ('+' Term)*\n"
        "Term  <- Factor ('*' Factor)*\n"
        "Factor <- [0-9]+ / '(' Expr ')'\n");
    g.set_start("Expr");

    // GrammarCompiler does NOT inject a ws rule, so referring to an
    // undefined ws rule is rejected. Verify the contract holds: an
    // undefined skipper rule throws std::invalid_argument.
    CHECK_THROWS_AS(g.set_skipper(g["__default_ws"]), std::invalid_argument);
    CHECK_FALSE(g.has_skipper());
}

TEST_CASE("skipper: user-added ws rule in compiled grammar works")
{
    // Compile a grammar and THEN add a ws rule + skipper. The compiled
    // DynSequenceExpr path must honour the skipper exactly as the static
    // path does.
    auto g = GrammarCompiler::from_string(
        "Expr  <- Term ('+' Term)*\n"
        "Term  <- Factor ('*' Factor)*\n"
        "Factor <- [0-9]+ / '(' Expr ')'\n");
    g.set_start("Expr");

    // Add a whitespace rule after compilation. Compiled rules reference
    // each other by name through the Grammar map, so adding a new rule
    // post-compile is fine.
    g["ws"] = *terminal<char>([](char c) {
        return c == ' ' || c == '\t';
    });
    g.set_skipper(g["ws"]);

    CHECK(g.parse_string("1+2*3"));
    CHECK(g.parse_string("1 + 2 * 3"));
    CHECK(g.parse_string("  ( 1 + 2 ) * 3 "));
}

// ===========================================================================
// CharT generality
// ===========================================================================

TEST_CASE("skipper: char32_t context supports set_skipper")
{
    Grammar<char32_t> g;
    g["ws"] = *terminal<char32_t>([](char32_t c) {
        return c == U' ' || c == U'\t' || c == U'\n';
    });
    g["ab"] = terminal(U'a') >> terminal(U'b');
    g.set_start("ab");
    g.set_skipper(g["ws"]);

    // parse_string is char-only; for char32_t, build a Context from a
    // u32string and call parse(ctx) directly.
    auto parse_u32 = [&g](const char32_t* s) {
        std::u32string input{s};
        Grammar<char32_t>::Context ctx{input};
        return g.parse(ctx);
    };

    CHECK(parse_u32(U"ab"));
    CHECK(parse_u32(U"a b"));
    CHECK(parse_u32(U"  a  b  "));
}
