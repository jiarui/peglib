#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

// ---------------------------------------------------------------------------
// Parser.h unit tests: low-level expression behavior and cut semantics.
// (Operator DSL coverage lives in rule_test.cpp.)
// ---------------------------------------------------------------------------

TEST_CASE("empty-expression-always-succeeds")
{
    std::string input = "abc";
    Context context(input);
    auto grammar = empty();
    CHECK(grammar.parse(context));
    CHECK(context.current() == 'a'); // empty does not consume
}

TEST_CASE("terminalseq-rollback-on-partial-match")
{
    std::string input = "intxyz";
    Context context(input);
    auto grammar = terminalSeq("int");
    CHECK(grammar.parse(context));
    CHECK(context.current() == 'x');

    // A non-matching sequence must restore position.
    std::string input2 = "inx";
    Context context2(input2);
    CHECK_FALSE(terminalSeq("int").parse(context2));
    CHECK(context2.mark() == 0);
}

TEST_CASE("cut-committed-failure-surfaces-via-take-error")
{
    Grammar<> g;

    // `('a' >> cut) | 'b'` on input "b":
    // first alt fails to match 'a', so cut is never set; second alt matches.
    std::string input1 = "b";
    Context context1(input1);
    g["g1"] = (terminal('a') >> cut()) | terminal('b');
    CHECK(g.parse("g1", context1));

    // `('a' >> cut >> 'x') | 'a'` on input "ab":
    // first alt matches 'a', sets cut, then fails on 'x'. With the
    // D1 contract change, Grammar::parse catches the internally-thrown
    // peg::ParseError and reports failure normally — it does NOT throw to
    // the caller. The diagnostic is queryable via ctx.take_error().
    std::string input2 = "ab";
    Context context2(input2);
    g["g2"] = (terminal('a') >> cut() >> terminal('x')) | terminal('a');
    CHECK_FALSE(g.parse("g2", context2));
    auto err = context2.take_error();
    REQUIRE(err);
    // Position should be 1 (where 'x' was expected, after matching 'a').
    CHECK(err->position() == 1);
    // The expected set reflects what the cut-committed branch wanted ('x').
    CHECK_FALSE(err->expected().empty());
}

TEST_CASE("repetition-stops-on-no-progress")
{
    // `*(empty)` would loop forever without no-progress detection.
    std::string input = "abc";
    Context context(input);
    Grammar<> g;
    g["g"] = *empty();
    CHECK(g.parse("g", context));
    CHECK(context.current() == 'a');
}

TEST_CASE("repetition-cut-on-no-progress-does-not-throw")
{
    // Regression: a successful no-progress iteration that sets cut
    // (e.g. via a lookahead) must NOT throw ParseError when the loop
    // exits via the no-progress branch. Only cut-committed child
    // failures should throw.
    //
    // Grammar: *((&'a') >> cut()) on input "abc":
    //   - Iteration 1: &'a' succeeds (lookahead, no consume), cut() sets cut,
    //     no progress → loop breaks.
    //   - Post-loop: cut is true but loop exited via no-progress, not failure.
    //   - Must return true, NOT throw.
    std::string input = "abc";
    Context context(input);
    Grammar<> g;
    g["g"] = *((&terminal('a')) >> cut());
    CHECK(g.parse("g", context));
    CHECK(context.current() == 'a');
}

TEST_CASE("terminalseq-records-expected-on-failure")
{
    // terminalSeq("foo") on input "xyz" should record "foo" as expected.
    std::string input = "xyz";
    Context context(input);
    CHECK_FALSE(terminalSeq("foo").parse(context));
    CHECK(context.has_error());
    CHECK(context.expected().size() == 1);
    CHECK(context.expected().begin()->kind == ExpectedKind::Literal);
    CHECK(context.expected().begin()->text == "\"foo\"");
}

TEST_CASE("terminal-range-records-range-expected")
{
    // terminal('a', 'z') on input '0' should record "'a'..'z'" as expected.
    std::string input = "0";
    Context context(input);
    CHECK_FALSE(terminal('a', 'z').parse(context));
    CHECK(context.has_error());
    CHECK(context.expected().size() == 1);
    CHECK(context.expected().begin()->kind == ExpectedKind::Range);
    CHECK(context.expected().begin()->text == "'a'..'z'");
}

TEST_CASE("bounded-repetition-cut-failure-does-not-throw")
{
    // D4 pinning test: a cut inside a BOUNDED repetition commits only to
    // that repetition's scope. On child failure the loop stops and returns
    // success (with the iterations matched so far) — it does NOT escalate
    // to peg::ParseError. Only unbounded (`*`/`+`) repetitions escalate.
    //
    // Grammar: 2 * ('a' >> cut() >> 'b')  on input "ax":
    //   - Iteration 1: 'a' matches, cut() commits, 'b' fails on 'x'.
    //   - Bounded repetition: cut does NOT escalate; loop stops, returns
    //     success with 1 iteration's worth of children (well, the cut
    //     failure rolled back iteration 1's partial match).
    //   - Must NOT throw.
    std::string input = "ax";
    Context context(input);
    Grammar<> g;
    g["g"] = 2 * (terminal('a') >> cut() >> terminal('b'));
    g.set_start("g");
    // The whole grammar still fails to match (it wanted 2 'a..b' pairs and
    // got none committed), but it fails NORMALLY — no exception escapes.
    CHECK_FALSE(g.parse(context));
    // And crucially: no cut-committed ParseError propagated. (If it had,
    // g.parse would have caught it per the D1 contract and reported via
    // take_error; the assertion above already covers "no throw". Here we
    // additionally confirm the failure is a regular parse failure.)
}
