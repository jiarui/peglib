// ---------------------------------------------------------------------------
// Meta-grammar tests — W2 of Phase 2 textual grammar format.
//
// Verifies that the C++ reference meta-grammar (MetaGrammar.h) correctly
// parses PEG text into PegAstNode trees via the post-parse action model.
// The shape of these trees is what GrammarCompiler (W3) will walk to build
// a runtime Grammar.
//
// Post-parse model: the Grammar rule's ParseTreeNode has a child for each
// Definition. Each Definition child's ->value is the PegAstNode for that
// rule. The tests extract nodes from the tree rather than from a value stack.
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <functional>
#include <string>

using namespace peg;

namespace
{
// Parse PEG text and return the root tree node.
PegParseCtx::ParseTreeNodePtr parse_peg_tree(std::string_view text)
{
    auto& mg = meta_grammar();
    std::string input{text};
    PegParseCtx ctx{input};
    auto tree = mg.parse_tree("Grammar", ctx);
    REQUIRE(tree != nullptr);
    return tree;
}

// Get the n-th Definition's AST value from the Grammar tree.
// Definitions are nested inside OneOrMoreExpr's anonymous node.
// Get the n-th Definition's AST value from the Grammar tree.
// Definitions are nested inside combinator wrapper nodes.
PegAstNodePtr get_def(const PegParseCtx::ParseTreeNodePtr& tree, std::size_t idx)
{
    std::size_t count = 0;
    std::function<PegAstNodePtr(const PegParseCtx::ParseTreeNodePtr&)> extract =
        [&](const PegParseCtx::ParseTreeNodePtr& node) -> PegAstNodePtr {
        if (!node)
            return nullptr;
        if (node->name == "Definition" && node->value) {
            if (count == idx)
                return node->value;
            count++;
            return nullptr;
        }
        for (auto& child : node->children) {
            if (auto r = extract(child))
                return r;
        }
        return nullptr;
    };
    return extract(tree);
}

// Convenience: parse and get the first (or only) definition's body.
PegAstNodePtr parse_peg_body(std::string_view text)
{
    auto tree = parse_peg_tree(text);
    auto def = get_def(tree, 0);
    REQUIRE(def);
    REQUIRE(def->children.size() >= 1);
    return def->children[0];
}

// Convenience: parse and get the first definition node.
PegAstNodePtr parse_peg_def(std::string_view text)
{
    auto tree = parse_peg_tree(text);
    return get_def(tree, 0);
}
} // namespace

// ---------------------------------------------------------------------------
// Single bare definition: A <- 'a'
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] single-literal-definition")
{
    auto def = parse_peg_def("A <- 'a'");
    REQUIRE(def);
    CHECK(def->kind == NodeKind::Definition);
    CHECK(def->text == "A");
    REQUIRE(def->children.size() == 1);
    auto body = def->children[0];
    CHECK(body->kind == NodeKind::Literal);
    CHECK(body->text == "a");
}

// ---------------------------------------------------------------------------
// Definition with rule reference and sequence: A <- B 'x'
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] sequence-with-ruleref")
{
    auto body = parse_peg_body("A <- B 'x'");
    REQUIRE(body);
    CHECK(body->kind == NodeKind::Sequence);
    REQUIRE(body->children.size() == 2);
    CHECK(body->children[0]->kind == NodeKind::RuleRef);
    CHECK(body->children[0]->text == "B");
    CHECK(body->children[1]->kind == NodeKind::Literal);
    CHECK(body->children[1]->text == "x");
}

// ---------------------------------------------------------------------------
// Choice: A <- 'a' / 'b' / 'c'
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] choice-of-literals")
{
    auto body = parse_peg_body("A <- 'a' / 'b' / 'c'");
    REQUIRE(body);
    CHECK(body->kind == NodeKind::Choice);
    REQUIRE(body->children.size() == 3);
    CHECK(body->children[0]->text == "a");
    CHECK(body->children[1]->text == "b");
    CHECK(body->children[2]->text == "c");
}

