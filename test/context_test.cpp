#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

// ---------------------------------------------------------------------------
// Context.h unit tests: position tracking, state save/restore, cut lifecycle.
// ---------------------------------------------------------------------------

TEST_CASE("context-basic-position")
{
    std::string input = "hello";
    Context context(input);

    CHECK_FALSE(context.ended());
    CHECK(*context.mark() == 'h');

    context.next();
    CHECK(*context.mark() == 'e');

    for (int i = 0; i < 10; ++i)
        context.next();
    CHECK(context.ended());
}

TEST_CASE("context-state-save-restore")
{
    std::string input = "abcdef";
    Context context(input);

    context.next();
    context.next();
    CHECK(*context.mark() == 'c');

    auto s = context.state();
    context.next();
    context.next();
    CHECK(*context.mark() == 'e');

    context.state(s);
    CHECK(*context.mark() == 'c');
}

TEST_CASE("context-reset-clamped-by-last-cut")
{
    std::string input = "abcdef";
    Context context(input);

    // Initially last_cut == begin, so reset anywhere in [begin, end] is OK.
    context.next();
    auto p = context.mark();
    context.reset(p);
    CHECK(*context.mark() == 'b');
}

TEST_CASE("context-cut-stack-lifecycle")
{
    std::string input = "abc";
    Context context(input);

    // Default: no cut record has been pushed yet, but the parser expressions
    // push one on alternation/repetition. Here we only exercise the public
    // API surface to make sure init/remove are balanced.
    context.init_cut();
    CHECK_FALSE(context.cut());
    context.cut(true);
    CHECK(context.cut());
    context.remove_cut();
}

TEST_CASE("context-get-input-returns-stable-reference")
{
    std::string input = "xyz";
    Context context(input);

    const auto& in1 = context.get_input();
    const auto& in2 = context.get_input();
    CHECK(in1.data() == in2.data());
    CHECK(in1.size() == 3);
}
