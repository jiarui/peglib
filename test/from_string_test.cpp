// ---------------------------------------------------------------------------
// GrammarCompiler tests — W3 of Phase 2 textual grammar format.
//
// Verifies that GrammarCompiler::from_string() produces working Grammar<>
// objects from PEG text. Each test compiles a small grammar and checks
// that it accepts/rejects the expected inputs.
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <fstream>
#include <sstream>
#include <string>

using namespace peg;

// ---------------------------------------------------------------------------
// Basic: single rule with a literal
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] basic-literal")
{
    auto g = GrammarCompiler::from_string("A <- 'hello'");
    CHECK(g.parse_string("hello"));
    CHECK_FALSE(g.parse_string("world"));
    CHECK_FALSE(g.parse_string("hel"));
}

// ---------------------------------------------------------------------------
// Character class
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] charclass")
{
    SUBCASE("range")
    {
        auto g = GrammarCompiler::from_string("N <- [0-9]+");
        CHECK(g.parse_string("12345"));
        CHECK(g.parse_string("0"));
        CHECK_FALSE(g.parse_string(""));
        CHECK_FALSE(g.parse_string("abc"));
    }
    SUBCASE("negated")
    {
        auto g = GrammarCompiler::from_string("S <- [^0-9]+");
        CHECK(g.parse_string("abc"));
        CHECK_FALSE(g.parse_string("123"));
        CHECK_FALSE(g.parse_string(""));
    }
    SUBCASE("mixed")
    {
        auto g = GrammarCompiler::from_string("I <- [a-zA-Z_][a-zA-Z0-9_]*");
        CHECK(g.parse_string("hello"));
        CHECK(g.parse_string("_foo_bar_99"));
        CHECK(g.parse_string("X"));
        CHECK_FALSE(g.parse_string("123abc"));
        CHECK_FALSE(g.parse_string(""));
    }
}

// ---------------------------------------------------------------------------
// Choice and sequence
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] choice-and-sequence")
{
    auto g = GrammarCompiler::from_string("Expr <- ('a' / 'b') ('c' / 'd')");
    CHECK(g.parse_string("ac"));
    CHECK(g.parse_string("ad"));
    CHECK(g.parse_string("bc"));
    CHECK(g.parse_string("bd"));
    CHECK_FALSE(g.parse_string("ab"));
    CHECK_FALSE(g.parse_string("cd"));
    CHECK_FALSE(g.parse_string("a"));
}

// ---------------------------------------------------------------------------
// Postfix operators
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] postfix-operators")
{
    SUBCASE("star")
    {
        auto g = GrammarCompiler::from_string("A <- 'a'*");
        CHECK(g.parse_string(""));
        CHECK(g.parse_string("a"));
        CHECK(g.parse_string("aaaa"));
        // "b" succeeds with zero matches (star allows 0 reps).
        CHECK(g.parse_string("b"));
    }
    SUBCASE("plus")
    {
        auto g = GrammarCompiler::from_string("A <- 'a'+");
        CHECK_FALSE(g.parse_string(""));
        CHECK(g.parse_string("a"));
        CHECK(g.parse_string("aaa"));
    }
    SUBCASE("optional")
    {
        auto g = GrammarCompiler::from_string("A <- 'a'?");
        CHECK(g.parse_string(""));
        CHECK(g.parse_string("a"));
        // "aa" succeeds (first 'a' matched by ?, second is unconsumed).
        // PEG parse_string only checks if the start rule matches, not if
        // the entire input was consumed.
        CHECK(g.parse_string("aa"));
    }
}

// ---------------------------------------------------------------------------
// Prefix operators
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] prefix-operators")
{
    SUBCASE("and-predicate")
    {
        auto g = GrammarCompiler::from_string("A <- &'a' 'a'");
        CHECK(g.parse_string("a"));
        CHECK_FALSE(g.parse_string("b"));
    }
    SUBCASE("not-predicate")
    {
        auto g = GrammarCompiler::from_string("A <- !'a' . ");
        CHECK(g.parse_string("b"));
        CHECK_FALSE(g.parse_string("a"));
    }
}

// ---------------------------------------------------------------------------
// Dot (any char)
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] dot")
{
    auto g = GrammarCompiler::from_string("A <- .+");
    CHECK(g.parse_string("x"));
    CHECK(g.parse_string("hello"));
    CHECK_FALSE(g.parse_string(""));
}

