// ---------------------------------------------------------------------------
// Grammar API tests — the primary user-facing API.
//
// Tests cover:
//   - Basic rule definition and parsing
//   - Self-referential and mutually-recursive rules
//   - Auto-naming (rule name = map key)
//   - Validation (undefined rules)
//   - parse_string convenience
//   - Semantic actions
//   - Rule handle chaining (set_action, set_label)
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

// ---------------------------------------------------------------------------
// Basic grammar: digit sequence
// ---------------------------------------------------------------------------
TEST_CASE("[grammar] basic-rule-definition")
{
    Grammar<> g;
    g["digit"] = g.terminal('0', '9');
    g["number"] = +g["digit"];
    g.set_start("number");

    SUBCASE("parse_string succeeds on valid input")
    {
        CHECK(g.parse_string("12345"));
    }
    SUBCASE("parse_string fails on empty input")
    {
        CHECK_FALSE(g.parse_string(""));
    }
    SUBCASE("parse_string fails on non-digit")
    {
        CHECK_FALSE(g.parse_string("abc"));
    }
    SUBCASE("explicit rule via parse(rule, ctx)")
    {
        std::string input = "42";
        Context ctx(input);
        CHECK(g.parse("number", ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("explicit start rule via parse(ctx)")
    {
        std::string input = "42";
        Context ctx(input);
        CHECK(g.parse(ctx));
        CHECK(ctx.ended());
    }
}

// ---------------------------------------------------------------------------
// Recursive grammar: arithmetic (left-recursion + seed-grow)
// ---------------------------------------------------------------------------
TEST_CASE("[grammar] recursive-arithmetic")
{
    Grammar<> g;
    g["num"] = +g.terminal('0', '9');
    g["mul"] = g["mul"] >> '*' >> g["num"] | g["num"];
    g["add"] = g["add"] >> '+' >> g["mul"] | g["mul"];
    g.set_start("add");

    SUBCASE("single number")
    {
        CHECK(g.parse_string("1"));
    }
    SUBCASE("addition")
    {
        CHECK(g.parse_string("1+2"));
    }
    SUBCASE("multiplication precedence")
    {
        CHECK(g.parse_string("1+2*3"));
    }
    SUBCASE("complex expression")
    {
        CHECK(g.parse_string("1*2+3*4"));
    }
}

// ---------------------------------------------------------------------------
// Right recursion
// ---------------------------------------------------------------------------
TEST_CASE("[grammar] right-recursion")
{
    Grammar<> g;
    // r = 'x' r 'b' | 'a'
    g["r"] = g.terminal('x') >> g["r"] >> g.terminal('b') | g.terminal('a');
    g.set_start("r");

    SUBCASE("a")
    {
        CHECK(g.parse_string("a"));
    }
    SUBCASE("xab")
    {
        CHECK(g.parse_string("xab"));
    }
    SUBCASE("xxabb")
    {
        CHECK(g.parse_string("xxabb"));
    }
    SUBCASE("rejects b")
    {
        CHECK_FALSE(g.parse_string("b"));
    }
}

// ---------------------------------------------------------------------------
// Auto-naming: rule name comes from the map key
// ---------------------------------------------------------------------------
TEST_CASE("[grammar] auto-naming")
{
    Grammar<> g;
    g["my_rule"] = g.terminal('a');

    // The rule's internal name should be "my_rule" (appears in error messages)
    std::string input = "x";
    Context ctx(input);
    g.parse("my_rule", ctx);

    // On failure, error messages should mention "my_rule"
    auto diag = ctx.take_error();
    REQUIRE(diag.has_value());
    const auto& diag_ref = diag.value();
    bool found = false;
    for (const auto& item : diag_ref.expected()) {
        if (item.text == "my_rule") {
            found = true;
        }
    }
    CHECK(found);
}

// ---------------------------------------------------------------------------
// Validation: undefined rules
// ---------------------------------------------------------------------------
TEST_CASE("[grammar] undefined-rules")
{
    Grammar<> g;
    g["main"] = g["undefined"] >> g.terminal('a');

    auto undefined = g.undefined_rules();
    REQUIRE(undefined.size() >= 1);
    bool found = false;
    for (const auto& name : undefined) {
        if (name == "undefined") {
            found = true;
        }
    }
    CHECK(found);
    // "main" is defined (has an expression)
    found = (std::find(undefined.begin(), undefined.end(), "main") != undefined.end());
    CHECK_FALSE(found);
}

// ---------------------------------------------------------------------------
// Forward reference: rule used before assignment
// ---------------------------------------------------------------------------
TEST_CASE("[grammar] forward-reference")
{
    Grammar<> g;
    // "a" references "b" before "b" is defined
    g["a"] = g.terminal('x') >> g["b"];
    g["b"] = g.terminal('y');
    g.set_start("a");

    CHECK(g.parse_string("xy"));
}

// ---------------------------------------------------------------------------
// Semantic actions
// ---------------------------------------------------------------------------
TEST_CASE("[grammar] semantic-action")
{
    using Ctx = Context<char>;
    Grammar<> g;

    int value = -1;
    auto digit = (g["digit"] = g.terminal('0', '9'));
    digit.set_action([&value](Ctx& ctx, peg::Span sp) -> std::monostate {
        value = ctx.at(sp.start) - '0';
        return {};
    });

    std::string input = "7";
    Ctx ctx(input);
    REQUIRE(g.parse_ast("digit", ctx)); // typed actions run in the post-parse fold
    CHECK(value == 7);
}

// ---------------------------------------------------------------------------
// Rule handle chaining: set_action, set_label
// ---------------------------------------------------------------------------
TEST_CASE("[grammar] rule-chaining")
{
    using Ctx = Context<char>;
    Grammar<> g;
    int count = 0;
    auto token = (g["token"] = g.terminal('a'));
    token.set_action([&count](Ctx&, peg::Span) -> std::monostate {
        count++;
        return {};
    });

    std::string input = "a";
    Ctx ctx(input);
    REQUIRE(g.parse_ast("token", ctx)); // typed actions run in the post-parse fold
    CHECK(count == 1);
}

// ---------------------------------------------------------------------------
// Multiple rules, introspection
// ---------------------------------------------------------------------------
TEST_CASE("[grammar] introspection")
{
    Grammar<> g;
    g["alpha"] = g.terminal('a');
    g["beta"] = g.terminal('b');
    g["gamma"] = g.terminal('c');

    CHECK(g.has_rule("alpha"));
    CHECK(g.has_rule("beta"));
    CHECK(g.has_rule("gamma"));
    CHECK_FALSE(g.has_rule("delta"));

    auto names = g.rule_names();
    CHECK(names.size() == 3);
}

// ---------------------------------------------------------------------------
// Same grammar, many parses (reusability)
// ---------------------------------------------------------------------------
TEST_CASE("[grammar] reusable-across-parses")
{
    Grammar<> g;
    g["num"] = +g.terminal('0', '9');
    g.set_start("num");

    for (const std::string input : {"1", "12", "123", "99999"}) {
        CAPTURE(input);
        CHECK(g.parse_string(input));
    }
}

// ---------------------------------------------------------------------------
// unreachable_rules(): validation for C++-defined grammars
// ---------------------------------------------------------------------------
TEST_CASE("[grammar] unreachable-rules-cpp-grammar")
{
    Grammar<> g;
    g["start"] = g["used"] >> g.terminal('x');
    g["used"] = g.terminal('a');
    g["dead"] = g.terminal('b');
    g.set_start("start");

    auto unreachable = g.unreachable_rules();
    REQUIRE(unreachable.size() == 1);
    CHECK(unreachable[0] == "dead");
}

// ---------------------------------------------------------------------------
// find(): read-only rule lookup that does NOT insert (unlike operator[]).
// ---------------------------------------------------------------------------
TEST_CASE("[grammar] find-does-not-create-rule")
{
    Grammar<> g;
    g["real"] = g.terminal('a');
    g.set_start("real");

    // find() on a missing rule returns nullopt...
    CHECK_FALSE(g.find("typo").has_value());
    // ...and crucially does NOT create it: undefined_rules() stays clean.
    CHECK(g.undefined_rules().empty());

    // Contrast: operator[] would have inserted "typo" as undefined.
    // (Demonstrated here only to pin the documented caveat.)
    (void)g["another_typo"];
    auto undef = g.undefined_rules();
    REQUIRE(undef.size() == 1);
    CHECK(undef[0] == "another_typo");

    // find() on an existing rule returns the handle, with the right name.
    auto h = g.find("real");
    REQUIRE(h.has_value());
    const auto& h_ref = h.value();
    CHECK(h_ref.name() == "real");
    CHECK(h_ref.is_defined());
}
