#pragma once
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "peglib/Context.h"

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
    Definition,   // name <- expr;    children[0] = body expression
    Sequence,     // e1 e2 ... en
    Choice,       // e1 / e2 / ... / en
    Optional,     // e?
    Star,         // e*
    Plus,         // e+
    AndPred,      // &e
    NotPred,      // !e
    Group,        // (e)  — child carried transparently

    // Leaves
    Literal,      // 'abc' / "abc";   text = the decoded content
    CharClass,    // [a-z^0-9_];      text = the raw inside-bracket source
    Dot,          // .
    RuleRef,      // name;            text = referenced rule name

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

    PegAstNode() = default;
    PegAstNode(NodeKind k, std::string t = {})
        : kind{k}, text{std::move(t)} {}

    // Convenience factory. Usage:
    //   auto n = PegAstNode::make(NodeKind::Literal, "abc");
    static std::shared_ptr<PegAstNode> make(NodeKind k, std::string t = {})
    {
        return std::make_shared<PegAstNode>(k, std::move(t));
    }
};

using PegAstNodePtr = std::shared_ptr<PegAstNode>;

// Context type used by the meta-grammar. The value stack carries
// PegAstNodePtr values; std::monostate users are unaffected.
using PegParseCtx = Context<std::span<const char>, PegAstNodePtr>;

} // namespace peg
