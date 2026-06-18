#pragma once
#include "peglib/Context.h"

#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace peg
{

// ---------------------------------------------------------------------------
// PegAst: AST produced by the meta-grammar when parsing PEG text.
//
// This is an intermediate representation between textual PEG input and the
// runtime Rule tree. The meta-grammar's semantic actions build these nodes
// and push them onto the Context value stack; the GrammarCompiler then
// walks the tree to construct Grammar<> rules.
//
// Shape: a simple discriminated tree. Every node has a kind, an optional
// text payload (literal value / char-class source / identifier name), and
// a list of children. We deliberately avoid std::variant here so that the
// meta-grammar's reduce actions stay uniform regardless of kind.
// ---------------------------------------------------------------------------

enum class NodeKind
{
    // Structural
    Definition, // name <- expr;    children[0] = body expression
    Sequence,   // e1 e2 ... en
    Choice,     // e1 / e2 / ... / en
    Optional,   // e?
    Star,       // e*
    Plus,       // e+
    AndPred,    // &e
    NotPred,    // !e
    Group,      // (e)  — child carried transparently

    // Leaves
    Literal,   // 'abc' / "abc";   text = the decoded content
    CharClass, // [a-z^0-9_];      text = the raw inside-bracket source
    Dot,       // .
    RuleRef,   // name;            text = referenced rule name

    // Internal sentinel used by the meta-grammar's reduce actions to
    // delimit a variable-length run of values on the value stack. Never
    // appears in a finished AST tree returned to user code.
    Marker,
};

struct PegAstNode
{
    NodeKind kind;
    std::string text;
    std::vector<std::shared_ptr<PegAstNode>> children;

    // Source span (byte offsets into the original PEG text). Filled by the
    // meta-grammar's actions from the matching ParseTreeNode's offsets, so
    // that GrammarCompiler diagnostics can point at where an AST node
    // originated — e.g. "undefined rule 'foo'" can name the offset of the
    // offending identifier rather than a hardcoded 0. Defaults to 0 for
    // nodes built without offset context.
    std::size_t start_offset = 0;
    std::size_t end_offset = 0;

    PegAstNode() = default;
    PegAstNode(NodeKind k, std::string t = {}) : kind{k}, text{std::move(t)} {}

    // Convenience factory. Usage:
    //   auto n = PegAstNode::make(NodeKind::Literal, "abc");
    static std::shared_ptr<PegAstNode> make(NodeKind k, std::string t = {})
    {
        return std::make_shared<PegAstNode>(k, std::move(t));
    }

    // Factory that also records the source span.
    static std::shared_ptr<PegAstNode>
    make(NodeKind k, std::string t, std::size_t start, std::size_t end)
    {
        auto n = std::make_shared<PegAstNode>(k, std::move(t));
        n->start_offset = start;
        n->end_offset = end;
        return n;
    }
};

using PegAstNodePtr = std::shared_ptr<PegAstNode>;

// Context type used by the meta-grammar. The value stack carries
// PegAstNodePtr values; std::monostate users are unaffected.
using PegParseCtx = Context<std::span<const char>, PegAstNodePtr>;

} // namespace peg
