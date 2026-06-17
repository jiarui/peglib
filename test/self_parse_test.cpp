// ---------------------------------------------------------------------------
// Self-parse test — W2.5 of Phase 2 textual grammar format.
//
// Verifies that the C++ reference meta-grammar can parse the canonical
// textual PEG spec (meta/peg.peg). This is the foundation for the
// bootstrap: once GrammarCompiler (W3) can turn the AST into a working
// Grammar, we'll verify the bootstrapped grammar produces the same AST.
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <fstream>
#include <sstream>
#include <string>

using namespace peg;

namespace
{
// Load meta/peg.peg from the project root.
std::string load_peg_spec()
{
    auto path = std::string{PEGLIB_PROJECT_ROOT} + "/meta/peg.peg";
    std::ifstream f{path};
    REQUIRE(f.is_open());
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

// ---------------------------------------------------------------------------
// The C++ meta-grammar can parse peg.peg without error.
// ---------------------------------------------------------------------------
TEST_CASE("[self-parse] cpp-meta-grammar-parses-peg-spec")
{
    auto spec = load_peg_spec();
    auto& mg = meta_grammar();
    PegParseCtx ctx{spec};
    auto tree = mg.parse_tree("Grammar", ctx);

    CHECK(tree != nullptr);
}

// ---------------------------------------------------------------------------
// The parsed AST contains the expected rules.
// ---------------------------------------------------------------------------
TEST_CASE("[self-parse] parsed-ast-has-expected-rules")
{
    auto spec = load_peg_spec();
    auto& mg = meta_grammar();
    PegParseCtx ctx{spec};
    auto tree = mg.parse_tree("Grammar", ctx);
    REQUIRE(tree != nullptr);

    // Collect all Definition names from the tree.
    std::vector<std::string> names;
    std::function<void(const PegParseCtx::ParseTreeNodePtr&)> collect =
        [&](const PegParseCtx::ParseTreeNodePtr& node) {
            if (!node)
                return;
            if (node->name == "Definition" && node->value) {
                names.push_back(node->value->text);
                return;
            }
            for (auto& c : node->children)
                collect(c);
        };
    collect(tree);

    // peg.peg defines at least these rules.
    CHECK(names.size() >= 15);
    bool has_grammar = false, has_definition = false, has_expression = false;
    bool has_literal = false, has_class = false, has_spacing = false;
    for (const auto& n : names) {
        if (n == "Grammar")
            has_grammar = true;
        if (n == "Definition")
            has_definition = true;
        if (n == "Expression")
            has_expression = true;
        if (n == "Literal")
            has_literal = true;
        if (n == "Class")
            has_class = true;
        if (n == "Spacing")
            has_spacing = true;
    }
    CHECK(has_grammar);
    CHECK(has_definition);
    CHECK(has_expression);
    CHECK(has_literal);
    CHECK(has_class);
    CHECK(has_spacing);
}

// ---------------------------------------------------------------------------
// The Grammar rule's body is a Sequence with Definition+ — verify the
// top-level structure matches what peg.peg describes.
// ---------------------------------------------------------------------------
TEST_CASE("[self-parse] grammar-body-structure")
{
    auto spec = load_peg_spec();
    auto& mg = meta_grammar();
    PegParseCtx ctx{spec};
    auto tree = mg.parse_tree("Grammar", ctx);
    REQUIRE(tree != nullptr);

    // Find the "Grammar" Definition's AST value and check its body.
    PegAstNodePtr grammar_def;
    std::function<void(const PegParseCtx::ParseTreeNodePtr&)> find_grammar =
        [&](const PegParseCtx::ParseTreeNodePtr& node) {
            if (!node)
                return;
            if (node->name == "Definition" && node->value && node->value->text == "Grammar") {
                grammar_def = node->value;
                return;
            }
            for (auto& c : node->children)
                find_grammar(c);
        };
    find_grammar(tree);
    REQUIRE(grammar_def);
    REQUIRE(grammar_def->children.size() >= 1);

    // Grammar body: Sequence(Spacing, Plus(Definition), EndOfFile)
    auto body = grammar_def->children[0];
    CHECK(body->kind == NodeKind::Sequence);
    REQUIRE(body->children.size() == 3);
    CHECK(body->children[0]->kind == NodeKind::RuleRef);
    CHECK(body->children[0]->text == "Spacing");
    CHECK(body->children[1]->kind == NodeKind::Plus);
    CHECK(body->children[2]->kind == NodeKind::RuleRef);
    CHECK(body->children[2]->text == "EndOfFile");
}
