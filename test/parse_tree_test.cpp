#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

// ---------------------------------------------------------------------------
// Parse-tree + PegContext concept tests
//
// Originally covered the value-stack API (push_node / pop_node / peek_node /
// node_count / clear_stack). The post-parse-action refactor removed the
// value stack entirely — AST data now flows through the parse tree returned
// by parse() (ParseResult.tree). These tests were rewritten to exercise the
// equivalent behaviour through the parse-tree API.
//
// Covers:
//   - Context<InputSource, NodeType> template parameter
//   - Default NodeType == std::monostate
//   - Action return value stored on ParseTreeNode::value
//   - Parse-tree shape after sequence / alternation / predicates
//   - PegContext concept (applied as a Grammar constraint)
// ---------------------------------------------------------------------------

// A simple user-defined AST node for testing.
struct IntNode
{
    int value = 0;
};

TEST_CASE("value-stack-default-node-type-is-monostate")
{
    std::string input = "a";
    Context context(input); // defaults to Context<..., std::monostate>
    static_assert(std::is_same_v<decltype(context)::node_type, std::monostate>);

    // Default Context produces a tree whose value is a default-constructed
    // std::monostate (no action set → node->value is value-initialised).
    Grammar<> g;
    g["rule"] = terminal('a');
    auto tree = g.parse_tree("rule", context);
    REQUIRE(tree);
    static_assert(std::is_same_v<decltype(tree->value), std::monostate>);
    CHECK(context.ended());
}

TEST_CASE("value-stack-custom-node-type")
{
    using MyContext = Context<std::span<const char>, IntNode>;
    static_assert(std::is_same_v<MyContext::node_type, IntNode>);

    std::string input = "4";
    MyContext context(input);

    using DigitTerm = TerminalExpr<MyContext, std::array<char, 2>>;
    Grammar<MyContext> g;
    g["num"] = DigitTerm({'0', '9'});
    g["num"].set_action([](MyContext& ctx, MyContext::ParseTreeNodePtr node) {
        char c = ctx.get_input()[node->start_offset];
        return IntNode{c - '0'};
    });

    auto tree = g.parse_tree("num", context);
    REQUIRE(tree);
    CHECK(tree->value.value == 4);
}

TEST_CASE("value-stack-clear")
{
    // Originally tested ctx.clear_stack(). The value stack no longer
    // exists; each parse produces an independent ParseResult.tree with no
    // shared mutable state to clear. Verify that successive parses do not
    // interfere with one another's trees.
    using MyContext = Context<std::span<const char>, IntNode>;
    using DigitTerm = TerminalExpr<MyContext, std::array<char, 2>>;

    Grammar<MyContext> g;
    g["num"] = DigitTerm({'0', '9'});
    g["num"].set_action([](MyContext& ctx, MyContext::ParseTreeNodePtr node) {
        char c = ctx.get_input()[node->start_offset];
        return IntNode{c - '0'};
    });

    std::string input1 = "1";
    MyContext ctx1(input1);
    auto tree1 = g.parse_tree("num", ctx1);
    REQUIRE(tree1);
    CHECK(tree1->value.value == 1);

    std::string input2 = "2";
    MyContext ctx2(input2);
    auto tree2 = g.parse_tree("num", ctx2);
    REQUIRE(tree2);
    CHECK(tree2->value.value == 2);

    // tree1 is unaffected by the second parse — no shared stack to clear.
    CHECK(tree1->value.value == 1);
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
    g["num"].set_action([](MyContext& ctx, MyContext::ParseTreeNodePtr node) {
        char c = ctx.get_input()[node->start_offset];
        return IntNode{c - '0'};
    });

    // Parse two digits in succession; each parse yields its own tree whose
    // value is the action's return value.
    auto first = g.parse_tree("num", context);
    REQUIRE(first);
    CHECK(first->value.value == 4);

    auto second = g.parse_tree("num", context);
    REQUIRE(second);
    CHECK(second->value.value == 2);
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
        [](DefaultCtxt& /*ctx*/, DefaultCtxt::ParseTreeNodePtr /*node*/) { return std::monostate{}; });

    auto tree = g.parse_tree("rule", context);
    REQUIRE(tree);
    static_assert(std::is_same_v<decltype(tree->value), std::monostate>);
}

// ---------------------------------------------------------------------------
// PegContext concept tests.
//
// PegContext is applied as a constraint on Grammar's template parameter, so
// a custom Context type that fails to satisfy it is rejected at the Grammar
// instantiation point with a single concept diagnostic. These tests pin the
// behaviour for the shipped Context specializations and confirm the concept
// actually rejects incomplete Context types.
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

// Negative case: a Context type that is missing a required member (here,
// init_cut / remove_cut — the cut stack API) must NOT satisfy PegContext.
// This proves the concept is genuinely restrictive, not vacuously true.
namespace
{
// Minimal stub that wires up the typedefs and a handful of methods but omits
// the cut API deliberately.
struct BadContextNoCut
{
    using iterator = const char*;
    using value_type = char;
    using node_type = std::monostate;
    using match_range = std::span<const char>;
};
} // namespace
static_assert(!PegContext<BadContextNoCut>,
              "a Context missing the cut API must fail PegContext");

