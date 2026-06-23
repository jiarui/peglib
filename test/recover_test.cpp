// ---------------------------------------------------------------------------
// Error recovery tests.
//
// Recovery is a NonTerminal-boundary mechanism: when a rule with a RecoverSpec
// set fails (and no cut is committed), it scans forward to the next sync
// token, records a diagnostic at the failure position, consumes the sync
// token, and reports success with a transparent null tree. Parsing then
// continues — a single malformed construct does not abort the whole file.
//
// Covers:
//   - Basic single-rule recovery
//   - Multi-diagnostic accumulation across two recovered failures
//   - Cut-committed failures are NOT recovered
//   - Sync token at EOF (recover_eof)
//   - Recovered node is transparent (no tree)
//   - Both API forms: Rule::set_recovery and peg::recover sugar
//   - Predicate-based sync via recover_predicate
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

// ---------------------------------------------------------------------------
// Basic recovery: rule fails, scanner skips to sync token, parse continues.
// ---------------------------------------------------------------------------
TEST_CASE("recover-basic-resync-to-semicolon")
{
    Grammar<> g;
    // "digit" matches a single digit. With recovery on ';', failing input
    // like "abc;" should resync to ';' and report success.
    g["digit"] = terminal('0', '9');
    g["digit"].set_recovery(recover_set<char>({';'}));

    std::string input = "abc;";
    Context ctx(input);
    CHECK(g.parse("digit", ctx));
    // Position should be just past the sync token ';'.
    CHECK(ctx.mark() == 4);
    // A diagnostic should have been recorded at the original failure pos.
    auto diags = ctx.take_diagnostics();
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].position() == 0);
}

// ---------------------------------------------------------------------------
// Multi-diagnostic: two failing rules both recover → two diagnostics.
// ---------------------------------------------------------------------------
TEST_CASE("recover-multi-diagnostic-two-failures")
{
    Grammar<> g;
    g["digit"] = terminal('0', '9');
    g["digit"].set_recovery(recover_set<char>({';'}, "digit"));

    // Sequence: two digits in a row, both recovering on ';'.
    g["two"] = g["digit"] >> g["digit"];
    g.set_start("two");

    // Input "x;y;" — first digit fails at pos 0, recovers to ';' at pos 1,
    // consumes it → pos 2. Second digit fails at pos 2 ('y'), recovers to
    // ';' at pos 3, consumes it → pos 4. Two diagnostics, both before EOF.
    std::string input = "x;y;";
    Context ctx(input);
    CHECK(g.parse(ctx));

    auto diags = ctx.take_diagnostics();
    CHECK(diags.size() == 2);
    CHECK(diags[0].position() == 0);
    CHECK(diags[1].position() == 2);
}

// ---------------------------------------------------------------------------
// Cut-committed failures must NOT be recovered. Cut is an explicit
// programmer commitment that the branch must succeed. This requires the
// cut to live inside an Alternation (which pushes the cut scope frame).
// ---------------------------------------------------------------------------
TEST_CASE("recover-cut-committed-failure-not-recovered")
{
    Grammar<> g;
    // 'a' commit 'b' / fallback. Once 'a' matches and cut commits, 'b' MUST
    // match — the failure escalates to a hard ParseError. Recovery must not
    // override that commitment.
    g["ab"] = (terminal('a') >> cut() >> terminal('b')) | terminal('c');
    g["ab"].set_recovery(recover_set<char>({';'}));

    std::string input = "ax;"; // 'a' matches, cut commits, 'b' fails on 'x'
    Context ctx(input);
    // The cut-committed failure throws internally; Grammar::parse catches
    // ParseError and returns false. Recovery must NOT fire — the input was
    // committed and a hard error is the correct outcome.
    CHECK_FALSE(g.parse("ab", ctx));
    // No diagnostics recorded (recovery did not run).
    CHECK(ctx.diagnostics().empty());
}

