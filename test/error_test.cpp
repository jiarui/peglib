#include "peglib.h"

#include "doctest.h"

#include <string>
#include <string_view>

using namespace peg;

// Concrete Context type alias for Grammar template argument.
using Ctxt = Context<char>;

// ---------------------------------------------------------------------------
// ParseError.h / Context error tracking tests
//
// Covers:
//   - escape_char_for_expected, escape_string_for_expected
//   - Context::record_failure / furthest_failure_pos / expected / take_error
//   - NonTerminal set_name/set_label producing expected items on failure
//   - TerminalExpr recording Literal expectations on failure
//   - Multiple alternatives at the same position accumulate into expected set
//   - Earlier failures are suppressed when a later position reaches further
//   - Diagnostic::format output format
// ---------------------------------------------------------------------------

TEST_CASE("error-escape-char-printable")
{
    CHECK(escape_char_for_expected('a') == "'a'");
    CHECK(escape_char_for_expected('Z') == "'Z'");
    CHECK(escape_char_for_expected('0') == "'0'");
    CHECK(escape_char_for_expected(' ') == "' '");
    CHECK(escape_char_for_expected('\\') == "'\\\\'");
    CHECK(escape_char_for_expected('\'') == "'\\''");
}

TEST_CASE("error-escape-char-control")
{
    CHECK(escape_char_for_expected('\t') == "'\\t'");
    CHECK(escape_char_for_expected('\n') == "'\\n'");
    CHECK(escape_char_for_expected('\r') == "'\\r'");
    // Non-printable: hex form
    CHECK(escape_char_for_expected('\x01') == "'\\x01'");
    CHECK(escape_char_for_expected('\xFF') == "'\\xFF'");
}

TEST_CASE("error-escape-string")
{
    CHECK(escape_string_for_expected("abc") == "\"abc\"");
    CHECK(escape_string_for_expected("a\tb") == "\"a\\tb\"");
    CHECK(escape_string_for_expected("a\"b") == "\"a\\\"b\"");
    // Use string concatenation to avoid greedy hex escape parsing
    CHECK(escape_string_for_expected("a\x01"
                                     "b") == "\"a\\x01b\"");
}

TEST_CASE("error-context-record-failure-basic")
{
    std::string input = "hello";
    Context context(input);

    CHECK_FALSE(context.has_error());

    context.record_failure(2, ExpectedItem{ExpectedKind::Literal, "'x'"});
    CHECK(context.has_error());
    CHECK(context.furthest_failure_pos() == 2);
    CHECK(context.expected().size() == 1);
    CHECK(context.expected().begin()->text == "'x'");
}

TEST_CASE("error-context-record-failure-furthest-wins")
{
    std::string input = "hello";
    Context context(input);

    // Record at pos 2
    context.record_failure(2, ExpectedItem{ExpectedKind::Literal, "'a'"});
    // Record at pos 5 — further, should clear and replace
    context.record_failure(5, ExpectedItem{ExpectedKind::Literal, "'b'"});
    CHECK(context.furthest_failure_pos() == 5);
    CHECK(context.expected().size() == 1);
    CHECK(context.expected().begin()->text == "'b'");
}

TEST_CASE("error-context-record-failure-earlier-ignored")
{
    std::string input = "hello";
    Context context(input);

    // Record at pos 5 first
    context.record_failure(5, ExpectedItem{ExpectedKind::Literal, "'b'"});
    // Record at pos 2 — earlier, should be ignored
    context.record_failure(2, ExpectedItem{ExpectedKind::Literal, "'a'"});
    CHECK(context.furthest_failure_pos() == 5);
    CHECK(context.expected().size() == 1);
    CHECK(context.expected().begin()->text == "'b'");
}

TEST_CASE("error-context-record-failure-same-pos-accumulates")
{
    std::string input = "hello";
    Context context(input);

    context.record_failure(3, ExpectedItem{ExpectedKind::Literal, "'a'"});
    context.record_failure(3, ExpectedItem{ExpectedKind::Literal, "'b'"});
    context.record_failure(3, ExpectedItem{ExpectedKind::Literal, "'a'"}); // dup
    CHECK(context.furthest_failure_pos() == 3);
    CHECK(context.expected().size() == 2); // deduped
}

TEST_CASE("error-context-take-error")
{
    std::string input = "hello";
    Context context(input);

    context.record_failure(1, ExpectedItem{ExpectedKind::RuleName, "MyRule"});
    auto err = context.take_error();
    REQUIRE(err.has_value());
    CHECK(err->position() == 1);
    CHECK(err->expected().size() == 1);
    CHECK(err->expected().begin()->kind == ExpectedKind::RuleName);

    // After take_error, the context should be clean
    CHECK_FALSE(context.has_error());
    auto err2 = context.take_error();
    CHECK_FALSE(err2.has_value());
}

TEST_CASE("error-terminal-records-expected-on-failure")
{
    std::string input = "abc";
    Context context(input);

    Grammar<> g;
    g["anon"] = peg::terminal('x');
    CHECK_FALSE(g.parse("anon", context));
    CHECK(context.has_error());
    CHECK(context.furthest_failure_pos() == 0);
    // Named rule: both the literal 'x' (from the terminal) and the rule
    // name "anon" (from the NonTerminal) appear in the expected set.
    CHECK(context.expected().size() == 2);
    bool found_literal = false;
    bool found_rule = false;
    for (const auto& item : context.expected()) {
        if (item.kind == ExpectedKind::Literal && item.text == "'x'") {
            found_literal = true;
        }
        if (item.kind == ExpectedKind::RuleName && item.text == "anon") {
            found_rule = true;
        }
    }
    CHECK(found_literal);
    CHECK(found_rule);
}