// ---------------------------------------------------------------------------
// Recursive grammar (left-recursion + seed-grow)
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] recursive-arithmetic")
{
    auto g = GrammarCompiler::from_string(R"(
        Num   <- [0-9]+
        Mul   <- Mul '*' Num / Num
        Add   <- Add '+' Mul / Mul
    )");
    g.set_start("Add");
    CHECK(g.parse_string("1"));
    CHECK(g.parse_string("1+2"));
    CHECK(g.parse_string("1*2"));
    CHECK(g.parse_string("1+2*3"));
    CHECK(g.parse_string("1*2+3*4"));
    CHECK_FALSE(g.parse_string(""));
}

// ---------------------------------------------------------------------------
// Right recursion
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] right-recursion")
{
    auto g = GrammarCompiler::from_string("R <- 'x' R 'b' / 'a'");
    CHECK(g.parse_string("a"));
    CHECK(g.parse_string("xab"));
    CHECK(g.parse_string("xxabb"));
    CHECK_FALSE(g.parse_string("b"));
}

// ---------------------------------------------------------------------------
// Comments and whitespace in grammar text
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] comments-and-whitespace")
{
    auto g = GrammarCompiler::from_string(R"(
        # This is a comment
        S <- 'a'+  # trailing comment

        # Another comment
        # More comments
    )");
    CHECK(g.parse_string("a"));
    CHECK(g.parse_string("aaa"));
    CHECK_FALSE(g.parse_string(""));
}

// ---------------------------------------------------------------------------
// Double-quoted literals
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] double-quoted-literal")
{
    auto g = GrammarCompiler::from_string("A <- \"world\"");
    CHECK(g.parse_string("world"));
    CHECK_FALSE(g.parse_string("hello"));
}

// ---------------------------------------------------------------------------
// Escape sequences in literals
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] literal-escapes")
{
    SUBCASE("tab")
    {
        auto g = GrammarCompiler::from_string("A <- 'a\\tb'");
        CHECK(g.parse_string("a\tb"));
    }
    SUBCASE("newline")
    {
        auto g = GrammarCompiler::from_string("A <- 'a\\nb'");
        CHECK(g.parse_string("a\nb"));
    }
}

// ---------------------------------------------------------------------------
// Error: malformed grammar text
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] rejects-malformed")
{
    SUBCASE("missing-arrow")
    {
        CHECK_THROWS_AS(GrammarCompiler::from_string("A 'x'"), ParseError);
    }
    SUBCASE("unclosed-literal")
    {
        CHECK_THROWS_AS(GrammarCompiler::from_string("A <- 'x"), ParseError);
    }
    SUBCASE("empty-input")
    {
        CHECK_THROWS_AS(GrammarCompiler::from_string(""), ParseError);
    }
}

// ---------------------------------------------------------------------------
// try_from_string: non-throwing version
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] try_from_string")
{
    Grammar<> g;
    Diagnostic err{0, {}};

    SUBCASE("valid-grammar")
    {
        CHECK(GrammarCompiler::try_from_string("A <- 'x'", g, err));
        CHECK(g.parse_string("x"));
    }
    SUBCASE("invalid-grammar")
    {
        CHECK_FALSE(GrammarCompiler::try_from_string("bad syntax !!!", g, err));
    }
}

// ---------------------------------------------------------------------------
// Semantic actions: post-binding after from_string
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] semantic-action-binding")
{
    auto g = GrammarCompiler::from_string("N <- [0-9]+");

    // Bind an action to count characters matched.
    int count = 0;
    using Ctx = Grammar<>::Context;
    g["N"].set_action([&count](Ctx& ctx, Ctx::ParseTreeNodePtr node) -> std::monostate {
        count += static_cast<int>(node->end_offset - node->start_offset);
        return {};
    });

    std::string input = "12345";
    Ctx ctx{input};
    CHECK(g.parse(ctx));
    CHECK(count == 5);
}

// ---------------------------------------------------------------------------
// Self-parse: compile peg.peg itself and verify it can parse PEG text
// ---------------------------------------------------------------------------
TEST_CASE("[from_string] self-hosting-compile-peg-spec")
{
    // Load meta/peg.peg
    std::string path = std::string{PEGLIB_PROJECT_ROOT} + "/meta/peg.peg";
    std::ifstream f{path};
    REQUIRE(f.is_open());
    std::stringstream ss;
    ss << f.rdbuf();
    std::string peg_spec = ss.str();

    // Compile peg.peg into a Grammar
    auto g = GrammarCompiler::from_string(peg_spec);

    // The compiled grammar should be able to parse a simple PEG snippet
    CHECK(g.parse_string("A <- 'x'"));
    CHECK(g.parse_string("Expr <- Term ('+' Term)*"));
}