// ---------------------------------------------------------------------------
// Parse-tree shape after backtracking combinators.
//
// Originally "Value-stack rollback on backtracking (W2 of Phase 2)".
// When a combinator fails, any value pushed by partially-matched children
// had to be rolled back. In the parse-tree model, each parse() call returns
// its own ParseResult; a failed child contributes no tree to its parent, so
// the equivalent guarantee is: the parent's tree contains exactly the
// successful children (failed siblings contribute nothing).
// ---------------------------------------------------------------------------

TEST_CASE("value-stack-rollback-on-sequence-failure")
{
    using MyContext = Context<std::span<const char>, IntNode>;
    using DigitTerm = TerminalExpr<MyContext, std::array<char, 2>>;
    Grammar<MyContext> g;
    g["digit"] = DigitTerm({'0', '9'});
    g["digit"].set_action([](MyContext& ctx, MyContext::ParseTreeNodePtr node) {
        return IntNode{ctx.get_input()[node->start_offset] - '0'};
    });
    g["nonzero"] = DigitTerm({'1', '9'});
    g["nonzero"].set_action([](MyContext& ctx, MyContext::ParseTreeNodePtr node) {
        return IntNode{ctx.get_input()[node->start_offset] - '0'};
    });
    // "fail_seq" succeeds only on "<digit><nonzero>"; on "11" the second
    // child ('nonzero' expects 1-9; '1' matches) succeeds, but on "1x" the
    // sequence fails — and the digit's node must not contribute to the
    // parent tree (in the old model its IntNode push had to be rolled back).
    g["fail_seq"] = g["digit"] >> g["nonzero"];

    SUBCASE("successful sequence leaves both children in tree")
    {
        std::string input = "12";
        MyContext ctx(input);
        auto tree = g.parse_tree("fail_seq", ctx);
        REQUIRE(tree);
        REQUIRE(tree->children.size() == 2);
        CHECK(tree->children[0]->value.value == 1); // digit '1'
        CHECK(tree->children[1]->value.value == 2); // nonzero '2'
    }
    SUBCASE("failed sequence produces no tree")
    {
        std::string input = "1x";
        MyContext ctx(input);
        CHECK_FALSE(g.parse("fail_seq", ctx));
    }
}

TEST_CASE("value-stack-rollback-on-alternation-failure")
{
    using MyContext = Context<std::span<const char>, IntNode>;
    using ATerm = TerminalExpr<MyContext, char>;
    Grammar<MyContext> g;
    g["a_node"] = ATerm('a');
    g["a_node"].set_action([](MyContext&, MyContext::ParseTreeNodePtr) { return IntNode{1}; });
    g["b_node"] = ATerm('b');
    g["b_node"].set_action([](MyContext&, MyContext::ParseTreeNodePtr) { return IntNode{2}; });
    // Both alternatives produce a node. The point of this test: when the
    // first alternative fails (after partial match), its node is not
    // contributed to the parent tree before the second alternative is tried.
    g["a_or_b"] = g["a_node"] | g["b_node"];
    g["seq_ab"] = g["a_or_b"] >> g["a_or_b"];

    SUBCASE("two successful alternatives leave two children in tree")
    {
        std::string input = "ab";
        MyContext ctx(input);
        auto tree = g.parse_tree("seq_ab", ctx);
        REQUIRE(tree);
        REQUIRE(tree->children.size() == 2);
        // First a_or_b chose a_node (IntNode{1}), second chose b_node (IntNode{2}).
        CHECK(tree->children[0]->value.value == 1);
        CHECK(tree->children[1]->value.value == 2);
    }

    SUBCASE("failed first alternative rolls back before retry")
    {
        // Input "ba": first a_or_b tries a_node, fails ('b'), then tries
        // b_node, succeeds (IntNode{2}). Second a_or_b tries a_node,
        // succeeds (IntNode{1}). Total: 2 children with values {2, 1}.
        std::string input = "ba";
        MyContext ctx(input);
        auto tree = g.parse_tree("seq_ab", ctx);
        REQUIRE(tree);
        REQUIRE(tree->children.size() == 2);
        CHECK(tree->children[0]->value.value == 2);
        CHECK(tree->children[1]->value.value == 1);
    }
}

TEST_CASE("value-stack-rollback-on-not-predicate")
{
    using MyContext = Context<std::span<const char>, IntNode>;
    using ATerm = TerminalExpr<MyContext, char>;
    Grammar<MyContext> g;
    g["a_node"] = ATerm('a');
    g["a_node"].set_action([](MyContext&, MyContext::ParseTreeNodePtr) { return IntNode{1}; });
    // !a_node succeeds (because lookahead doesn't match), and must leave
    // zero children — the child's would-be node is discarded by NotExpr.
    g["not_a"] = !g["a_node"];

    std::string input = "b";
    MyContext ctx(input);
    auto tree = g.parse_tree("not_a", ctx);
    REQUIRE(tree);
    CHECK(tree->children.empty());
}

TEST_CASE("value-stack-rollback-on-and-predicate")
{
    using MyContext = Context<std::span<const char>, IntNode>;
    using ATerm = TerminalExpr<MyContext, char>;
    Grammar<MyContext> g;
    g["a_node"] = ATerm('a');
    g["a_node"].set_action([](MyContext&, MyContext::ParseTreeNodePtr) { return IntNode{1}; });
    // &a_node succeeds (lookahead matches 'a'), and must leave zero children
    // — the child's node is discarded by AndExpr.
    g["and_a"] = &g["a_node"];

    std::string input = "a";
    MyContext ctx(input);
    auto tree = g.parse_tree("and_a", ctx);
    REQUIRE(tree);
    CHECK(tree->children.empty());
}
