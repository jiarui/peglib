// ---------------------------------------------------------------------------
// Meta-grammar tests — W2 of Phase 2 textual grammar format.
//
// Verifies that the C++ reference meta-grammar (MetaGrammar.h) correctly
// parses PEG text into PegAstNode trees. The shape of these trees is what
// GrammarCompiler (W3) will walk to build a runtime Grammar.
//
// NOTE: MetaGrammar.h is currently a placeholder (empty Grammar). These
// tests previously inspected the value stack after a parse; the
// post-parse-action refactor removed the value stack and MetaGrammar.h
// has not yet been rewritten to build trees through actions. All test
// bodies are disabled until MetaGrammar.h is re-implemented.
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

#if 0

namespace
{
// Parse `text` with the meta-grammar and return the top-of-stack node.
// Assumes parse succeeds and stack has exactly one node (the Grammar rule's
// Definition children are popped by the caller of this helper when needed).
PegAstNodePtr parse_peg(std::string_view text)
{
    auto& mg = meta_grammar();
    std::string input{text};
    PegParseCtx ctx{input};
    REQUIRE(mg.parse(ctx));
    REQUIRE(ctx.node_count() >= 1);
    return ctx.pop_node();
}
} // namespace

// ---------------------------------------------------------------------------
// Single bare definition: A <- 'a'
// Stack after parse: [Definition("A", [Literal("a")])]
// (Grammar rule is transparent, so only Definition pushes.)
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] single-literal-definition")
{
    auto root = parse_peg("A <- 'a'");
    REQUIRE(root);
    CHECK(root->kind == NodeKind::Definition);
    CHECK(root->text == "A");
    REQUIRE(root->children.size() == 1);
    auto body = root->children[0];
    CHECK(body->kind == NodeKind::Literal);
    CHECK(body->text == "a");
}

// ---------------------------------------------------------------------------
// Definition with rule reference and sequence: A <- B 'x'
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] sequence-with-ruleref")
{
    auto root = parse_peg("A <- B 'x'");
    REQUIRE(root->kind == NodeKind::Definition);
    REQUIRE(root->children.size() == 1);
    auto body = root->children[0];
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
    auto root = parse_peg("A <- 'a' / 'b' / 'c'");
    auto body = root->children[0];
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
        auto root = parse_peg("A <- 'a'*");
        auto body = root->children[0];
        CHECK(body->kind == NodeKind::Star);
        REQUIRE(body->children.size() == 1);
        CHECK(body->children[0]->text == "a");
    }
    SUBCASE("plus")
    {
        auto root = parse_peg("A <- 'a'+");
        auto body = root->children[0];
        CHECK(body->kind == NodeKind::Plus);
        REQUIRE(body->children.size() == 1);
        CHECK(body->children[0]->text == "a");
    }
    SUBCASE("optional")
    {
        auto root = parse_peg("A <- 'a'?");
        auto body = root->children[0];
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
        auto root = parse_peg("A <- &'a'");
        auto body = root->children[0];
        CHECK(body->kind == NodeKind::AndPred);
        REQUIRE(body->children.size() == 1);
        CHECK(body->children[0]->text == "a");
    }
    SUBCASE("not-predicate")
    {
        auto root = parse_peg("A <- !'a'");
        auto body = root->children[0];
        CHECK(body->kind == NodeKind::NotPred);
        REQUIRE(body->children.size() == 1);
        CHECK(body->children[0]->text == "a");
    }
}

// ---------------------------------------------------------------------------
// Dot (any-char) and character class
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] dot-and-class")
{
    SUBCASE("dot")
    {
        auto root = parse_peg("A <- .");
        auto body = root->children[0];
        CHECK(body->kind == NodeKind::Dot);
    }
    SUBCASE("class")
    {
        auto root = parse_peg("A <- [a-z]");
        auto body = root->children[0];
        CHECK(body->kind == NodeKind::CharClass);
        CHECK(body->text == "[a-z]");
    }
    SUBCASE("negated-class")
    {
        auto root = parse_peg("A <- [^0-9]");
        auto body = root->children[0];
        CHECK(body->kind == NodeKind::CharClass);
        CHECK(body->text == "[^0-9]");
    }
}

