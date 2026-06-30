#include "peglib.h"

#include "doctest.h"

#include <set>
#include <string>

using namespace peg;

// ---------------------------------------------------------------------------
// Operator DSL tests (>>, |, *, +, -, !, &, terminals, recursion)
// Ported from the original test/main.cpp assertions.
// ---------------------------------------------------------------------------

TEST_CASE("and-expression (lookahead)")
{
    Grammar<> g;
    g["grammar"] = &g.terminal('a');

    SUBCASE("matches 'a' without consuming")
    {
        std::string input = "a";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.mark() == 0);
    }
    SUBCASE("does not match 'b'")
    {
        std::string input = "b";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK_FALSE(ok);
        CHECK(context.mark() == 0);
    }
}

TEST_CASE("alternation-expression")
{
    Grammar<> g;
    g["grammar"] = g.terminal('a') | 'b' | 'c';

    SUBCASE("a")
    {
        const std::string input = "a";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.ended());
    }
    SUBCASE("b")
    {
        const std::string input = "b";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.ended());
    }
    SUBCASE("c")
    {
        const std::string input = "c";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.ended());
    }
    SUBCASE("d (no match)")
    {
        const std::string input = "d";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK_FALSE(ok);
        CHECK(context.mark() == 0);
    }
}

TEST_CASE("zero-or-more-expression")
{
    Grammar<> g;
    g["grammar"] = *g.terminal('a');

    SUBCASE("a")
    {
        const std::string input = "a";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.ended());
    }
    SUBCASE("aa")
    {
        const std::string input = "aa";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.ended());
    }
    SUBCASE("aaa")
    {
        const std::string input = "aaa";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.ended());
    }
    SUBCASE("b (zero matches, no advance)")
    {
        const std::string input = "b";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.mark() == 0);
    }
    SUBCASE("empty input")
    {
        const std::string input = "";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.mark() == 0);
    }
}

TEST_CASE("one-or-more-expression")
{
    Grammar<> g;
    g["grammar"] = +g.terminal('a');

    SUBCASE("a")
    {
        const std::string input = "a";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.ended());
    }
    SUBCASE("aaa")
    {
        const std::string input = "aaa";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.ended());
    }
    SUBCASE("b (no match)")
    {
        const std::string input = "b";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK_FALSE(ok);
        CHECK(context.mark() == 0);
    }
    SUBCASE("empty input (no match)")
    {
        const std::string input = "";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK_FALSE(ok);
        CHECK(context.mark() == 0);
    }
}

TEST_CASE("n-times-expression")
{
    SUBCASE("1 * 'a' on 'a' consumes one")
    {
        Grammar<> g;
        g["grammar"] = 1 * g.terminal('a');
        const std::string input = "a";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.ended());
    }
    SUBCASE("1 * 'a' on 'aa' stops after one")
    {
        Grammar<> g;
        g["grammar"] = 1 * g.terminal('a');
        const std::string input = "aa";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.mark() == 1);
    }
    SUBCASE("2 * 'a' on 'a' fails")
    {
        Grammar<> g;
        g["grammar"] = 2 * g.terminal('a');
        const std::string input = "a";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK_FALSE(ok);
        CHECK(context.mark() == 0);
    }
    SUBCASE("2 * 'a' on 'aa' succeeds")
    {
        Grammar<> g;
        g["grammar"] = 2 * g.terminal('a');
        const std::string input = "aa";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.ended());
    }
}

TEST_CASE("not-expression")
{
    Grammar<> g;
    g["grammar"] = !g.terminal('a');

    SUBCASE("matches 'b' without consuming")
    {
        const std::string input = "b";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.mark() == 0);
    }
    SUBCASE("does not match 'a'")
    {
        const std::string input = "a";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK_FALSE(ok);
        CHECK(context.mark() == 0);
    }
}

TEST_CASE("optional-expression")
{
    Grammar<> g;
    g["grammar"] = -g.terminal('a');

    SUBCASE("matches 'a'")
    {
        const std::string input = "a";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.ended());
    }
    SUBCASE("tolerates 'b' (no advance)")
    {
        const std::string input = "b";
        Context context(input);
        bool ok = g.parse("grammar", context);
        CHECK(ok);
        CHECK(context.mark() == 0);
    }
}