TEST_CASE("error-named-rule-records-rulename-on-failure")
{
    std::string input = "abc";
    Context context(input);

    Grammar<> g;
    g["MyRule"] = peg::terminal('x');
    CHECK_FALSE(g.parse("MyRule", context));
    CHECK(context.has_error());
    // The NonTerminal adds RuleName (label takes priority, but we only set name)
    bool found_rule_name = false;
    for (const auto& item : context.expected()) {
        if (item.kind == ExpectedKind::RuleName && item.text == "MyRule") {
            found_rule_name = true;
        }
    }
    CHECK(found_rule_name);
}

TEST_CASE("error-labeled-rule-records-rulelabel-on-failure")
{
    std::string input = "abc";
    Context context(input);

    Grammar<> g;
    g["MyRule"] = peg::terminal('x');
    g["MyRule"].set_label("a specific thing");
    CHECK_FALSE(g.parse("MyRule", context));
    CHECK(context.has_error());
    bool found_label = false;
    for (const auto& item : context.expected()) {
        if (item.kind == ExpectedKind::RuleLabel && item.text == "a specific thing") {
            found_label = true;
        }
    }
    CHECK(found_label);
}

TEST_CASE("error-alternatives-accumulate-expected")
{
    // Two terminals in an alternation, both fail at the same position,
    // should produce 2 expected items.
    std::string input = "x";
    Context context(input);

    Grammar<> g;
    g["alt"] = peg::terminal('a') | peg::terminal('b');
    CHECK_FALSE(g.parse("alt", context));
    CHECK(context.has_error());
    // Both 'a' and 'b' should be in the expected set at position 0
    CHECK(context.expected().size() >= 1);
    bool found_a = false;
    bool found_b = false;
    for (const auto& item : context.expected()) {
        if (item.text == "'a'")
            found_a = true;
        if (item.text == "'b'")
            found_b = true;
    }
    CHECK(found_a);
    CHECK(found_b);
}

TEST_CASE("error-diagnostic-format")
{
    std::string_view src = "first line\nsecond line\nthird";
    SourceMap map{src};

    {
        Diagnostic diag{7,
                        {ExpectedItem{ExpectedKind::Literal, "'x'"},
                         ExpectedItem{ExpectedKind::Literal, "'y'"}}};
        std::string msg = diag.format(map, "test.lua");
        // Position 7 is on line 1, column 8
        CHECK(msg == "test.lua:1:8: error: expected 'x' or 'y'");
    }

    {
        // Position 12 is on line 2, column 2 ("second" — s=col1, e=col2)
        Diagnostic diag{12, {ExpectedItem{ExpectedKind::RuleName, "Name"}}};
        std::string msg = diag.format(map, "file.lua");
        CHECK(msg == "file.lua:2:2: error: expected Name");
    }

    {
        // Empty expected set
        Diagnostic diag{0, {}};
        std::string msg = diag.format(map, "f.lua");
        CHECK(msg == "f.lua:1:1: error: unexpected input");
    }
}

TEST_CASE("error-parseerror-exception-construct")
{
    ParseError err{5, {ExpectedItem{ExpectedKind::Literal, "'a'"}}};
    CHECK(err.position() == 5);
    CHECK(err.expected().size() == 1);

    // what() should mention the offset
    std::string what = err.what();
    CHECK(what.find("offset 5") != std::string::npos);
    CHECK(what.find("'a'") != std::string::npos);

    // Convert to Diagnostic
    Diagnostic diag = err.to_diagnostic();
    CHECK(diag.position() == 5);
    CHECK(diag.expected().size() == 1);
}

TEST_CASE("error-parseerror-exception-thrown-and-caught")
{
    std::set<ExpectedItem> expected = {
        ExpectedItem{ExpectedKind::RuleName, "Numeral"},
        ExpectedItem{ExpectedKind::RuleName, "Name"},
    };
    bool caught = false;
    try {
        throw ParseError{10, expected};
    } catch (const ParseError& e) {
        caught = true;
        CHECK(e.position() == 10);
        CHECK(e.expected().size() == 2);
    }
    CHECK(caught);
}

TEST_CASE("error-parseerror-caught-as-runtime-error")
{
    // ParseError derives from std::runtime_error, so catching by base works.
    bool caught = false;
    try {
        throw ParseError{3, {ExpectedItem{ExpectedKind::Literal, "'x'"}}};
    } catch (const std::runtime_error& e) {
        caught = true;
        CHECK(std::string(e.what()).find("offset 3") != std::string::npos);
    }
    CHECK(caught);
}

TEST_CASE("grammar-auto-names-rules")
{
    std::string input = "x";
    Context context(input);

    // Grammar::operator[] auto-names the rule from the map key.
    Grammar<char> g;
    g["my_digit"] = peg::terminal('0') | peg::terminal('1');
    (void)context;

    CHECK(g["my_digit"].name() == "my_digit");
    CHECK(g["my_digit"].label().empty());
}

TEST_CASE("grammar-set-label-works")
{
    std::string input = "x";
    Context context(input);

    Grammar<char> g;
    g["my_thing"] = peg::terminal('z');
    g["my_thing"].set_label("a specific thing");
    (void)context;

    CHECK(g["my_thing"].name() == "my_thing");
    CHECK(g["my_thing"].label() == "a specific thing");
}