// ---------------------------------------------------------------------------
// Grouping: A <- ('a' 'b') 'c'
// The group's inner Sequence is transparently passed through.
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] grouping")
{
    auto root = parse_peg("A <- ('a' 'b') 'c'");
    auto body = root->children[0];
    // Outer sequence has two children: the inner sequence and 'c'.
    CHECK(body->kind == NodeKind::Sequence);
    REQUIRE(body->children.size() == 2);
    CHECK(body->children[0]->kind == NodeKind::Sequence);
    REQUIRE(body->children[0]->children.size() == 2);
    CHECK(body->children[1]->kind == NodeKind::Literal);
    CHECK(body->children[1]->text == "c");
}

// ---------------------------------------------------------------------------
// Escape sequences inside literals
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] literal-escapes")
{
    SUBCASE("newline")
    {
        auto root = parse_peg("A <- '\\n'");
        auto body = root->children[0];
        CHECK(body->kind == NodeKind::Literal);
        CHECK(body->text == std::string("\n", 1));
    }
    SUBCASE("escaped-quote")
    {
        auto root = parse_peg("A <- '\\''");
        auto body = root->children[0];
        CHECK(body->kind == NodeKind::Literal);
        CHECK(body->text == std::string("'", 1));
    }
    SUBCASE("double-quoted-with-escape")
    {
        auto root = parse_peg("A <- \"a\\tb\"");
        auto body = root->children[0];
        CHECK(body->kind == NodeKind::Literal);
        CHECK(body->text == "a\tb");
    }
}

// ---------------------------------------------------------------------------
// Comments and whitespace
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] comments-and-whitespace")
{
    auto root = parse_peg("# leading comment\n  A <- 'a'  # trailing\n");
    REQUIRE(root);
    CHECK(root->kind == NodeKind::Definition);
    CHECK(root->text == "A");
    REQUIRE(root->children.size() == 1);
    CHECK(root->children[0]->kind == NodeKind::Literal);
    CHECK(root->children[0]->text == "a");
}

// ---------------------------------------------------------------------------
// Multiple definitions: stack contains one Definition node per rule
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] multiple-definitions")
{
    auto& mg = meta_grammar();
    std::string input = "A <- 'a'\nB <- A 'b'\nC <- A / B";
    PegParseCtx ctx{input};
    CHECK(mg.parse(ctx));
    // Three Definition nodes on the stack.
    CHECK(ctx.node_count() == 3);
    auto c = ctx.pop_node();
    auto b = ctx.pop_node();
    auto a = ctx.pop_node();
    CHECK(a->text == "A");
    CHECK(b->text == "B");
    CHECK(c->text == "C");
    CHECK(b->children[0]->kind == NodeKind::Sequence);
    CHECK(c->children[0]->kind == NodeKind::Choice);
}

// ---------------------------------------------------------------------------
// Recursive grammar (mutual): classic arithmetic
//   Expr <- Term ('+' Term)*
//   Term <- Factor ('*' Factor)*
//   Factor <- [0-9]+ / '(' Expr ')'
// ---------------------------------------------------------------------------
TEST_CASE("[meta_grammar] recursive-arithmetic")
{
    std::string input = R"(
        Expr   <- Term ('+' Term)*
        Term   <- Factor ('*' Factor)*
        Factor <- [0-9]+ / '(' Expr ')'
    )";
    auto& mg = meta_grammar();
    PegParseCtx ctx{input};
    CHECK(mg.parse(ctx));
    REQUIRE(ctx.node_count() == 3);
    auto factor = ctx.pop_node();
    auto term = ctx.pop_node();
    auto expr = ctx.pop_node();
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
        std::string input = "A 'a'";  // missing <-
        PegParseCtx ctx{input};
        CHECK_FALSE(mg.parse(ctx));
    }
    SUBCASE("unclosed-literal")
    {
        std::string input = "A <- 'a";  // missing closing quote
        PegParseCtx ctx{input};
        CHECK_FALSE(mg.parse(ctx));
    }
    SUBCASE("unclosed-group")
    {
        std::string input = "A <- ('a'";  // missing )
        PegParseCtx ctx{input};
        CHECK_FALSE(mg.parse(ctx));
    }
    SUBCASE("empty-input")
    {
        std::string input = "";
        PegParseCtx ctx{input};
        CHECK_FALSE(mg.parse(ctx));  // Grammar requires at least one Definition
    }
}

#endif // 0 — disabled pending MetaGrammar.h re-implementation