// Regression test: Repetition must restore the parser position to the last
// successful iteration boundary when a child fails mid-way. Without the fix,
// a failed child that partially consumed input would leave the parser
// stranded, corrupting the next sibling expression in a sequence.
//
// Note: most current expression types (SequenceExpr, NonTerminal) self-restore
// on failure, so the fix is defensive for those. It becomes critical for
// future expression types that consume partially before failing without
// self-restoring.
TEST_CASE("repetition-restores-position-on-child-partial-failure")
{
    // *(('a' >> 'b')) >> 'a'  on input "aba"
    // The repetition matches "ab" once, then tries ('a' >> 'b') again at
    // position 2: matches 'a', fails on EOF (wanted 'b'). The SequenceExpr
    // self-restores to position 2. The trailing 'a' then matches at position 2.
    Grammar<> g;
    g["grammar"] = *(g.terminal('a') >> g.terminal('b')) >> g.terminal('a');

    std::string input = "aba";
    Context context(input);
    CHECK(g.parse("grammar", context));
    CHECK(context.ended());
}

TEST_CASE("non-terminal-recursion")
{
    Grammar<> g;
    g["grammar"] = 'a' >> (g["grammar"] | 'b') | 'b' >> ('a' | g["grammar"]);

    SUBCASE("ab")
    {
        const std::string input = "ab";
        Context context(input);
        CHECK(g.parse("grammar", context));
        CHECK(context.ended());
    }
    SUBCASE("aab")
    {
        const std::string input = "aab";
        Context context(input);
        CHECK(g.parse("grammar", context));
        CHECK(context.ended());
    }
    SUBCASE("aaab")
    {
        const std::string input = "aaab";
        Context context(input);
        CHECK(g.parse("grammar", context));
        CHECK(context.ended());
    }
    SUBCASE("b (no match)")
    {
        const std::string input = "b";
        Context context(input);
        CHECK_FALSE(g.parse("grammar", context));
        CHECK(context.mark() == 0);
    }
}

TEST_CASE("sequence-expression")
{
    Grammar<> g;
    g["grammar"] = g.terminal('a') >> 'b' >> 'c';

    SUBCASE("abc")
    {
        const std::string input = "abc";
        Context context(input);
        CHECK(g.parse("grammar", context));
        CHECK(context.ended());
    }
    SUBCASE("dabc (no match)")
    {
        const std::string input = "dabc";
        Context context(input);
        CHECK_FALSE(g.parse("grammar", context));
        CHECK(context.mark() == 0);
    }
    SUBCASE("adbc (no match)")
    {
        const std::string input = "adbc";
        Context context(input);
        CHECK_FALSE(g.parse("grammar", context));
        CHECK(context.mark() == 0);
    }
}

TEST_CASE("terminal-range-expression")
{
    Grammar<> g;
    g["grammar"] = g.terminal('0', '9');

    SUBCASE("matches '0'")
    {
        const std::string input = "0";
        Context context(input);
        CHECK(g.parse("grammar", context));
        CHECK(context.ended());
    }
    SUBCASE("does not match 'b'")
    {
        const std::string input = "b";
        Context context(input);
        CHECK_FALSE(g.parse("grammar", context));
        CHECK(context.mark() == 0);
    }
}

