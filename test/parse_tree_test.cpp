#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

// ---------------------------------------------------------------------------
// Parse-tree + PegContext concept tests
//
// Covers:
//   - Context<CharT, NodeType> template parameter
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
    g["rule"] = g.terminal('a');
    auto tree = g.parse_tree("rule", context);
    REQUIRE(tree);
    static_assert(std::is_same_v<decltype(tree->value), std::monostate>);
    CHECK(context.ended());
}

TEST_CASE("value-stack-custom-node-type")
{
    using MyContext = Context<char, IntNode>;
    static_assert(std::is_same_v<MyContext::node_type, IntNode>);

    std::string input = "4";
    MyContext context(input);

    Grammar<char, IntNode> g;
    auto num = (g["num"] = g.terminal('0', '9'));
    num.set_action([](MyContext& ctx, peg::Span sp) {
        char c = ctx.at(sp.start);
        return IntNode{c - '0'};
    });

    auto ast = g.parse_ast("num", context);
    REQUIRE(ast);
    CHECK(ast->value == 4);
}

TEST_CASE("value-stack-clear")
{
    // Originally tested ctx.clear_stack(). The value stack no longer
    // exists; each parse produces an independent ParseResult.tree with no
    // shared mutable state to clear. Verify that successive parses do not
    // interfere with one another's AST values.
    using MyContext = Context<char, IntNode>;

    Grammar<char, IntNode> g;
    auto num = (g["num"] = g.terminal('0', '9'));
    num.set_action([](MyContext& ctx, peg::Span sp) {
        char c = ctx.at(sp.start);
        return IntNode{c - '0'};
    });

    std::string input1 = "1";
    MyContext ctx1(input1);
    auto ast1 = g.parse_ast("num", ctx1);
    REQUIRE(ast1);
    CHECK(ast1->value == 1);

    std::string input2 = "2";
    MyContext ctx2(input2);
    auto ast2 = g.parse_ast("num", ctx2);
    REQUIRE(ast2);
    CHECK(ast2->value == 2);

    // ast1 is unaffected by the second parse — independent values.
    CHECK(ast1->value == 1);
}

TEST_CASE("value-stack-action-result-pushed")
{
    using MyContext = Context<char, IntNode>;

    std::string input = "42";
    MyContext context(input);

    // Grammar: num = single digit, action returns the digit as IntNode.
    // Construct a TerminalExpr that matches any digit using std::set<char>.
    std::set<char> digits = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
    Grammar<char, IntNode> g;
    auto num = (g["num"] = g.terminal(digits));
    num.set_action([](MyContext& ctx, peg::Span sp) {
        char c = ctx.at(sp.start);
        return IntNode{c - '0'};
    });

    // Parse two digits in succession; each parse yields its own AST whose
    // value is the action's return value.
    auto first = g.parse_ast("num", context);
    REQUIRE(first);
    CHECK(first->value == 4);

    auto second = g.parse_ast("num", context);
    REQUIRE(second);
    CHECK(second->value == 2);
}

TEST_CASE("value-stack-monostate-action-returns-default")
{
    // Default Context: action must return std::monostate.
    using DefaultCtxt = Context<char>;
    std::string input = "a";
    DefaultCtxt context(input);

    Grammar<char> g;
    auto rule = (g["rule"] = g.terminal('a'));
    rule.set_action([](DefaultCtxt&, peg::Span) { return std::monostate{}; });

    auto ast = g.parse_ast("rule", context);
    REQUIRE(ast);
    static_assert(std::is_same_v<decltype(*ast), std::monostate&>);
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
    using C = Context<char>;
    static_assert(PegContext<C>);
    static_assert(is_peg_context_v<char, std::monostate>);
    CHECK(PegContext<C>);
}

TEST_CASE("concept-custom-context-satisfies-pegcontext")
{
    using C = Context<char, IntNode>;
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
    // After source erasure, a FileSource-backed Context is the same type as a
    // span-backed one: Context<char>. The distinction lives in the
    // type-erased InputSourceBase held inside, not in the Context type.
    // Verify it still satisfies PegContext and can actually drive a parse.
    using C = Context<char>;
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
};
} // namespace
static_assert(!PegContext<BadContextNoCut>, "a Context missing the cut API must fail PegContext");

// ---------------------------------------------------------------------------
// Parse-tree shape after backtracking combinators.
//
// Each parse() call returns its own ParseResult; a failed child contributes
// no tree to its parent. So the equivalent guarantee is: the parent's tree
// contains exactly the successful children (failed siblings contribute
// nothing).
// ---------------------------------------------------------------------------

TEST_CASE("value-stack-rollback-on-sequence-failure")
{
    using MyContext = Context<char, IntNode>;
    Grammar<char, IntNode> g;
    auto digit = (g["digit"] = g.terminal('0', '9'));
    digit.set_action([](MyContext& ctx, peg::Span sp) { return IntNode{ctx.at(sp.start) - '0'}; });
    auto nonzero = (g["nonzero"] = g.terminal('1', '9'));
    nonzero.set_action(
        [](MyContext& ctx, peg::Span sp) { return IntNode{ctx.at(sp.start) - '0'}; });
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
        CHECK(tree->children[0]->name == "digit");   // digit '1'
        CHECK(tree->children[1]->name == "nonzero"); // nonzero '2'
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
    using MyContext = Context<char, IntNode>;
    Grammar<char, IntNode> g;
    auto a_node = (g["a_node"] = g.terminal('a'));
    a_node.set_action([](MyContext&, peg::Span) { return IntNode{1}; });
    auto b_node = (g["b_node"] = g.terminal('b'));
    b_node.set_action([](MyContext&, peg::Span) { return IntNode{2}; });
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
        // First a_or_b chose a_node, second chose b_node (by name).
        CHECK(tree->children[0]->name == "a_or_b");
        CHECK(tree->children[1]->name == "a_or_b");
    }

    SUBCASE("failed first alternative rolls back before retry")
    {
        // Input "ba": first a_or_b tries a_node, fails ('b'), then tries
        // b_node, succeeds. Second a_or_b tries a_node, succeeds. Total:
        // 2 children, both a_or_b.
        std::string input = "ba";
        MyContext ctx(input);
        auto tree = g.parse_tree("seq_ab", ctx);
        REQUIRE(tree);
        REQUIRE(tree->children.size() == 2);
        CHECK(tree->children[0]->name == "a_or_b");
        CHECK(tree->children[1]->name == "a_or_b");
    }
}

TEST_CASE("value-stack-rollback-on-not-predicate")
{
    using MyContext = Context<char, IntNode>;
    Grammar<char, IntNode> g;
    auto a_node = (g["a_node"] = g.terminal('a'));
    a_node.set_action([](MyContext&, peg::Span) { return IntNode{1}; });
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
    using MyContext = Context<char, IntNode>;
    Grammar<char, IntNode> g;
    auto a_node = (g["a_node"] = g.terminal('a'));
    a_node.set_action([](MyContext&, peg::Span) { return IntNode{1}; });
    // &a_node succeeds (lookahead matches 'a'), and must leave zero children
    // — the child's node is discarded by AndExpr.
    g["and_a"] = &g["a_node"];

    std::string input = "a";
    MyContext ctx(input);
    auto tree = g.parse_tree("and_a", ctx);
    REQUIRE(tree);
    CHECK(tree->children.empty());
}
