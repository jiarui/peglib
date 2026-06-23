// ---------------------------------------------------------------------------
// PEG text-grammar extensions: cut (~), lexeme (< e >), recovery (%recover).
//
// These three constructs exist in the C++ combinator API (cut(), lexeme(),
// Rule::set_recovery) since Phases 1/3/4. WS-E adds the textual PEG surface
// for them so GrammarCompiler::from_string() can compile grammars that use
// them.
//
// Coverage:
//   - cut (~): standalone primary, committed-choice behaviour
//   - lexeme (< e >): parses, compiles, forward-compatible (no-op until a
//     future %whitespace directive installs a skipper)
//   - recovery (%recover): set/eof/eol forms, cut-committed non-recovery
//
// The full bootstrap-equivalence check (compiled grammar produces the same
// AST as the C++ meta-grammar) is a planned follow-up; these tests
// validate each construct's parse behaviour independently.
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

// ===========================================================================
// CUT (~) — committed choice
// ===========================================================================

// ---------------------------------------------------------------------------
// Cut is a standalone primary. Used inside an alternative, a failure after
// the cut commits the choice: alternatives after the cut are NOT tried.
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] cut-commits-alternative")
{
    // 'a' ~ 'b' / 'c' — once 'a' matches and cut commits, 'b' MUST match.
    // Input "ax" → 'a' ok, cut commits, 'b' fails on 'x' → hard failure
    // (the 'c' fallback is skipped because of cut). Without cut, 'c' would
    // be tried and the parse would succeed on 'c'-prefixed input.
    auto g = GrammarCompiler::from_string("S <- ('a' ~ 'b') / 'c'");
    CHECK(g.parse_string("ab"));       // 'a' ~ 'b' succeeds
    CHECK(g.parse_string("c"));        // first alt fails on 'a' (no cut), 'c' ok
    CHECK_FALSE(g.parse_string("ax")); // 'a' ~ then 'b' fails → committed failure
    CHECK_FALSE(g.parse_string("ac")); // 'a' ~ commits, 'b' fails on 'c' → no fallback
}

// ---------------------------------------------------------------------------
// Cut as a standalone primary (no surrounding alternative). It parses
// without error and the grammar compiles. With no Alternation/Repetition
// scope to commit, the cut flag is dropped (Context::cut(bool) is a no-op
// on an empty cut stack) — so standalone `~` behaves like `empty`: it
// matches the empty prefix and leaves the rest of the input unconsumed.
// parse_string does not require full consumption unless the grammar anchors
// on EOF, so "x" parses successfully (matching the empty prefix).
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] cut-as-primary-compiles")
{
    // Verifies the meta-grammar accepts `~` as a primary and the compiler
    // emits a DynCutExpr without error. Behaviour: matches empty prefix.
    auto g = GrammarCompiler::from_string("S <- ~");
    CHECK(g.parse_string(""));  // empty input, empty match
    CHECK(g.parse_string("x")); // matches empty prefix; "x" left unconsumed
}

// ---------------------------------------------------------------------------
// Cut inside an unbounded repetition: max<0 path in repeat_parse_impl
// escalates a cut-committed failure to a thrown ParseError.
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] cut-in-repetition")
{
    // ('a' ~ )* 'b' — once 'a' matches and cut commits, the repetition
    // must keep matching 'a' (or hit 'b'). If something else appears, the
    // cut-committed failure throws.
    auto g = GrammarCompiler::from_string("S <- ('a' ~)* 'b'");
    CHECK(g.parse_string("b"));    // zero 'a's, then 'b'
    CHECK(g.parse_string("ab"));   // one 'a', then 'b'
    CHECK(g.parse_string("aaab")); // three 'a's, then 'b'
    // After at least one cut-committed 'a', a non-'a'/non-'b' char fails
    // the committed repetition.
    CHECK_FALSE(g.parse_string("ax"));
}

// ===========================================================================
// LEXEME (< e >) — disable auto-skip
// ===========================================================================

