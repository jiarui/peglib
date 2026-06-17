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
    const Rule<> grammar = &terminal('a');

    SUBCASE("matches 'a' without consuming")
    {
        std::string input = "a";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().begin());
    }
    SUBCASE("does not match 'b'")
    {
        std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        CHECK_FALSE(ok);
        CHECK(context.mark() == context.get_input().begin());
    }
}

TEST_CASE("alternation-expression")
{
    const Rule<> grammar = terminal('a') | 'b' | 'c';

    SUBCASE("a")
    {
        const std::string input = "a";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("b")
    {
        const std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("c")
    {
        const std::string input = "c";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("d (no match)")
    {
        const std::string input = "d";
        Context context(input);
        bool ok = grammar(context);
        CHECK_FALSE(ok);
        CHECK(context.mark() == context.get_input().begin());
    }
}

TEST_CASE("zero-or-more-expression")
{
    const Rule<> grammar = *terminal('a');

    SUBCASE("a")
    {
        const std::string input = "a";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("aa")
    {
        const std::string input = "aa";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("aaa")
    {
        const std::string input = "aaa";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("b (zero matches, no advance)")
    {
        const std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().begin());
    }
    SUBCASE("empty input")
    {
        const std::string input = "";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().begin());
    }
}

TEST_CASE("one-or-more-expression")
{
    const Rule<> grammar = +terminal('a');

    SUBCASE("a")
    {
        const std::string input = "a";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("aaa")
    {
        const std::string input = "aaa";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("b (no match)")
    {
        const std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        CHECK_FALSE(ok);
        CHECK(context.mark() == context.get_input().begin());
    }
    SUBCASE("empty input (no match)")
    {
        const std::string input = "";
        Context context(input);
        bool ok = grammar(context);
        CHECK_FALSE(ok);
        CHECK(context.mark() == context.get_input().begin());
    }
}

TEST_CASE("n-times-expression")
{
    SUBCASE("1 * 'a' on 'a' consumes one")
    {
        const Rule<> grammar = 1 * terminal('a');
        const std::string input = "a";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("1 * 'a' on 'aa' stops after one")
    {
        const Rule<> grammar = 1 * terminal('a');
        const std::string input = "aa";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == std::next(context.get_input().begin()));
    }
    SUBCASE("2 * 'a' on 'a' fails")
    {
        const Rule<> grammar = 2 * terminal('a');
        const std::string input = "a";
        Context context(input);
        bool ok = grammar(context);
        CHECK_FALSE(ok);
        CHECK(context.mark() == context.get_input().begin());
    }
    SUBCASE("2 * 'a' on 'aa' succeeds")
    {
        const Rule<> grammar = 2 * terminal('a');
        const std::string input = "aa";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().end());
    }
}

TEST_CASE("not-expression")
{
    const Rule<> grammar = !terminal('a');

    SUBCASE("matches 'b' without consuming")
    {
        const std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().begin());
    }
    SUBCASE("does not match 'a'")
    {
        const std::string input = "a";
        Context context(input);
        bool ok = grammar(context);
        CHECK_FALSE(ok);
        CHECK(context.mark() == context.get_input().begin());
    }
}

TEST_CASE("optional-expression")
{
    const Rule<> grammar = -terminal('a');

    SUBCASE("matches 'a'")
    {
        const std::string input = "a";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("tolerates 'b' (no advance)")
    {
        const std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        CHECK(ok);
        CHECK(context.mark() == context.get_input().begin());
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
    const Rule<> grammar = *(terminal('a') >> terminal('b')) >> terminal('a');

    std::string input = "aba";
    Context context(input);
    CHECK(grammar(context));
    CHECK(context.ended());
}

TEST_CASE("non-terminal-recursion")
{
    Rule<> grammar;
    grammar = 'a' >> (grammar | 'b') | 'b' >> ('a' | grammar);

    SUBCASE("ab")
    {
        const std::string input = "ab";
        Context context(input);
        CHECK(grammar(context));
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("aab")
    {
        const std::string input = "aab";
        Context context(input);
        CHECK(grammar(context));
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("aaab")
    {
        const std::string input = "aaab";
        Context context(input);
        CHECK(grammar(context));
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("b (no match)")
    {
        const std::string input = "b";
        Context context(input);
        CHECK_FALSE(grammar(context));
        CHECK(context.mark() == context.get_input().begin());
    }
}

TEST_CASE("sequence-expression")
{
    const Rule<> grammar = terminal('a') >> 'b' >> 'c';

    SUBCASE("abc")
    {
        const std::string input = "abc";
        Context context(input);
        CHECK(grammar(context));
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("dabc (no match)")
    {
        const std::string input = "dabc";
        Context context(input);
        CHECK_FALSE(grammar(context));
        CHECK(context.mark() == context.get_input().begin());
    }
    SUBCASE("adbc (no match)")
    {
        const std::string input = "adbc";
        Context context(input);
        CHECK_FALSE(grammar(context));
        CHECK(context.mark() == context.get_input().begin());
    }
}

TEST_CASE("terminal-range-expression")
{
    const Rule<> grammar = terminal('0', '9');

    SUBCASE("matches '0'")
    {
        const std::string input = "0";
        Context context(input);
        CHECK(grammar(context));
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("does not match 'b'")
    {
        const std::string input = "b";
        Context context(input);
        CHECK_FALSE(grammar(context));
        CHECK(context.mark() == context.get_input().begin());
    }
}

TEST_CASE("terminal-set-expression")
{
    const Rule<> grammar =
        terminal(std::set<char>{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'});

    SUBCASE("matches '0'")
    {
        const std::string input = "0";
        Context context(input);
        CHECK(grammar(context));
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("matches '5'")
    {
        const std::string input = "5";
        Context context(input);
        CHECK(grammar(context));
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("matches '9'")
    {
        const std::string input = "9";
        Context context(input);
        CHECK(grammar(context));
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("does not match 'b'")
    {
        const std::string input = "b";
        Context context(input);
        CHECK_FALSE(grammar(context));
        CHECK(context.mark() == context.get_input().begin());
    }
}

TEST_CASE("terminal-seq-expression")
{
    const Rule<> grammar = terminalSeq("int");

    SUBCASE("matches 'int'")
    {
        const std::string input = "int";
        Context context(input);
        CHECK(grammar(context));
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("does not match 'b'")
    {
        const std::string input = "b";
        Context context(input);
        CHECK_FALSE(grammar(context));
        CHECK(context.mark() == context.get_input().begin());
    }
}

TEST_CASE("terminal-predicate-expression")
{
    const Rule<> grammar = terminal<char>([](char c) { return c == 'a'; });

    SUBCASE("matches 'a'")
    {
        const std::string input = "a";
        Context context(input);
        CHECK(grammar(context));
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("does not match 'b'")
    {
        const std::string input = "b";
        Context context(input);
        CHECK_FALSE(grammar(context));
        CHECK(context.mark() == context.get_input().begin());
    }
}

TEST_CASE("semantic-action-fires-on-match")
{
    Rule<> grammar = terminal('a');
    int matches = 0;
    const std::string input = "a";
    Context context(input);
    grammar.setAction(
        [&matches](decltype(context)&, decltype(context)::match_range) -> std::monostate {
            matches++;
            return {};
        });
    bool ok = grammar(context);
    CHECK(ok);
    CHECK(context.mark() == context.get_input().end());
    CHECK(matches == 1);
}

TEST_CASE("right-recursion")
{
    Rule<> r;
    r = 'x' >> r >> 'b' | 'a';

    SUBCASE("a")
    {
        const std::string input = "a";
        Context context(input);
        CHECK(r(context));
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("xab")
    {
        const std::string input = "xab";
        Context context(input);
        CHECK(r(context));
        CHECK(context.mark() == context.get_input().end());
    }
    SUBCASE("xxabb")
    {
        const std::string input = "xxabb";
        Context context(input);
        CHECK(r(context));
        CHECK(context.mark() == context.get_input().end());
    }
}

// ---------------------------------------------------------------------------
// Left-recursion tests (seed-grow algorithm).
// ---------------------------------------------------------------------------
TEST_CASE("left-recursion-simple")
{
    Rule<> r;
    r = (r >> 'b') | (r >> 'c') | terminal('a') | terminal('d');

    auto checkOk = [&](const std::string& input, bool ended) {
        Context context(input);
        bool ok = r(context);
        CHECK(ok);
        CHECK(context.ended() == ended);
    };
    auto checkFail = [&](const std::string& input) {
        Context context(input);
        bool ok = r(context);
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
// mutually-recursive non-terminals (add → mul → num → add), so the rules are
// default-constructed first and assigned in dependency order via a static
// initializer lambda.
// ---------------------------------------------------------------------------
namespace
{
Rule<> add_lr;
Rule<> mul_lr;
Rule<> num_lr;

[[maybe_unused]] const bool arithmetic_grammar_init = [] {
    auto digit = terminal('0', '9');
    auto integer = +digit;
    num_lr = integer | '(' >> add_lr >> ')';
    mul_lr = (mul_lr >> '*' >> num_lr) | (mul_lr >> '/' >> num_lr) | num_lr;
    add_lr = (add_lr >> '+' >> mul_lr) | (add_lr >> '-' >> mul_lr) | mul_lr;
    return true;
}();
} // namespace

TEST_CASE("left-recursion-arithmetic")
{
    auto checkOk = [](const std::string& input) {
        Context context(input);
        bool ok = add_lr(context);
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
// Phase 2.0 regression: local Rule variables in a lambda must not dangle.
//
// In the old design, NonTerminalRef stored a bare const NonTerminal&. When
// the referenced NonTerminal went out of scope (e.g. a local Rule<> in a
// grammar-construction lambda), the reference dangled — causing "pure virtual
// method called" crashes. With the shared_ptr-based Rule design, the
// NonTerminal's lifetime is extended by all Rule copies that reference it.
// ---------------------------------------------------------------------------
TEST_CASE("local-rule-in-lambda-does-not-dangle")
{
    // Build a grammar inside a lambda using local Rule variables, then return
    // the top-level Rule. The locals go out of scope, but the returned Rule
    // (and its expression tree) keeps the NonTerminals alive.
    auto build_grammar = []() -> Rule<> {
        Rule<> digit = terminal('0', '9');
        Rule<> number = +digit;
        Rule<> ws = *terminal<char>([](char c) { return c == ' '; });
        Rule<> expr;
        expr = number >> *(ws >> terminal('+') >> ws >> number);
        return expr;
    };

    Rule<> grammar = build_grammar();

    SUBCASE("simple addition")
    {
        std::string input = "1+2";
        Context context(input);
        CHECK(grammar(context));
        CHECK(context.ended());
    }
    SUBCASE("multiple additions with whitespace")
    {
        std::string input = "1 + 2 + 3";
        Context context(input);
        CHECK(grammar(context));
        CHECK(context.ended());
    }
    SUBCASE("single number")
    {
        std::string input = "42";
        Context context(input);
        CHECK(grammar(context));
        CHECK(context.ended());
    }
}
