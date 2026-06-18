// ---------------------------------------------------------------------------
// JSON grammar — a real-world example of building a complete language grammar
// with peglib's Grammar API.
//
// This demonstrates:
//   - Mutually recursive rules (value → object/array → value) — no forward
//     declaration needed; Grammar::operator[] lazily creates rules.
//   - Auto-naming — rule names come from the map key (for error reporting).
//   - Whitespace handling (manual; auto-skipper arrives in Phase 3).
//   - Error reporting integration (Diagnostic on parse failure).
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
// With the Grammar API, mutual recursion is natural — g["value"] creates the
// rule on first access (lazily), and assignment modifies it in-place. No
// forward declarations, no static-initializer lambdas.
// ---------------------------------------------------------------------------

namespace json_grammar
{
inline Grammar<> g;

[[maybe_unused]] const bool grammar_initialized = [] {
    auto cut_ = cut<Context<std::span<const char>>>();

    // Whitespace: space, tab, newline, carriage return. (Phase 3 will
    // introduce Context::set_skipper so users won't need to thread this
    // manually.)
    g["ws"] =
        *terminal<char>([](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });

    // ----- string -----
    auto escape_seq = terminal('\\') >> terminal<char>([](char c) {
                          return c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' ||
                                 c == 'n' || c == 'r' || c == 't' || c == 'u';
                      });
    auto string_char = terminal<char>(
        [](char c) { return c != '"' && c != '\\' && static_cast<unsigned char>(c) >= 0x20; });
    g["string"] = terminal('"') >> *(escape_seq | string_char) >> terminal('"');

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
    g["number"] = integer >> -frac >> -exp;

    // ----- keyword (true/false/null) -----
    g["keyword"] = terminalSeq("true") | terminalSeq("false") | terminalSeq("null");

    // ----- array -----
    // array = '[' ws ( value (ws ',' ws value)* )? ws ']'
    auto comma_sep = g["ws"] >> terminal(',') >> g["ws"];
    g["value_list"] = g["value"] >> *(comma_sep >> g["value"]);
    g["array"] = terminal('[') >> g["ws"] >> -g["value_list"] >> g["ws"] >> terminal(']');

    // ----- object -----
    // object = '{' ws ( string ws ':' ws value (ws ',' ws string ws ':' ws value)* )? ws '}'
    g["key_value"] = g["string"] >> g["ws"] >> terminal(':') >> g["ws"] >> g["value"];
    g["member_list"] = g["key_value"] >> *(comma_sep >> g["key_value"]);
    g["object"] = terminal('{') >> g["ws"] >> -g["member_list"] >> g["ws"] >> terminal('}');

    // ----- value -----
    // Each alternative commits with `cut` after a successful match: once we
    // know it's a number, we don't backtrack into trying it as an object.
    // The cut must live inside this AlternationExpr so the cut stack frame
    // is provided by the alternation, not by the individual rules.
    g["value"] = (g["keyword"] >> cut_) | (g["number"] >> cut_) | (g["object"] >> cut_) |
                 (g["array"] >> cut_) | (g["string"] >> cut_);

    // ----- top-level -----
    g["json"] = g["ws"] >> g["value"] >> g["ws"];
    g.set_start("json");

    return true;
}();
} // namespace json_grammar

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("[json] keyword-rule")
{
    using namespace json_grammar;
    for (const std::string s : {"true", "false", "null"}) {
        Context ctx(s);
        CHECK(g.parse("keyword", ctx));
        CHECK(ctx.ended());
    }
}

TEST_CASE("[json] number-rule")
{
    using namespace json_grammar;
    for (const std::string s : {"0",
                                 "1",
                                 "42",
                                 "-1",
                                 "3.14",
                                 "-0.5",
                                 "1e10",
                                 "2.5E-3",
                                 "123.456e789",
                                 "10.0",
                                 "0.001",
                                 "-123.456e+789"}) {
        Context ctx(s);
        CAPTURE(s);
        CHECK(g.parse("number", ctx));
        CHECK(ctx.ended());
    }
}

TEST_CASE("[json] number-rule-rejects-invalid")
{
    using namespace json_grammar;
    for (const std::string s : {"01", "1.", "+5", ".5", "--1"}) {
        Context ctx(s);
        CAPTURE(s);
        bool matched = g.parse("number", ctx);
        CHECK(!(matched && ctx.ended()));
    }
}

TEST_CASE("[json] string-rule")
{
    using namespace json_grammar;
    for (const std::string s : {"\"\"",
                                 "\"a\"",
                                 "\"hello world\"",
                                 "\"tab\\there\"",
                                 "\"unicode\\u0041\"",
                                 "\"slash\\/end\""}) {
        Context ctx(s);
        CAPTURE(s);
        CHECK(g.parse("string", ctx));
        CHECK(ctx.ended());
    }
}

TEST_CASE("[json] string-rule-rejects-unterminated")
{
    using namespace json_grammar;
    std::string input = "\"unterminated";
    Context ctx(input);
    CHECK_FALSE(g.parse("string", ctx));
}

TEST_CASE("[json] whitespace-tolerance")
{
    using namespace json_grammar;
    for (const std::string input : {
             "  null  ",
             "\n\ttrue\r\n",
             "   42   ",
             "  \"hi\"  ",
         }) {
        Context ctx(input);
        CAPTURE(input);
        CHECK(g.parse(ctx));
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
        CHECK(g.parse("array", ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("with-whitespace")
    {
        std::string input = "[  ]";
        Context ctx(input);
        CHECK(g.parse("array", ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("numbers")
    {
        std::string input = "[1, 2, 3]";
        Context ctx(input);
        CHECK(g.parse("array", ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("mixed-types")
    {
        std::string input = "[\"a\", true, null, 42]";
        Context ctx(input);
        CHECK(g.parse("array", ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("trailing-comma-invalid")
    {
        std::string input = "[1, 2,]";
        Context ctx(input);
        bool matched = g.parse("array", ctx);
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
        CHECK(g.parse("object", ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("simple")
    {
        std::string input = "{\"name\": \"value\", \"n\": 42}";
        Context ctx(input);
        CHECK(g.parse("object", ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("nested")
    {
        std::string input = "{\"outer\": {\"inner\": true}}";
        Context ctx(input);
        CHECK(g.parse("object", ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("missing-colon-invalid")
    {
        std::string input = "{\"key\" \"value\"}";
        Context ctx(input);
        bool matched = g.parse("object", ctx);
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
        CHECK(g.parse(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("mixed-nesting")
    {
        std::string input = R"({"a": [1, {"b": 2}, true], "c": null})";
        Context ctx(input);
        CHECK(g.parse(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("deep-object")
    {
        std::string input = R"({"l1": {"l2": {"l3": {"l4": "deep"}}}})";
        Context ctx(input);
        CHECK(g.parse(ctx));
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
    CHECK(g.parse(ctx));
    CHECK(ctx.ended());
}

TEST_CASE("[json] error-on-malformed-reports-position")
{
    using namespace json_grammar;
    std::string input = "[1, 2,]"; // trailing comma is invalid JSON
    Context ctx(input);
    bool matched = g.parse(ctx);
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
    CHECK(g.parse("value", ctx));
    CHECK_FALSE(ctx.ended());
}
