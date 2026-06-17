#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

// ---------------------------------------------------------------------------
// Value stack + PegContext concept tests
//
// Covers:
//   - Context<InputSource, NodeType> template parameter
//   - Default NodeType == std::monostate
//   - push_node / pop_node / peek_node / node_count / clear_stack
//   - Semantic action return value pushed onto stack
//   - PegContext concept satisfied by valid Context, rejected by invalid types
// ---------------------------------------------------------------------------

// A simple user-defined AST node for testing.
struct IntNode
{
    int value = 0;
};

TEST_CASE("value-stack-default-node-type-is-monostate")
{
    std::string input = "abc";
    Context context(input); // defaults to Context<..., std::monostate>
    static_assert(std::is_same_v<decltype(context)::node_type, std::monostate>);

    context.push_node(std::monostate{});
    CHECK(context.node_count() == 1);
    (void)context.pop_node();
    CHECK(context.node_count() == 0);
}

TEST_CASE("value-stack-custom-node-type")
{
    using MyContext = Context<std::span<const char>, IntNode>;
    static_assert(std::is_same_v<MyContext::node_type, IntNode>);

    std::string input = "abc";
    MyContext context(input);

    CHECK(context.node_count() == 0);

    context.push_node(IntNode{42});
    CHECK(context.node_count() == 1);
    CHECK(context.peek_node().value == 42);

    context.push_node(IntNode{7});
    CHECK(context.node_count() == 2);
    CHECK(context.peek_node().value == 7);

    IntNode popped = context.pop_node();
    CHECK(popped.value == 7);
    CHECK(context.peek_node().value == 42);

    popped = context.pop_node();
    CHECK(popped.value == 42);
    CHECK(context.node_count() == 0);
}

TEST_CASE("value-stack-clear")
{
    std::string input = "abc";
    Context<std::span<const char>, IntNode> context(input);

    context.push_node(IntNode{1});
    context.push_node(IntNode{2});
    context.push_node(IntNode{3});
    CHECK(context.node_count() == 3);

    context.clear_stack();
    CHECK(context.node_count() == 0);
}

TEST_CASE("value-stack-action-result-pushed")
{
    using MyContext = Context<std::span<const char>, IntNode>;

    std::string input = "42";
    MyContext context(input);

    // Grammar: num = single digit, action returns the digit as IntNode.
    // Construct a TerminalExpr that matches any digit using std::set<char>.
    std::set<char> digits = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
    using DigitTerminal = TerminalExpr<MyContext, std::set<char>>;
    Grammar<MyContext> g;
    g["num"] = DigitTerminal(digits);
    g["num"].set_action([](MyContext& ctx, MyContext::match_range range) {
        char c = *range.begin();
        return IntNode{c - '0'};
    });

    // Parse two digits
    CHECK(context.node_count() == 0);
    CHECK(g.parse("num", context));
    CHECK(g.parse("num", context));
    CHECK(context.node_count() == 2);

    // Last pushed should be the second digit (2)
    CHECK(context.peek_node().value == 2);

    // Pop in LIFO order
    IntNode second = context.pop_node();
    IntNode first = context.pop_node();
    CHECK(second.value == 2);
    CHECK(first.value == 4);
}

TEST_CASE("value-stack-monostate-action-returns-default")
{
    // Default Context: action must return std::monostate.
    using DefaultCtxt = Context<std::span<const char>>;
    std::string input = "a";
    DefaultCtxt context(input);

    Grammar<DefaultCtxt> g;
    g["rule"] = terminal('a');
    g["rule"].set_action(
        [](DefaultCtxt& ctx, DefaultCtxt::match_range range) { return std::monostate{}; });

    CHECK(g.parse("rule", context));
    CHECK(context.node_count() == 1);
    // peek_node returns a const ref to std::monostate — no value to check,
    // but the stack must have one entry.
    (void)context.pop_node();
    CHECK(context.node_count() == 0);
}

// ---------------------------------------------------------------------------
// PegContext concept tests
// ---------------------------------------------------------------------------

TEST_CASE("concept-default-context-satisfies-pegcontext")
{
    using C = Context<std::span<const char>>;
    static_assert(PegContext<C>);
    static_assert(is_peg_context_v<std::span<const char>, std::monostate>);
    CHECK(PegContext<C>);
}

TEST_CASE("concept-custom-context-satisfies-pegcontext")
{
    using C = Context<std::span<const char>, IntNode>;
    static_assert(PegContext<C>);
    CHECK(PegContext<C>);
}

TEST_CASE("concept-int-does-not-satisfy-pegcontext")
{
    static_assert(!PegContext<int>);
    static_assert(!PegContext<std::string>);
    CHECK(!PegContext<int>);
}

TEST_CASE("concept-filesource-context-satisfies-pegcontext")
{
    using C = Context<FileSource<char>>;
    static_assert(PegContext<C>);
    CHECK(PegContext<C>);
}
