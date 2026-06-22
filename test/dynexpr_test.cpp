// ---------------------------------------------------------------------------
// DynExpr smoke tests.
//
// Verifies that the type-erased dynamic combinators can build a working
// expression tree at runtime (without the compile-time DSL) and that they
// honour cut escalation and zero-width predicate semantics.
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <memory>
#include <string>

using namespace peg;
using namespace peg::parsers;

namespace
{
// Shortcut: build a DynExpr from a single interface pointer.
template<typename Ctx>
DynExpr<Ctx> dyn(std::shared_ptr<ParsingExprInterface<Ctx>> p)
{
    return DynExpr<Ctx>{std::move(p)};
}
} // namespace

// ---------------------------------------------------------------------------
// DynSequenceExpr matches all children in order.
// ---------------------------------------------------------------------------
TEST_CASE("[dynexpr] sequence-success-and-failure")
{
    using Ctx = Context<char>;
    std::vector<std::shared_ptr<ParsingExprInterface<Ctx>>> children;
    children.push_back(std::make_shared<TerminalExpr<Ctx, char>>('a'));
    children.push_back(std::make_shared<TerminalExpr<Ctx, char>>('b'));
    children.push_back(std::make_shared<TerminalExpr<Ctx, char>>('c'));

    DynSequenceExpr<Ctx> seq{std::move(children)};

    SUBCASE("matches exact input")
    {
        std::string input = "abc";
        Ctx ctx(input);
        CHECK(seq.parse(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("fails on prefix match")
    {
        std::string input = "abx";
        Ctx ctx(input);
        CHECK_FALSE(seq.parse(ctx));
        // On failure, position is restored to the start.
        CHECK(ctx.mark() == 0);
    }
    SUBCASE("fails on short input")
    {
        std::string input = "ab";
        Ctx ctx(input);
        CHECK_FALSE(seq.parse(ctx));
    }
}

// ---------------------------------------------------------------------------
// DynAlternationExpr tries each alternative.
// ---------------------------------------------------------------------------
TEST_CASE("[dynexpr] alternation-first-success-wins")
{
    using Ctx = Context<char>;
    std::vector<std::shared_ptr<ParsingExprInterface<Ctx>>> children;
    children.push_back(std::make_shared<TerminalExpr<Ctx, char>>('a'));
    children.push_back(std::make_shared<TerminalExpr<Ctx, char>>('b'));

    DynAlternationExpr<Ctx> alt{std::move(children)};

    SUBCASE("first alternative")
    {
        std::string input = "a";
        Ctx ctx(input);
        CHECK(alt.parse(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("second alternative")
    {
        std::string input = "b";
        Ctx ctx(input);
        CHECK(alt.parse(ctx));
        CHECK(ctx.ended());
    }
    SUBCASE("neither matches")
    {
        std::string input = "c";
        Ctx ctx(input);
        CHECK_FALSE(alt.parse(ctx));
    }
}

// ---------------------------------------------------------------------------
// DynRepeatExpr covers star/plus/optional.
// ---------------------------------------------------------------------------
TEST_CASE("[dynexpr] repeat-zero-or-more")
{
    using Ctx = Context<char>;
    auto child = std::make_shared<TerminalExpr<Ctx, char>>('a');
    // * : min=0, max=-1
    DynRepeatExpr<Ctx> star{child, 0, -1};

    SUBCASE("zero matches")
    {
        std::string input = "";
        Ctx ctx(input);
        CHECK(star.parse(ctx));
    }
    SUBCASE("many matches")
    {
        std::string input = "aaaa";
        Ctx ctx(input);
        CHECK(star.parse(ctx));
        CHECK(ctx.ended());
    }
}

TEST_CASE("[dynexpr] repeat-one-or-more")
{
    using Ctx = Context<char>;
    auto child = std::make_shared<TerminalExpr<Ctx, char>>('a');
    // + : min=1, max=-1
    DynRepeatExpr<Ctx> plus{child, 1, -1};

    SUBCASE("rejects empty input")
    {
        std::string input = "";
        Ctx ctx(input);
        CHECK_FALSE(plus.parse(ctx));
    }
    SUBCASE("accepts one")
    {
        std::string input = "a";
        Ctx ctx(input);
        CHECK(plus.parse(ctx));
    }
}

TEST_CASE("[dynexpr] repeat-optional")
{
    using Ctx = Context<char>;
    auto child = std::make_shared<TerminalExpr<Ctx, char>>('a');
    // ? : min=0, max=1
    DynRepeatExpr<Ctx> opt{child, 0, 1};

    SUBCASE("absent is fine")
    {
        std::string input = "b";
        Ctx ctx(input);
        CHECK(opt.parse(ctx));
        CHECK_FALSE(ctx.ended());
    }
    SUBCASE("present consumes once")
    {
        std::string input = "aa";
        Ctx ctx(input);
        CHECK(opt.parse(ctx));
        CHECK_FALSE(ctx.ended()); // second 'a' unconsumed
    }
}

// ---------------------------------------------------------------------------
// DynAndExpr / DynNotExpr predicates consume nothing.
// ---------------------------------------------------------------------------
TEST_CASE("[dynexpr] and-predicate-zero-width")
{
    using Ctx = Context<char>;
    auto child = std::make_shared<TerminalExpr<Ctx, char>>('a');
    DynAndExpr<Ctx> a{child};

    std::string input = "a";
    Ctx ctx(input);
    CHECK(a.parse(ctx));
    CHECK_FALSE(ctx.ended()); // position unchanged
}

TEST_CASE("[dynexpr] not-predicate-zero-width")
{
    using Ctx = Context<char>;
    auto child = std::make_shared<TerminalExpr<Ctx, char>>('a');
    DynNotExpr<Ctx> n{child};

    SUBCASE("succeeds when child fails")
    {
        std::string input = "b";
        Ctx ctx(input);
        CHECK(n.parse(ctx));
        CHECK_FALSE(ctx.ended());
    }
    SUBCASE("fails when child would succeed")
    {
        std::string input = "a";
        Ctx ctx(input);
        CHECK_FALSE(n.parse(ctx));
    }
}

// ---------------------------------------------------------------------------
// DynExpr handle participates in the operator DSL.
// ---------------------------------------------------------------------------
TEST_CASE("[dynexpr] handle-interop-with-dsl")
{
    using Ctx = Context<char>;
    // A DynExpr wrapping a terminal can be sequenced with a static terminal
    // via the public operator>>. This is what GrammarCompiler will do.
    auto term_a = std::make_shared<TerminalExpr<Ctx, char>>('a');
    DynExpr<Ctx> dyn_a{term_a};

    // (dyn_a >> 'b') should parse "ab".
    auto expr = dyn_a >> terminal('b');

    std::string input = "ab";
    Ctx ctx(input);
    CHECK(expr.parse(ctx));
    CHECK(ctx.ended());
}

// ---------------------------------------------------------------------------
// PegAst node factory smoke test.
// ---------------------------------------------------------------------------
TEST_CASE("[pegast] node-factory-and-fields")
{
    auto leaf = PegAstNode::make(NodeKind::Literal, "abc");
    CHECK(leaf->kind == NodeKind::Literal);
    CHECK(leaf->text == "abc");
    CHECK(leaf->children.empty());

    auto parent = PegAstNode::make(NodeKind::Sequence);
    parent->children.push_back(PegAstNode::make(NodeKind::RuleRef, "x"));
    parent->children.push_back(leaf);
    CHECK(parent->children.size() == 2);
    CHECK(parent->children[0]->kind == NodeKind::RuleRef);
    CHECK(parent->children[1]->text == "abc");
}
