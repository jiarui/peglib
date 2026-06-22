// ---------------------------------------------------------------------------
// JSON grammar with auto-skip — the "after" picture for Phase 3.
//
// Compare with json_test.cpp, which threads `>> g["ws"] >>` manually
// between every pair of terminals (5 occurrences). Here, a single
// set_skipper(g["ws"]) call replaces all of that. Both grammars accept
// the same language; this test proves the skipper-based version handles
// the same whitespace permutations as the manual version.
//
// The grammar is intentionally small (no string-escape depth, no number
// exponent) — the point is the whitespace ergonomics, not full JSON.
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

namespace
{
// Build the auto-skip variant. One ws rule, one set_skipper call, and
// the value/object/array rules are free of inter-token ws noise.
Grammar<char> make_json_skipper_grammar()
{
    Grammar<char> g;

    g["ws"] = *terminal<char>([](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    });

    // No `>> g["ws"] >>` anywhere — the skipper handles it.
    g["number"] = lexeme(+terminal('0', '9'));
    g["string"] = lexeme(terminal('"') >> *(terminal<char>([](char c) {
                         return c != '"' && c != '\\';
                     })) >> terminal('"'));
    g["value"] = g["number"] | g["string"] | g["object"] | g["array"]
                 | terminalSeq("true") | terminalSeq("false") | terminalSeq("null");
    g["member"] = g["string"] >> terminal(':') >> g["value"];
    g["members"] = g["member"] >> *(terminal(',') >> g["member"]);
    g["object"] = terminal('{') >> -g["members"] >> terminal('}');
    g["elements"] = g["value"] >> *(terminal(',') >> g["value"]);
    g["array"] = terminal('[') >> -g["elements"] >> terminal(']');
    g["json"] = g["value"];

    g.set_start("json");
    g.set_skipper(g["ws"]);
    return g;
}
} // namespace

TEST_CASE("json_skipper: compact input parses")
{
    auto g = make_json_skipper_grammar();
    CHECK(g.parse_string(R"({"a":1,"b":[2,3]})"));
    CHECK(g.parse_string(R"([1,2,3])"));
    CHECK(g.parse_string(R"("hello")"));
    CHECK(g.parse_string("42"));
}

TEST_CASE("json_skipper: whitespace-flexible input parses")
{
    auto g = make_json_skipper_grammar();
    // These are the inputs that would FAIL without the skipper (or
    // without the manual `>> ws >>` threading of json_test.cpp).
    CHECK(g.parse_string(R"({ "a" : 1 , "b" : [ 2 , 3 ] })"));
    CHECK(g.parse_string(R"({
        "a": 1,
        "b": [2, 3]
    })"));
    CHECK(g.parse_string(R"(  [ 1 , 2 , 3 ]  )"));
    CHECK(g.parse_string("\t[ 1,\t2 ,3\t]\n"));
}

TEST_CASE("json_skipper: nested objects and arrays with whitespace")
{
    auto g = make_json_skipper_grammar();
    CHECK(g.parse_string(R"(
        {
            "outer": {
                "inner": [ 1, 2, { "deep": 3 } ]
            }
        }
    )"));
}

TEST_CASE("json_skipper: lexeme keeps number digits contiguous")
{
    auto g = make_json_skipper_grammar();
    // parse_string is partial-match: a top-level `value` rule matches
    // the first complete value and returns true, even if trailing input
    // remains. So "12 34" parses as one number "12" + unconsumed " 34".
    // The skipper-relevant property: lexeme stops the number at the
    // space, so "12 34" does NOT become one number "1234".
    CHECK(g.parse_string("12"));         // one number, full match
    CHECK(g.parse_string("12 34"));      // partial match — value="12"

    // Inside an array, elements are comma-separated. The skipper lets
    // whitespace flex around the comma, but does NOT substitute for it.
    // So [12, 34] parses, [12 34] does not (missing comma).
    CHECK(g.parse_string(R"([12, 34])"));
    CHECK(g.parse_string(R"([ 12 , 34 ])"));
    CHECK_FALSE(g.parse_string(R"([12 34])")); // no comma -> not valid JSON

    // And critically: a number's digits stay contiguous. [1 2 3] is
    // three separate one-digit elements (with no commas) — invalid JSON,
    // NOT one number "123".
    CHECK_FALSE(g.parse_string(R"([1 2 3])"));
}

TEST_CASE("json_skipper: has_skipper is true for the auto-skip grammar")
{
    auto g = make_json_skipper_grammar();
    CHECK(g.has_skipper());
}

TEST_CASE("json_skipper: grammar visualises cleanly via to_dot")
{
    // The skipper rule and all value rules appear in the DOT output —
    // confirming that adding a skipper does not pollute the dependency
    // graph (the skipper is referenced by no other rule's body).
    auto g = make_json_skipper_grammar();
    const auto dot = g.to_dot();
    for (const char* name :
         {"ws", "number", "string", "value", "member", "members", "object",
          "elements", "array", "json"}) {
        CHECK(dot.find(std::string{"\""} + name + "\"") != std::string::npos);
    }
    // The skipper rule (ws) is never *referenced* by any rule body — it
    // is invoked only by run_skipper() — so no edge points to it.
    CHECK(dot.find("\"ws\";") != std::string::npos); // node declaration present
    // No edge of the form `X -> "ws"`.
    bool any_edge_to_ws = false;
    std::string::size_type pos = 0;
    while ((pos = dot.find(" -> \"ws\"", pos)) != std::string::npos) {
        any_edge_to_ws = true;
        break;
    }
    CHECK_FALSE(any_edge_to_ws);
}