// ---------------------------------------------------------------------------
// Postfix operators: * + ?
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] postfix-operators")
{
    SUBCASE("star")
    {
        auto body = parse_peg_body("A <- 'a'*");
        CHECK(body->kind == NodeKind::Star);
        REQUIRE(body->children.size() == 1);
        CHECK(body->children[0]->text == "a");
    }
    SUBCASE("plus")
    {
        auto body = parse_peg_body("A <- 'a'+");
        CHECK(body->kind == NodeKind::Plus);
        REQUIRE(body->children.size() == 1);
        CHECK(body->children[0]->text == "a");
    }
    SUBCASE("optional")
    {
        auto body = parse_peg_body("A <- 'a'?");
        CHECK(body->kind == NodeKind::Optional);
        REQUIRE(body->children.size() == 1);
        CHECK(body->children[0]->text == "a");
    }
}

// ---------------------------------------------------------------------------
// Prefix operators: & !
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] prefix-operators")
{
    SUBCASE("and-predicate")
    {
        auto body = parse_peg_body("A <- &'a'");
        CHECK(body->kind == NodeKind::AndPred);
        REQUIRE(body->children.size() == 1);
        CHECK(body->children[0]->text == "a");
    }
    SUBCASE("not-predicate")
    {
        auto body = parse_peg_body("A <- !'a'");
        CHECK(body->kind == NodeKind::NotPred);
        REQUIRE(body->children.size() == 1);
        CHECK(body->children[0]->text == "a");
    }
}

// ---------------------------------------------------------------------------
// Dot and character class
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] dot-and-class")
{
    SUBCASE("dot")
    {
        auto body = parse_peg_body("A <- .");
        CHECK(body->kind == NodeKind::Dot);
    }
    SUBCASE("class")
    {
        auto body = parse_peg_body("A <- [a-z]");
        CHECK(body->kind == NodeKind::CharClass);
        CHECK(body->text == "[a-z]");
    }
    SUBCASE("negated-class")
    {
        auto body = parse_peg_body("A <- [^0-9]");
        CHECK(body->kind == NodeKind::CharClass);
        CHECK(body->text == "[^0-9]");
    }
}

// ---------------------------------------------------------------------------
// Grouping: A <- ('a' 'b') 'c'
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] grouping")
{
    auto body = parse_peg_body("A <- ('a' 'b') 'c'");
    REQUIRE(body);
    CHECK(body->kind == NodeKind::Sequence);
    REQUIRE(body->children.size() == 2);
    // Child 0: the group's inner sequence (transparently passed through)
    CHECK(body->children[0]->kind == NodeKind::Sequence);
    REQUIRE(body->children[0]->children.size() == 2);
    CHECK(body->children[1]->kind == NodeKind::Literal);
    CHECK(body->children[1]->text == "c");
}

// ---------------------------------------------------------------------------
// Operators inside groups must not leak to the outer level.
// Regression test for has_named searching too broadly.
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] operators-inside-groups-do-not-leak")
{
    SUBCASE("inner-question-outer-star")
    {
        // ('a'?)*  →  Star(Literal("a")), NOT Optional
        auto body = parse_peg_body("A <- ('a'?)*");
        REQUIRE(body);
        CHECK(body->kind == NodeKind::Star);
        REQUIRE(body->children.size() == 1);
        // Inner: Optional(Literal("a"))
        CHECK(body->children[0]->kind == NodeKind::Optional);
    }
    SUBCASE("inner-star-outer-plus")
    {
        // ('a'*)+  →  Plus(Literal("a")), NOT Star
        auto body = parse_peg_body("A <- ('a'*)+");
        REQUIRE(body);
        CHECK(body->kind == NodeKind::Plus);
        REQUIRE(body->children.size() == 1);
        CHECK(body->children[0]->kind == NodeKind::Star);
    }
    SUBCASE("inner-not-outer-and")
    {
        // !('a' &b)  →  NotPred(...), NOT AndPred
        auto body = parse_peg_body("A <- !('a' &b)");
        REQUIRE(body);
        CHECK(body->kind == NodeKind::NotPred);
        REQUIRE(body->children.size() == 1);
    }
    SUBCASE("group-with-star-no-outer-op")
    {
        // ('a'*)  →  Star(Literal("a")), not double-wrapped
        auto body = parse_peg_body("A <- ('a'*)");
        REQUIRE(body);
        CHECK(body->kind == NodeKind::Star);
        REQUIRE(body->children.size() == 1);
        CHECK(body->children[0]->kind == NodeKind::Literal);
    }
}