TEST_CASE("terminal-set-expression")
{
    Grammar<> g;
    g["grammar"] = g.terminal(std::set<char>{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'});

    SUBCASE("matches '0'")
    {
        const std::string input = "0";
        Context context(input);
        CHECK(g.parse("grammar", context));
        CHECK(context.ended());
    }
    SUBCASE("matches '5'")
    {
        const std::string input = "5";
        Context context(input);
        CHECK(g.parse("grammar", context));
        CHECK(context.ended());
    }
    SUBCASE("matches '9'")
    {
        const std::string input = "9";
        Context context(input);
        CHECK(g.parse("grammar", context));
        CHECK(context.ended());
    }
    SUBCASE("does not match 'b'")
    {
        const std::string input = "b";
        Context context(input);
        CHECK_FALSE(g.parse("grammar", context));
        CHECK(context.mark() == 0);
    }
}

TEST_CASE("terminal-seq-expression")
{
    Grammar<> g;
    g["grammar"] = g.terminalSeq("int");

    SUBCASE("matches 'int'")
    {
        const std::string input = "int";
        Context context(input);
        CHECK(g.parse("grammar", context));
        CHECK(context.ended());
    }
    SUBCASE("does not match 'b'")
    {
        const std::string input = "b";
        Context context(input);
        CHECK_FALSE(g.parse("grammar", context));
        CHECK(context.mark() == 0);
    }
}

TEST_CASE("terminal-predicate-expression")
{
    Grammar<> g;
    g["grammar"] = g.terminal([](char c) { return c == 'a'; });

    SUBCASE("matches 'a'")
    {
        const std::string input = "a";
        Context context(input);
        CHECK(g.parse("grammar", context));
        CHECK(context.ended());
    }
    SUBCASE("does not match 'b'")
    {
        const std::string input = "b";
        Context context(input);
        CHECK_FALSE(g.parse("grammar", context));
        CHECK(context.mark() == 0);
    }
}

TEST_CASE("semantic-action-fires-on-match")
{
    Grammar<> g;
    int matches = 0;
    const std::string input = "a";
    Context<char> context(input);
    auto grammar = (g["grammar"] = g.terminal('a'));
    grammar.set_action([&matches](Context<char>&, peg::Span) -> std::monostate {
        matches++;
        return {};
    });
    auto ast = g.parse_ast("grammar", context); // typed actions run in the post-parse fold
    REQUIRE(ast);
    CHECK(context.ended());
    CHECK(matches == 1);
}

TEST_CASE("right-recursion")
{
    Grammar<> g;
    g["r"] = 'x' >> g["r"] >> 'b' | 'a';

    SUBCASE("a")
    {
        const std::string input = "a";
        Context context(input);
        CHECK(g.parse("r", context));
        CHECK(context.ended());
    }
    SUBCASE("xab")
    {
        const std::string input = "xab";
        Context context(input);
        CHECK(g.parse("r", context));
        CHECK(context.ended());
    }
    SUBCASE("xxabb")
    {
        const std::string input = "xxabb";
        Context context(input);
        CHECK(g.parse("r", context));
        CHECK(context.ended());
    }
}

// ---------------------------------------------------------------------------
// Left-recursion tests (seed-grow algorithm).
// ---------------------------------------------------------------------------
TEST_CASE("left-recursion-simple")
{
    Grammar<> g;
    g["r"] = (g["r"] >> 'b') | (g["r"] >> 'c') | g.terminal('a') | g.terminal('d');

    auto checkOk = [&](const std::string& input, bool ended) {
        Context context(input);
        bool ok = g.parse("r", context);
        CHECK(ok);
        CHECK(context.ended() == ended);
    };
    auto checkFail = [&](const std::string& input) {
        Context context(input);
        bool ok = g.parse("r", context);
        CHECK_FALSE(ok);
        CHECK_FALSE(context.ended());
    };

    checkOk("a", true);
    checkOk("ab", true);
    checkOk("abc", true);
    checkOk("acb", true);
    checkOk("abcb", true);
    checkOk("acbc", true);
    // These inputs match but leave trailing chars.
    checkOk("aa", false);
    checkOk("aba", false);
    checkOk("aca", false);
    checkOk("ad", false);
    checkOk("abd", false);
    // Non-starters.
    checkFail("b");
    checkFail("c");
    checkFail("ba");
    checkFail("ca");
}

// ---------------------------------------------------------------------------
// Left-recursion tests (seed-grow algorithm). The arithmetic grammar uses
// mutually-recursive non-terminals (add → mul → num → add). With the Grammar
// API, forward references are handled automatically, so the rules can be
// assigned in dependency order via a static initializer lambda.
// ---------------------------------------------------------------------------
namespace
{
Grammar<> arithmetic_g;

[[maybe_unused]] const bool arithmetic_grammar_init = [] {
    auto digit = arithmetic_g.terminal('0', '9');
    auto integer = +digit;
    arithmetic_g["num"] = integer | '(' >> arithmetic_g["add"] >> ')';
    arithmetic_g["mul"] = (arithmetic_g["mul"] >> '*' >> arithmetic_g["num"]) |
                          (arithmetic_g["mul"] >> '/' >> arithmetic_g["num"]) | arithmetic_g["num"];
    arithmetic_g["add"] = (arithmetic_g["add"] >> '+' >> arithmetic_g["mul"]) |
                          (arithmetic_g["add"] >> '-' >> arithmetic_g["mul"]) | arithmetic_g["mul"];
    arithmetic_g.set_start("add");
    return true;
}();
} // namespace

TEST_CASE("left-recursion-arithmetic")
{
    auto checkOk = [](const std::string& input) {
        Context context(input);
        bool ok = arithmetic_g.parse(context);
        CHECK(ok);
    };

    checkOk("1");
    checkOk("1+2");
    checkOk("1+2*3");
    checkOk("1*2+3");
    checkOk("(1+2)*3");
    checkOk("1*(2+3)");
    checkOk("(1*(2+3))*4");
}

// ---------------------------------------------------------------------------
// Indirect / mutual left-recursion (Warth seed-grow across rules).
//
// The tests above cover direct left-recursion (a rule's body begins with
// itself) and the arithmetic grammar, whose cross-rule reference (num →
// '(' add ')') is in a non-left position and so never exercises growth
// propagating across rules. These tests target the genuinely hard case:
// rules that call each other in LEFT position, where growth of one rule's
// seed must be observable as growth of a partner rule's seed. Before the
// fix, such grammars matched only the seed and never grew.
//
// The checkOk helpers below require FULL input consumption (context.ended())
// because parse() / parse_string() report partial-match success, which would
// otherwise mask a "matched only the seed" regression.
// ---------------------------------------------------------------------------
TEST_CASE("left-recursion-indirect-via-alias")
{
    // A delegates to B; B left-recurses through A. Mutual left-recursion
    // through an alias. A → B, B → A 'b' | 'x'.
    Grammar<> g;
    g["A"] = g["B"];
    g["B"] = (g["A"] >> 'b') | g.terminal('x');
    g.set_start("A");

    auto checkOk = [](const Grammar<>& gr, const std::string& input) {
        Context context(input);
        bool ok = gr.parse(context);
        CHECK(ok);
        CHECK(context.ended());
    };
    auto checkFail = [](const Grammar<>& gr, const std::string& input) {
        Context context(input);
        bool ok = gr.parse(context);
        CHECK_FALSE(ok);
        CHECK(context.mark() == 0);
    };

    checkOk(g, "x");
    checkOk(g, "xb");
    checkOk(g, "xbb");
    checkOk(g, "xbbb");
    checkFail(g, "");
    checkFail(g, "b");
}

TEST_CASE("left-recursion-mutual")
{
    // L → M | 'x',  M → L 'm'. Two mutually left-recursive rules with no
    // direct self-recursion on either.
    Grammar<> g;
    g["L"] = g["M"] | g.terminal('x');
    g["M"] = g["L"] >> 'm';
    g.set_start("L");

    auto checkOk = [](const Grammar<>& gr, const std::string& input) {
        Context context(input);
        bool ok = gr.parse(context);
        CHECK(ok);
        CHECK(context.ended());
    };
    auto checkFail = [](const Grammar<>& gr, const std::string& input) {
        Context context(input);
        bool ok = gr.parse(context);
        CHECK_FALSE(ok);
    };

    checkOk(g, "x");
    checkOk(g, "xm");
    checkOk(g, "xmm");
    checkFail(g, "");
    checkFail(g, "m");
}

TEST_CASE("left-recursion-indirect-via-base")
{
    // L → L M | 'x' (direct LR on L), M → 'm'. The left-recursive operand is
    // itself a rule, so growth of L must re-drive M each iteration. This is
    // the shape closest to the arithmetic grammar but with M purely a base.
    Grammar<> g;
    g["L"] = (g["L"] >> g["M"]) | g.terminal('x');
    g["M"] = g.terminal('m');
    g.set_start("L");

    auto checkOk = [&g](const std::string& input) {
        Context context(input);
        bool ok = g.parse(context);
        CHECK(ok);
        CHECK(context.ended());
    };

    checkOk("x");
    checkOk("xm");
    checkOk("xmm");
    checkOk("xmmm");
}

// ---------------------------------------------------------------------------
// Local Rule variables inside a lambda must not dangle after the lambda
// returns. Rule is a non-owning view (bare NonTerminal* + copied name); the
// Grammar returned from the lambda owns the NonTerminal via shared_ptr, so
// the Rule handles that were captured into expression trees stay valid as
// long as the Grammar is alive.
// ---------------------------------------------------------------------------
TEST_CASE("local-rule-in-lambda-does-not-dangle")
{
    // Build a grammar inside a lambda using local named rules, then return
    // the Grammar by value. The Rule handles returned by operator[] go out
    // of scope, but the Grammar (sole owner of the NonTerminals) keeps
    // everything alive.
    auto build_grammar = []() -> Grammar<> {
        Grammar<> g;
        g["digit"] = g.terminal('0', '9');
        g["number"] = +g["digit"];
        g["ws"] = *g.terminal([](char c) { return c == ' '; });
        g["expr"] = g["number"] >> *(g["ws"] >> g.terminal('+') >> g["ws"] >> g["number"]);
        g.set_start("expr");
        return g;
    };

    Grammar<> grammar = build_grammar();

    SUBCASE("simple addition")
    {
        std::string input = "1+2";
        Context context(input);
        CHECK(grammar.parse(context));
        CHECK(context.ended());
    }
    SUBCASE("multiple additions with whitespace")
    {
        std::string input = "1 + 2 + 3";
        Context context(input);
        CHECK(grammar.parse(context));
        CHECK(context.ended());
    }
    SUBCASE("single number")
    {
        std::string input = "42";
        Context context(input);
        CHECK(grammar.parse(context));
        CHECK(context.ended());
    }
}
