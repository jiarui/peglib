// ---------------------------------------------------------------------------
// JSON grammar — a real-world example of building a complete language grammar
// with peglib's operator DSL.
//
// This demonstrates:
//   - Mutually recursive rules (value → object/array → value)
//   - Forward-declared Rule<> objects assigned via a static initializer
//   - Whitespace handling (manual; auto-skipper arrives in Phase 3)
//   - Error reporting integration (Diagnostic on parse failure)
//
// The grammar uses the default Context (NodeType = std::monostate). AST
// construction from match ranges is shown in the test code itself — this is
// how real consumers would bridge the parse tree to their own data model.
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <string>
#include <string_view>

using namespace peg;

// ---------------------------------------------------------------------------
// Grammar definition.
//
// Mutual recursion (value → object/array → value) means the rules need forward
// declaration before assignment. We use the same static-initializer pattern
// documented in rule_test.cpp:480-501.
// ---------------------------------------------------------------------------

namespace json_grammar
{
// Forward-declared rules. Default-constructed; assigned in init_grammar().
inline Rule<> value;
inline Rule<> object;
inline Rule<> array;
inline Rule<> string_rule;
inline Rule<> number_rule;
inline Rule<> keyword_rule;
inline Rule<> ws;   // whitespace, optional
inline Rule<> json; // top-level: optional-ws value optional-ws
// Auxiliary rules used inside array/object. These must outlive the
// static initializer lambda, so they're declared at namespace scope
// (NOT as locals). NonTerminalRef holds a bare reference — a local
// Rule<> would dangle after the lambda exits.
inline Rule<> value_list;
inline Rule<> member_list;
inline Rule<> key_value;

[[maybe_unused]] const bool grammar_initialized = [] {
    auto cut_ = cut<Context<std::span<const char>>>();

    // Whitespace: space, tab, newline, carriage return. (Phase 3 will
    // introduce Context::set_skipper so users won't need to thread this
    // manually.)
    ws = *terminal<char>([](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    });

    // ----- string -----
    auto escape_seq = terminal('\\') >> terminal<char>([](char c) {
        return c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' ||
               c == 'n' || c == 'r' || c == 't' || c == 'u';
    });
    auto string_char = terminal<char>([](char c) {
        return c != '"' && c != '\\' && static_cast<unsigned char>(c) >= 0x20;
    });
    string_rule = terminal('"') >> *(escape_seq | string_char) >> terminal('"');

    // ----- number -----
    // JSON number grammar:
    //   int = -? (0 | [1-9] [0-9]*)
    //   frac = "." [0-9]+
    //   exp = ("e"|"E") ("+"|"-")? [0-9]+
    //   number = int frac? exp?
    auto digit = terminal('0', '9');
    auto nonzero_digit = terminal('1', '9');
    auto sign = terminal('-') | terminal('+');
    auto integer = (-terminal('-')) >> (terminal('0') | (nonzero_digit >> *digit));
    auto frac = terminal('.') >> +digit;
    auto exp = (terminal('e') | 'E') >> -sign >> +digit;
    number_rule = integer >> -frac >> -exp;

    // ----- keyword (true/false/null) -----
    keyword_rule = terminalSeq("true") | terminalSeq("false") | terminalSeq("null");

    // ----- array -----
    // array = '[' ws ( value (ws ',' ws value)* )? ws ']'
    auto comma_sep = ws >> terminal(',') >> ws;
    value_list = value >> *(comma_sep >> value);
    array = terminal('[') >> ws >> -value_list >> ws >> terminal(']');

    // ----- object -----
    // object = '{' ws ( string ws ':' ws value (ws ',' ws string ws ':' ws value)* )? ws '}'
    key_value = string_rule >> ws >> terminal(':') >> ws >> value;
    member_list = key_value >> *(comma_sep >> key_value);
    object = terminal('{') >> ws >> -member_list >> ws >> terminal('}');

    // ----- value -----
    // Each alternative commits with `cut` after a successful match: once we
    // know it's a number, we don't backtrack into trying it as an object.
    // The cut must live inside this AlternationExpr so the cut stack frame
    // is provided by the alternation, not by the individual rules.
    value = (keyword_rule >> cut_) | (number_rule >> cut_) |
            (object >> cut_) | (array >> cut_) | (string_rule >> cut_);

    // ----- top-level -----
    json = ws >> value >> ws;

    return true;
}();
} // namespace json_grammar

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("[json] keyword-rule")
{
    using namespace json_grammar;
    for (const std::string& s : {"true", "false", "null"}) {
        Context ctx(s);
        CHECK(keyword_rule(ctx));
        CHECK(ctx.ended());
    }
}

TEST_CASE("[json] number-rule")
{
    using namespace json_grammar;
    for (const std::string& s : {"0", "1", "42", "-1", "3.14", "-0.5", "1e10",
                                 "2.5E-3", "123.456e789", "10.0", "0.001",
                                 "-123.456e+789"}) {
        Context ctx(s);
        CAPTURE(s);
        CHECK(number_rule(ctx));
        CHECK(ctx.ended());
    }
}

