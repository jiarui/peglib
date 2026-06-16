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
    CHECK(*context.mark() == 'a'); // empty does not consume
}

TEST_CASE("terminalseq-rollback-on-partial-match")
{
    std::string input = "intxyz";
    Context context(input);
    auto grammar = terminalSeq("int");
    CHECK(grammar.parse(context));
    CHECK(*context.mark() == 'x');

    // A non-matching sequence must restore position.
    std::string input2 = "inx";
    Context context2(input2);
    CHECK_FALSE(terminalSeq("int").parse(context2));
    CHECK(context2.mark() == context2.get_input().begin());
}

TEST_CASE("cut-suppresses-alternatives")
{
    // `('a' >> cut) | 'b'` on input "b":
    // first alt fails to match 'a', so cut is never set; second alt matches.
    std::string input1 = "b";
    Context context1(input1);
    Rule<> g1 = (terminal('a') >> cut()) | terminal('b');
    CHECK(g1(context1));

    // `('a' >> cut >> 'x') | 'a'` on input "ab":
    // first alt matches 'a', sets cut, then fails on 'x'; cut suppresses
    // the second alternative, so the overall parse fails.
    std::string input2 = "ab";
    Context context2(input2);
    Rule<> g2 = (terminal('a') >> cut() >> terminal('x')) | terminal('a');
    CHECK_FALSE(g2(context2));
}

TEST_CASE("repetition-stops-on-no-progress")
{
    // `*(empty)` would loop forever without no-progress detection.
    std::string input = "abc";
    Context context(input);
    Rule<> g = *empty();
    CHECK(g(context));
    CHECK(*context.mark() == 'a');
}