// ---------------------------------------------------------------------------
// Sync token at EOF: recover_eof consumes the rest of the input.
// ---------------------------------------------------------------------------
TEST_CASE("recover-eof-consumes-rest-of-input")
{
    Grammar<> g;
    g["digit"] = terminal('0', '9');
    g["digit"].set_recovery(recover_eof<char>());

    std::string input = "abc"; // no sync token anywhere
    Context ctx(input);
    CHECK(g.parse("digit", ctx));
    // recover_eof reaches the end without finding a sync token; the whole
    // remainder is consumed and the rule reports recovered success.
    CHECK(ctx.ended());

    auto diags = ctx.take_diagnostics();
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].position() == 0);
}

// ---------------------------------------------------------------------------
// Recovered node is transparent: parent Sequence's children count is
// unaffected by a recovered child (it produces a null tree).
// ---------------------------------------------------------------------------
TEST_CASE("recover-node-is-transparent")
{
    Grammar<> g;
    g["digit"] = terminal('0', '9');
    g["digit"].set_recovery(recover_set<char>({';'}));

    // Build a sequence whose first child will recover; the sequence's own
    // tree should not contain a child for the recovered digit (its tree
    // is null and gets skipped by SequenceExpr).
    g["seq"] = g["digit"] >> terminal('z');
    g.set_start("seq");

    // Input "x;z" — digit fails (recovers to ';'), then 'z' fails (no 'z').
    // The whole parse fails, but the point is the recovered digit produced
    // no child tree. Verify via a parse_tree call on a grammar where the
    // sequence can succeed:
    Grammar<> g2;
    g2["digit"] = terminal('0', '9');
    g2["digit"].set_recovery(recover_set<char>({';'}));
    // After recover consumes ';', position is at end. No further children.
    g2.set_start("digit");

    std::string input = "x;";
    Context ctx(input);
    auto tree = g2.parse_tree("digit", ctx);
    // Recovered success returns null tree (transparent).
    CHECK(tree == nullptr);
    // And the rule-level parse still succeeded.
    CHECK(ctx.mark() == 2);
}

// ---------------------------------------------------------------------------
// Both API forms: Rule::set_recovery and peg::recover produce identical
// behaviour.
// ---------------------------------------------------------------------------
TEST_CASE("recover-both-api-forms-equivalent")
{
    auto run = [](bool use_sugar) {
        Grammar<> g;
        g["digit"] = terminal('0', '9');
        if (use_sugar) {
            recover(g["digit"], recover_set<char>({';'}));
        } else {
            g["digit"].set_recovery(recover_set<char>({';'}));
        }
        std::string input = "x;";
        Context ctx(input);
        auto ok = g.parse("digit", ctx);
        return std::tuple{ok, ctx.mark(), ctx.take_diagnostics().size()};
    };

    auto [ok1, mark1, ndiag1] = run(false);
    auto [ok2, mark2, ndiag2] = run(true);
    CHECK(ok1 == ok2);
    CHECK(mark1 == mark2);
    CHECK(ndiag1 == ndiag2);
    CHECK(ok1);
    CHECK(mark1 == 2);
    CHECK(ndiag1 == 1);
}

// ---------------------------------------------------------------------------
// Predicate-based sync: recover_predicate covers custom conditions like
// "semicolon OR newline".
// ---------------------------------------------------------------------------
TEST_CASE("recover-predicate-custom-condition")
{
    Grammar<> g;
    g["digit"] = terminal('0', '9');
    g["digit"].set_recovery(recover_predicate<char>([](char c) { return c == ';' || c == '\n'; }));

    // Recovery should stop at the first ';' or '\n', whichever comes first.
    std::string input = "abc\ndef";
    Context ctx(input);
    CHECK(g.parse("digit", ctx));
    // Scanner stops at '\n' (pos 3), consumes it (pos 4).
    CHECK(ctx.mark() == 4);

    auto diags = ctx.take_diagnostics();
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].position() == 0);
}