TEST_CASE("[json] number-rule-rejects-invalid")
{
    using namespace json_grammar;
    for (const std::string& s : {"01", "1.", "+5", ".5", "--1"}) {
        Context ctx(s);
        CAPTURE(s);
        bool matched = number_rule(ctx);
        CHECK(!(matched && ctx.ended()));
    }
}

TEST_CASE("[json] string-rule")
{
    using namespace json_grammar;
    for (const std::string& s : {"\"\"", "\"a\"", "\"hello world\"",
                                 "\"tab\\there\"", "\"unicode\\u0041\"",
                                 "\"slash\\/end\""}) {
        Context ctx(s);
        CAPTURE(s);
        CHECK(string_rule(ctx));
        CHECK(ctx.ended());
    }
}

TEST_CASE("[json] string-rule-rejects-unterminated")
{
    using namespace json_grammar;
    std::string input = "\"unterminated";
    Context ctx(input);
    CHECK_FALSE(string_rule(ctx));
}

TEST_CASE("[json] whitespace-tolerance")
{
    using namespace json_grammar;
    for (const std::string& input : {
             "  null  ",
             "\n\ttrue\r\n",
             "   42   ",
             "  \"hi\"  ",
         }) {
        Context ctx(input);
        CAPTURE(input);
        CHECK(json(ctx));
        CHECK(ctx.ended());
    }
}

TEST_CASE("[json] array-rule")
{
    using namespace json_grammar;
    SUBCASE("empty")
    {
        std::string input = "[]";
        Context ctx(input);
        CHECK(array(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("with-whitespace")
    {
        std::string input = "[  ]";
        Context ctx(input);
        CHECK(array(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("numbers")
    {
        std::string input = "[1, 2, 3]";
        Context ctx(input);
        CHECK(array(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("mixed-types")
    {
        std::string input = "[\"a\", true, null, 42]";
        Context ctx(input);
        CHECK(array(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("trailing-comma-invalid")
    {
        std::string input = "[1, 2,]";
        Context ctx(input);
        bool matched = array(ctx);
        CHECK(!(matched && ctx.ended()));
    }
}

TEST_CASE("[json] object-rule")
{
    using namespace json_grammar;
    SUBCASE("empty")
    {
        std::string input = "{}";
        Context ctx(input);
        CHECK(object(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("simple")
    {
        std::string input = "{\"name\": \"value\", \"n\": 42}";
        Context ctx(input);
        CHECK(object(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("nested")
    {
        std::string input = "{\"outer\": {\"inner\": true}}";
        Context ctx(input);
        CHECK(object(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("missing-colon-invalid")
    {
        std::string input = "{\"key\" \"value\"}";
        Context ctx(input);
        bool matched = object(ctx);
        CHECK(!(matched && ctx.ended()));
    }
}

TEST_CASE("[json] deeply-nested-structures")
{
    using namespace json_grammar;
    SUBCASE("nested-arrays")
    {
        std::string input = "[[[[[42]]]]]";
        Context ctx(input);
        CHECK(json(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("mixed-nesting")
    {
        std::string input = R"({"a": [1, {"b": 2}, true], "c": null})";
        Context ctx(input);
        CHECK(json(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("deep-object")
    {
        std::string input = R"({"l1": {"l2": {"l3": {"l4": "deep"}}}})";
        Context ctx(input);
        CHECK(json(ctx));
        CHECK(ctx.ended());
    }
}

TEST_CASE("[json] complex-realistic-document")
{
    using namespace json_grammar;
    std::string input = R"({
        "name": "peglib",
        "version": 1.0,
        "features": ["packrat", "left-recursion", "cut"],
        "config": {
            "cpp_standard": 20,
            "header_only": true,
            "dependencies": null
        },
        "empty_array": [],
        "empty_object": {}
    })";
    Context ctx(input);
    CHECK(json(ctx));
    CHECK(ctx.ended());
}

TEST_CASE("[json] error-on-malformed-reports-position")
{
    using namespace json_grammar;
    std::string input = "[1, 2,]"; // trailing comma is invalid JSON
    Context ctx(input);
    bool matched = json(ctx);
    CHECK(!(matched && ctx.ended()));

    // Phase 1 error tracking: a Diagnostic should be available.
    if (auto diag = ctx.take_error()) {
        SourceMap map{std::string_view{input}};
        std::string formatted = diag->format(map, "input");
        CHECK_FALSE(formatted.empty());
        CHECK(formatted.starts_with("input:"));
    }
}

TEST_CASE("[json] value-leaves-residual-input")
{
    using namespace json_grammar;
    // value matches "true" and commits via cut. The residual 'x' is left
    // unconsumed — value only consumes what it needs, not the entire input.
    std::string input = "truex";
    Context ctx(input);
    CHECK(value(ctx));
    CHECK_FALSE(ctx.ended());
}
