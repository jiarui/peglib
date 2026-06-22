// ---------------------------------------------------------------------------
// Grammar API tests — the primary user-facing API (Phase 2).
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
    g["digit"] = terminal('0', '9');
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
    g["num"] = +terminal('0', '9');
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
    g["r"] = terminal('x') >> g["r"] >> terminal('b') | terminal('a');
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
    g["my_rule"] = terminal('a');

    // The rule's internal name should be "my_rule" (appears in error messages)
    std::string input = "x";
    Context ctx(input);
    g.parse("my_rule", ctx);

    // On failure, error messages should mention "my_rule"
    auto diag = ctx.take_error();
    REQUIRE(diag.has_value());
    bool found = false;
    for (const auto& item : diag->expected()) {
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
    g["main"] = g["undefined"] >> terminal('a');

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
    g["a"] = terminal('x') >> g["b"];
    g["b"] = terminal('y');
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
    g["digit"] = terminal('0', '9');

    int value = -1;
    g["digit"].set_action([&value](Ctx& ctx, Ctx::ParseTreeNodePtr node) -> std::monostate {
        value = ctx.at(node->start_offset) - '0';
        return {};
    });

    std::string input = "7";
    Ctx ctx(input);
    CHECK(g.parse("digit", ctx));
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
    g["token"] = terminal('a');
    g["token"].set_action([&count](Ctx&, Ctx::ParseTreeNodePtr) -> std::monostate {
        count++;
        return {};
    });

    std::string input = "a";
    Ctx ctx(input);
    g.parse("token", ctx);
    CHECK(count == 1);
}

// ---------------------------------------------------------------------------
// Multiple rules, introspection
// ---------------------------------------------------------------------------
TEST_CASE("[grammar] introspection")
{
    Grammar<> g;
    g["alpha"] = terminal('a');
    g["beta"] = terminal('b');
    g["gamma"] = terminal('c');

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
    g["num"] = +terminal('0', '9');
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
    g["start"] = g["used"] >> terminal('x');
    g["used"] = terminal('a');
    g["dead"] = terminal('b');
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
    g["real"] = terminal('a');
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
    CHECK(h->name() == "real");
    CHECK(h->is_defined());
}