// ---------------------------------------------------------------------------
// Lexeme parses and compiles. With no skipper configured (the
// GrammarCompiler default), lexeme is a no-op — it just wraps the inner
// expression. This test verifies the wrapper doesn't change behaviour.
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] lexeme-no-op-without-skipper")
{
    // < [0-9]+ > — lexeme wraps the inner expression. With no skipper,
    // behaviour is identical to [0-9]+.
    auto g = GrammarCompiler::from_string("N <- < [0-9]+ >");
    CHECK(g.parse_string("12345"));
    CHECK(g.parse_string("0"));
    CHECK_FALSE(g.parse_string(""));
    CHECK_FALSE(g.parse_string("abc"));
}

// ---------------------------------------------------------------------------
// Nested lexeme is a no-op-on-the-flag (skip_enabled is already false).
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] lexeme-nested")
{
    auto g = GrammarCompiler::from_string("N <- < < [0-9]+ > >");
    CHECK(g.parse_string("12345"));
    CHECK_FALSE(g.parse_string("abc"));
}

// ---------------------------------------------------------------------------
// Lexeme disambiguation: `<` must not be confused with `<-` (LEFTARROW).
// A definition using `<-` then a body starting with `< ... >` lexeme must
// parse correctly.
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] lexeme-not-confused-with-leftarrow")
{
    // The `<-` is the LEFTARROW; the `< [a-z]+ >` is a lexeme primary.
    // If the lookahead disambiguation failed, this would not compile.
    auto g = GrammarCompiler::from_string("W <- < [a-z]+ >");
    CHECK(g.parse_string("hello"));
    CHECK_FALSE(g.parse_string("123"));
}

// ===========================================================================
// RECOVERY (%recover) — NonTerminal-level resync
// ===========================================================================

// ---------------------------------------------------------------------------
// %recover({';'}) — sync on a single character. When the body fails, the
// rule scans forward to ';', consumes it, records a diagnostic, and reports
// recovered success.
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] recover-set-single-char")
{
    auto g = GrammarCompiler::from_string("S <- 'x' %recover({';'})");
    // Body succeeds.
    {
        std::string input = "x";
        Context ctx(input);
        CHECK(g.parse("S", ctx));
        CHECK(ctx.ended());
        CHECK(ctx.diagnostics().empty());
    }
    // Body fails at pos 0 ('y'); recovery scans to ';', consumes it.
    {
        std::string input = "y;";
        Context ctx(input);
        CHECK(g.parse("S", ctx));
        CHECK(ctx.mark() == 2); // past ';'
        auto diags = ctx.take_diagnostics();
        REQUIRE(diags.size() == 1);
        CHECK(diags[0].position() == 0);
    }
}

// ---------------------------------------------------------------------------
// %recover({';', '}'}) — sync on multiple characters.
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] recover-set-multi-char")
{
    auto g = GrammarCompiler::from_string("S <- 'x' %recover({';', '}'})");
    // Recovery on the first sync char encountered ('}').
    {
        std::string input = "yy}";
        Context ctx(input);
        CHECK(g.parse("S", ctx));
        CHECK(ctx.mark() == 3);
        CHECK(ctx.take_diagnostics().size() == 1);
    }
    // Recovery on the other sync char (';').
    {
        std::string input = "yy;z";
        Context ctx(input);
        CHECK(g.parse("S", ctx));
        CHECK(ctx.mark() == 3); // past ';', not 'z'
        CHECK(ctx.take_diagnostics().size() == 1);
    }
}

// ---------------------------------------------------------------------------
// %recover(eof) — last-ditch: consume the rest of the input.
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] recover-eof")
{
    auto g = GrammarCompiler::from_string("S <- 'x' %recover(eof)");
    std::string input = "abc"; // no sync token, recover_eof runs to end
    Context ctx(input);
    CHECK(g.parse("S", ctx));
    CHECK(ctx.ended());
    auto diags = ctx.take_diagnostics();
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].position() == 0);
}

// ---------------------------------------------------------------------------
// %recover(eol) — sync on newline.
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] recover-eol")
{
    auto g = GrammarCompiler::from_string("S <- 'x' %recover(eol)");
    std::string input = "abc\ndef";
    Context ctx(input);
    CHECK(g.parse("S", ctx));
    // Scanner stops at '\n' (pos 3), consumes it (pos 4).
    CHECK(ctx.mark() == 4);
    auto diags = ctx.take_diagnostics();
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].position() == 0);
}