// ---------------------------------------------------------------------------
// Escape sequences inside literals
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] literal-escapes")
{
    SUBCASE("newline")
    {
        auto body = parse_peg_body("A <- '\\n'");
        CHECK(body->kind == NodeKind::Literal);
        CHECK(body->text == std::string("\n", 1));
    }
    SUBCASE("escaped-quote")
    {
        auto body = parse_peg_body("A <- '\\''");
        CHECK(body->kind == NodeKind::Literal);
        CHECK(body->text == std::string("'", 1));
    }
    SUBCASE("double-quoted-with-escape")
    {
        auto body = parse_peg_body("A <- \"a\\tb\"");
        CHECK(body->kind == NodeKind::Literal);
        CHECK(body->text == "a\tb");
    }
}

// ---------------------------------------------------------------------------
// Comments and whitespace
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] comments-and-whitespace")
{
    auto def = parse_peg_def("# leading comment\n  A <- 'a'  # trailing\n");
    REQUIRE(def);
    CHECK(def->kind == NodeKind::Definition);
    CHECK(def->text == "A");
    REQUIRE(def->children.size() == 1);
    CHECK(def->children[0]->kind == NodeKind::Literal);
    CHECK(def->children[0]->text == "a");
}

// ---------------------------------------------------------------------------
// Multiple definitions
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] multiple-definitions")
{
    auto tree = parse_peg_tree("A <- 'a'\nB <- A 'b'\nC <- A / B");
    auto a = get_def(tree, 0);
    auto b = get_def(tree, 1);
    auto c = get_def(tree, 2);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(c);
    CHECK(a->text == "A");
    CHECK(b->text == "B");
    CHECK(c->text == "C");
    CHECK(b->children[0]->kind == NodeKind::Sequence);
    CHECK(c->children[0]->kind == NodeKind::Choice);
}

// ---------------------------------------------------------------------------
// Recursive arithmetic grammar
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] recursive-arithmetic")
{
    std::string input = R"(
        Expr   <- Term ('+' Term)*
        Term   <- Factor ('*' Factor)*
        Factor <- [0-9]+ / '(' Expr ')'
    )";
    auto tree = parse_peg_tree(input);
    auto expr = get_def(tree, 0);
    auto term = get_def(tree, 1);
    auto factor = get_def(tree, 2);
    REQUIRE(expr);
    REQUIRE(term);
    REQUIRE(factor);
    CHECK(expr->text == "Expr");
    CHECK(term->text == "Term");
    CHECK(factor->text == "Factor");
    // Expr body: Sequence(Term, Star(Sequence(Literal('+'), Term)))
    REQUIRE(expr->children.size() == 1);
    CHECK(expr->children[0]->kind == NodeKind::Sequence);
    REQUIRE(expr->children[0]->children.size() == 2);
    CHECK(expr->children[0]->children[0]->kind == NodeKind::RuleRef);
    CHECK(expr->children[0]->children[0]->text == "Term");
    CHECK(expr->children[0]->children[1]->kind == NodeKind::Star);
}

// ---------------------------------------------------------------------------
// Error cases: malformed input must fail to parse
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] rejects-malformed-input")
{
    auto& mg = meta_grammar();
    SUBCASE("missing-arrow")
    {
        std::string input = "A 'a'";
        PegParseCtx ctx{input};
        CHECK_FALSE(mg.parse(ctx));
    }
    SUBCASE("unclosed-literal")
    {
        std::string input = "A <- 'a";
        PegParseCtx ctx{input};
        CHECK_FALSE(mg.parse(ctx));
    }
    SUBCASE("unclosed-group")
    {
        std::string input = "A <- ('a'";
        PegParseCtx ctx{input};
        CHECK_FALSE(mg.parse(ctx));
    }
    SUBCASE("empty-input")
    {
        std::string input = "";
        PegParseCtx ctx{input};
        CHECK_FALSE(mg.parse(ctx));
    }
}
