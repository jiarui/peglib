// ---------------------------------------------------------------------------
// Left-recursion seed-grow across a factored suffix sub-rule.
//
// Regression coverage for the grow-loop fixed-point fix (see NonTerminal.h
// parseImpl): an indirect/mutual left-recursion cycle whose inner head grows a
// suffix in one iteration, then re-parses to a shorter (regressed) result in
// the next, must NOT let the regressed result overwrite the grown one. Before
// the fix, the grown suffix was silently dropped — `a.b` parsed as just `a`.
//
// ORDERING NOTE (PEG ordered choice + seed-grow):
//   peglib's seed-grow respects PEG's ordered-choice semantics. For a
//   left-recursive head to GROW, the recursive (suffix) branches must be tried
//   BEFORE the base case, and a call-style branch must precede a bare-name
//   branch in a shared prefixexp. Otherwise the base/bare branch matches the
//   seed and short-circuits the alternation before the recursive branch can
//   extend it:
//     var           = var_field | var_index | var_name     (suffix FIRST)
//     prefixexp     = functioncall | var | '(' exp ')'     (call FIRST)
//   This mirrors how a real Lua grammar must be written (see yueshi's
//   parser_conv.h, which documents the same rule). Writing base-first here is a
//   GRAMMAR bug, not a library bug — these tests use the correct order.
//
// `Name` matches any ASCII letter, so `a.b`, `a.b.c`, etc. are valid inputs.
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <cctype>
#include <string>

using namespace peg;

namespace
{
// `Name` matches any ASCII letter, so `a.b`, `a.b.c`, etc. are valid inputs.
auto name_pred() { return [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }; }

// Parse `input` from the grammar's start rule; succeed only on full
// consumption (a partial match would mask a "grew only the seed" failure).
bool ok(const Grammar<>& g, std::string input)
{
    Context context(input);
    bool parsed = g.parse(context);
    return parsed && context.ended();
}
} // namespace

// ---------------------------------------------------------------------------
// Baseline: the left-recursive suffix `prefixexp '.' Name` written INLINE
// inside `var`'s alternation. Suffix branches first, call before var.
// ---------------------------------------------------------------------------
TEST_CASE("LR-triangle: inline suffix in var alternation (baseline)")
{
    Grammar<> g;
    g["Name"] = g.terminal(name_pred());
    g["args"] = g.terminal('(') >> g.terminal(')');
    // Suffix productions FIRST in var; the base Name is the LAST alternative.
    g["var"] = (g["prefixexp"] >> g.terminal('.') >> g["Name"])
             | (g["prefixexp"] >> g.terminal('[') >> g["exp"] >> g.terminal(']'))
             | g["Name"];
    g["functioncall"] = (g["prefixexp"] >> g["args"])
                      | (g["prefixexp"] >> g.terminal(':') >> g["Name"] >> g["args"]);
    // functioncall BEFORE var so a call suffix can extend past a bare Name.
    g["prefixexp"] = g["functioncall"] | g["var"] | (g.terminal('(') >> g["exp"] >> g.terminal(')'));
    g["exp"] = g["prefixexp"] | g.terminal('0', '9');
    g["chunk"] = g["exp"];
    g.set_start("chunk");

    CHECK_MESSAGE(ok(g, "a"), "base case");
    CHECK_MESSAGE(ok(g, "a.b"), "one suffix");
    CHECK_MESSAGE(ok(g, "a.b.c"), "two suffixes");
    CHECK_MESSAGE(ok(g, "a()"), "plain call");
    CHECK_MESSAGE(ok(g, "a()()"), "chained call");
}

// ---------------------------------------------------------------------------
// The shape the grow-loop fix targets: identical grammar, but each suffix
// production factored into its own named rule (the typed-fold-required shape,
// where each production needs its own rule so set_action's argument shape is
// fixed). This is the indirect/mutual left-recursion cycle
//   exp → prefixexp → var → var_field → prefixexp
// that previously lost the grown suffix.
// ---------------------------------------------------------------------------
TEST_CASE("LR-triangle: factored suffix sub-rules (typed-fold shape)")
{
    Grammar<> g;
    g["Name"] = g.terminal(name_pred());
    g["args"] = g.terminal('(') >> g.terminal(')');

    g["var_name"]  = g["Name"];
    g["var_field"] = g["prefixexp"] >> g.terminal('.') >> g["Name"];
    g["var_index"] = g["prefixexp"] >> g.terminal('[') >> g["exp"] >> g.terminal(']');

    g["call_plain"]  = g["prefixexp"] >> g["args"];
    g["call_method"] = g["prefixexp"] >> g.terminal(':') >> g["Name"] >> g["args"];

    // Suffix productions FIRST; base Name LAST.
    g["var"]          = g["var_field"] | g["var_index"] | g["var_name"];
    g["functioncall"] = g["call_method"] | g["call_plain"];
    // functioncall BEFORE var (see ordering note at top of file).
    g["prefixexp"]    = g["functioncall"] | g["var"] | (g.terminal('(') >> g["exp"] >> g.terminal(')'));
    g["exp"]          = g["prefixexp"] | g.terminal('0', '9');
    g["chunk"]        = g["exp"];
    g.set_start("chunk");

    CHECK_MESSAGE(ok(g, "a"), "base case still works (seed = Name)");
    CHECK_MESSAGE(ok(g, "a.b"), "one suffix");
    CHECK_MESSAGE(ok(g, "a.b.c"), "two suffixes");
    CHECK_MESSAGE(ok(g, "a[1]"), "index suffix");
    CHECK_MESSAGE(ok(g, "a()"), "plain-call suffix");
    CHECK_MESSAGE(ok(g, "a()()"), "chained-call suffix");
}

// ---------------------------------------------------------------------------
// Minimal 2-rule reduction (drops functioncall). Purest check that a factored
// suffix sub-rule grows: var = var_field | var_name; prefixexp = var.
// ---------------------------------------------------------------------------
TEST_CASE("LR-triangle: minimal factored 2-rule reduction grows")
{
    Grammar<> g;
    g["Name"] = g.terminal(name_pred());
    g["var_name"]  = g["Name"];
    g["var_field"] = g["prefixexp"] >> g.terminal('.') >> g["Name"];
    // Suffix FIRST.
    g["var"]       = g["var_field"] | g["var_name"];
    g["prefixexp"] = g["var"];
    g["exp"]       = g["prefixexp"];
    g["chunk"]     = g["exp"];
    g.set_start("chunk");

    CHECK_MESSAGE(ok(g, "a"), "base case");
    CHECK_MESSAGE(ok(g, "a.b"), "one suffix");
    CHECK_MESSAGE(ok(g, "a.b.c"), "two suffixes");
}