// ---------------------------------------------------------------------------
// Recovery respects the diagnostic label: the recorded diagnostic uses the
// rule's name when no explicit label is provided (the meta-grammar uses the
// rule name as the label, matching the C++ API default).
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] recover-uses-rule-name-as-label")
{
    auto g = GrammarCompiler::from_string("MyRule <- 'x' %recover({';'})");
    std::string input = "y;";
    Context ctx(input);
    CHECK(g.parse("MyRule", ctx));
    auto diags = ctx.take_diagnostics();
    REQUIRE(diags.size() == 1);
    // The diagnostic's first expected item carries the rule name as text.
    REQUIRE(!diags[0].expected().empty());
    CHECK(diags[0].expected().begin()->text == "MyRule");
}

// ---------------------------------------------------------------------------
// Cut-committed failures are NOT recovered, even with %recover configured.
// Cut is an explicit programmer commitment that overrides recovery. For the
// cut to commit, it must live inside an Alternation (which pushes the cut
// scope frame); a standalone cut has no scope and is silently dropped.
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] recover-cut-committed-not-recovered")
{
    // Body: ('a' ~ 'b') / 'c' with %recover({';'}). Once 'a' matches and
    // cut commits inside the alternative, 'b' MUST match. On "ax", 'b'
    // fails on 'x' → the cut-committed failure throws internally;
    // Grammar::parse catches and returns false. Recovery does NOT fire
    // because the cut escalation happens before the NonTerminal's recovery
    // hook (the !context.cut() guard in NonTerminal::parse).
    auto g = GrammarCompiler::from_string("S <- (('a' ~ 'b') / 'c') %recover({';'})");
    std::string input = "ax;"; // 'a' matches, cut commits, 'b' fails on 'x'
    Context ctx(input);
    // Cut-committed failure throws internally; Grammar::parse catches and
    // returns false. Recovery must NOT fire.
    CHECK_FALSE(g.parse("S", ctx));
    CHECK(ctx.diagnostics().empty());
}

// ---------------------------------------------------------------------------
// Recovery on a multi-rule grammar: two recovered rules accumulate two
// diagnostics in a single parse.
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] recover-multi-rule-accumulates-diagnostics")
{
    auto g = GrammarCompiler::from_string(R"(
        Digit <- [0-9] %recover({';'})
        Two   <- Digit Digit
    )");
    g.set_start("Two");
    // Input "x;y;" — first Digit fails at pos 0, recovers to ';' (pos 1),
    // consumes → pos 2. Second Digit fails at pos 2 ('y'), recovers to ';'
    // (pos 3), consumes → pos 4. Two diagnostics.
    std::string input = "x;y;";
    Context ctx(input);
    CHECK(g.parse(ctx));
    auto diags = ctx.take_diagnostics();
    CHECK(diags.size() == 2);
    CHECK(diags[0].position() == 0);
    CHECK(diags[1].position() == 2);
}

// ---------------------------------------------------------------------------
// Whitespace inside %recover({ ... }) is tolerated; commas are optional.
// All three spellings below must compile and behave identically.
// ---------------------------------------------------------------------------
TEST_CASE("[pegrule-ext] recover-set-formatting-flexibility")
{
    auto run = [](const std::string& grammar_text) {
        auto g = GrammarCompiler::from_string(grammar_text);
        std::string input = "y;";
        Context ctx(input);
        auto ok = g.parse("S", ctx);
        return std::tuple{ok, ctx.mark(), ctx.take_diagnostics().size()};
    };

    // No spaces, no comma.
    auto [ok1, mark1, ndiag1] = run("S <- 'x' %recover({';'})");
    // Spaces, no comma.
    auto [ok2, mark2, ndiag2] = run("S <- 'x' %recover( { ';'} )");
    // Multi-char with comma.
    auto [ok3, mark3, ndiag3] = run("S <- 'x' %recover({';', '}'})");

    CHECK(ok1);
    CHECK(ok2);
    CHECK(ok3);
    CHECK(mark1 == 2);
    CHECK(mark2 == 2);
    // mark3: 'y' fails at 0, scanner finds ';' at pos 1, consumes → 2.
    CHECK(mark3 == 2);
    CHECK(ndiag1 == 1);
    CHECK(ndiag2 == 1);
    CHECK(ndiag3 == 1);
}
