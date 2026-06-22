// ---------------------------------------------------------------------------
// Grammar::to_dot() — Graphviz DOT export tests.
//
// The DOT format is structural text; these tests assert on substrings
// rather than exact output, so cosmetic changes (spacing, ordering) do
// not break the suite. What matters is:
//   - digraph header + closing brace
//   - every defined rule appears as a node
//   - the start rule is marked with peripheries=2
//   - rule-reference edges are emitted (via collect_rule_refs)
//   - undefined references appear as dangling edge targets (typo detection)
//   - rule names with DOT-special characters are escaped
//   - an empty / unset-start grammar does not crash
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

TEST_CASE("to_dot: emits digraph with header and closing brace")
{
    Grammar<char> g;
    g["a"] = terminal('a');
    g.set_start("a");

    const auto dot = g.to_dot();
    CHECK(dot.starts_with("digraph peglib_grammar {"));
    CHECK(dot.find("}\n", dot.size() - 2) != std::string::npos);
}

TEST_CASE("to_dot: every defined rule appears as a node")
{
    Grammar<char> g;
    g["a"] = terminal('a');
    g["b"] = terminal('b');
    g["c"] = terminal('c');
    g.set_start("a");

    const auto dot = g.to_dot();
    CHECK(dot.find("\"a\"") != std::string::npos);
    CHECK(dot.find("\"b\"") != std::string::npos);
    CHECK(dot.find("\"c\"") != std::string::npos);
}

TEST_CASE("to_dot: start rule marked with peripheries=2")
{
    Grammar<char> g;
    g["start"] = terminal('s');
    g["other"] = terminal('o');
    g.set_start("start");

    const auto dot = g.to_dot();
    // The start rule gets the double-border marker.
    CHECK(dot.find("\"start\" [peripheries=2]") != std::string::npos);
    // Non-start rules do not.
    CHECK(dot.find("\"other\" [peripheries=2]") == std::string::npos);
}

TEST_CASE("to_dot: rule references become edges")
{
    Grammar<char> g;
    // expr references term; term references factor.
    g["factor"] = terminal('0', '9');
    g["term"] = g["factor"];
    g["expr"] = g["term"];
    g.set_start("expr");

    const auto dot = g.to_dot();
    CHECK(dot.find("\"expr\" -> \"term\"") != std::string::npos);
    CHECK(dot.find("\"term\" -> \"factor\"") != std::string::npos);
}

TEST_CASE("to_dot: recursive rule emits a self-loop edge")
{
    Grammar<char> g;
    // Forward-declare then assign — the only safe form for self-reference.
    g["a"] = terminal('x');
    g["a"] = terminal('x') >> g["a"];
    g.set_start("a");

    const auto dot = g.to_dot();
    CHECK(dot.find("\"a\" -> \"a\"") != std::string::npos);
}

TEST_CASE("to_dot: undefined reference appears as dangling edge target")
{
    Grammar<char> g;
    // Reference a rule that is never defined — useful for spotting typos.
    // NB: g["typo"] below lazily inserts "typo" as a forward-declared rule
    // (body == nullptr, so is_defined() == false). to_dot must still emit
    // the edge, but must NOT emit a standalone node declaration line for
    // "typo" (only defined rules get node lines).
    g["a"] = g["typo"];
    g.set_start("a");

    const auto dot = g.to_dot();
    // The edge is emitted; the target name appears between quotes.
    CHECK(dot.find("\"a\" -> \"typo\"") != std::string::npos);
    // The rule "typo" is undefined, so it must not get its own node
    // declaration. A node declaration is "  \"name\";" or "  \"name\" [...];"
    // at the start of a line (2-space indent + quote). The edge line
    // "  \"a\" -> \"typo\";" also contains "typo\";", so a naive substring
    // search would false-positive. Instead, check that "typo" does not
    // appear as the leading token of any line.
    bool typo_has_node_line = false;
    size_t pos = 0;
    while ((pos = dot.find("\n  \"typo\"", pos)) != std::string::npos) {
        // After the quoted name, the next non-space char should be ';' or
        // '[' for a node declaration, or '-' for an edge (->). Node decl
        // iff ';"' or '['.
        size_t after = pos + std::string{"\n  \"typo\""}.size();
        // Skip past the closing quote.
        if (after < dot.size() && dot[after] == ';') {
            typo_has_node_line = true;
            break;
        }
        if (after < dot.size() && dot[after] == ' ') {
            // Could be " [peripheries=2]" (node) or " ->" (edge).
            size_t k = after;
            while (k < dot.size() && dot[k] == ' ')
                ++k;
            if (k < dot.size() && (dot[k] == '[' || dot[k] == ';')) {
                typo_has_node_line = true;
                break;
            }
        }
        ++pos;
    }
    CHECK_FALSE(typo_has_node_line);
}

TEST_CASE("to_dot: special characters in rule names are escaped")
{
    Grammar<char> g;
    // Rule names containing DOT-special characters.
    g["quote\"name"] = terminal('a');
    g["back\\slash"] = terminal('b');
    g.set_start("quote\"name");

    const auto dot = g.to_dot();
    // " should be emitted as \" inside the DOT string.
    CHECK(dot.find("\"quote\\\"name\"") != std::string::npos);
    // Backslash should be emitted as \\.
    CHECK(dot.find("\"back\\\\slash\"") != std::string::npos);
}

TEST_CASE("to_dot: empty grammar does not crash")
{
    Grammar<char> g;
    const auto dot = g.to_dot();
    // Still emits a valid (empty) digraph body.
    CHECK(dot.starts_with("digraph peglib_grammar {"));
    CHECK(dot.find("}\n", dot.size() - 2) != std::string::npos);
}

TEST_CASE("to_dot: start rule unset still produces valid output")
{
    Grammar<char> g;
    g["a"] = terminal('a');
    // No set_start — no rule should get peripheries=2.
    const auto dot = g.to_dot();
    CHECK(dot.find("\"a\";") != std::string::npos);
    CHECK(dot.find("[peripheries=2]") == std::string::npos);
}

TEST_CASE("to_dot: mutually recursive rules render fully")
{
    Grammar<char> g;
    g["even"] = terminal('0') | (terminal('1') >> g["odd"]);
    g["odd"] = terminal('1') | (terminal('0') >> g["even"]);
    g.set_start("even");

    const auto dot = g.to_dot();
    CHECK(dot.find("\"even\" -> \"odd\"") != std::string::npos);
    CHECK(dot.find("\"odd\" -> \"even\"") != std::string::npos);
}

TEST_CASE("to_dot: unreachable defined rules still render as nodes")
{
    // Dead code should be visible in the diagram for inspection, not
    // silently dropped. to_dot visits every defined rule, not just those
    // reachable from start.
    Grammar<char> g;
    g["used"] = terminal('a');
    g["dead"] = terminal('b');
    g.set_start("used");

    const auto dot = g.to_dot();
    CHECK(dot.find("\"used\"") != std::string::npos);
    CHECK(dot.find("\"dead\"") != std::string::npos);
}
