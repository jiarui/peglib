// ---------------------------------------------------------------------------
// Regression: shared_ptr cycle leak in recursive grammars.
//
// Discovered Phase 2 W3 (GrammarCompiler). Before the X4 redesign, recursive
// rules formed a shared_ptr cycle that prevented destruction:
//
//   NonTerminal.m_rule → body tree → RuleProxy.m_rule
//     → shared_ptr<NonTerminal>  ← cycle back to start
//
// The X4 redesign (Rule is now a non-owning handle storing a bare
// NonTerminal*) eliminates the cycle at the source. `~Grammar()` needs no
// special handling — the map releases all shared_ptrs and everything dies.
//
// This TU compiles + destroys N recursive grammars in a loop so that a leak
// regression is observable at process exit. Under ASan (detect_leaks=1,
// already set in the CI sanitizer job), any leak here fails the job. Without
// sanitizers, the test still guards against crashes/hangs from
// create/destroy churn.
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

// ---------------------------------------------------------------------------
// Baseline loop counts. Bounded so the test runs fast; large enough that
// any leak is unmistakable under ASan.
// ---------------------------------------------------------------------------
TEST_CASE("[lifecycle] recursive-grammar-no-cycle")
{
    SUBCASE("textual-grammar path (GrammarCompiler)")
    {
        // 100 self-recursive grammars compiled from PEG text.
        // Before X4: each one leaked its NonTerminal + body.
        // After X4: no cycle, no leak.
        for (int i = 0; i < 100; ++i) {
            auto g = GrammarCompiler::from_string("A <- A 'x' / 'y'");
            g.set_start("A");
            // Exercise the last one so we know the grammar still works.
            // Parse both the base case ('y') and the recursive arm
            // ('yxx' -> 'y' then 'x' then 'x') so the recursive path is
            // actually exercised by the leak regression.
            if (i == 99) {
                CHECK(g.parse_string("y"));
                CHECK(g.parse_string("yxx"));
            }
        }
    }

    SUBCASE("static-grammar-API path (primary user API)")
    {
        // The primary API (g["add"] = g["add"] ...) forms a cycle via the
        // static AlternationExpr / SequenceExpr tree, which held RuleProxy
        // copies by value. With X4, these are now Rule copies holding bare
        // NonTerminal* — no shared_ptr cycle.
        for (int i = 0; i < 100; ++i) {
            Grammar<> g;
            g["num"] = +terminal('0', '9');
            g["mul"] = g["mul"] >> '*' >> g["num"] | g["num"];
            g["add"] = g["add"] >> '+' >> g["mul"] | g["mul"];
            g.set_start("add");
            if (i == 99) {
                CHECK(g.parse_string("1+2*3"));
            }
        }
    }

    SUBCASE("mutual recursion across rules")
    {
        for (int i = 0; i < 100; ++i) {
            Grammar<> g;
            g["A"] = g["B"] | terminal('x');
            g["B"] = g["A"] | terminal('y');
            g.set_start("A");
            if (i == 99) {
                CHECK(g.parse_string("x"));
                CHECK(g.parse_string("y"));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Debug lifetime aid: ~Grammar() poisons each NonTerminal's body before
// release so a dangling Rule handle fails fast (assert in debug, use-after-
// free under ASan) rather than silently calling into freed memory.
//
// We can't directly assert on the assert/abort (process-level), so this test
// verifies the observable mechanism: clear_body_for_debug() makes
// is_defined() flip to false, which is what drives the assert in parseImpl.
// Runs in all build types; the actual poisoning in ~Grammar is debug-only.
// ---------------------------------------------------------------------------
TEST_CASE("[lifecycle] clear-body-for-debug-makes-rule-undefined")
{
    Grammar<> g;
    auto handle = g["rule"];
    handle = +terminal('a');
    CHECK(handle.is_defined());

    // Poison the body as ~Grammar would in a debug build.
    handle.impl()->clear_body_for_debug();
    CHECK_FALSE(handle.is_defined());
}
