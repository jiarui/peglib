#pragma once
#include "peglib/Grammar.h"
#include "peglib/PegAst.h"
#include "peglib/Rule.h"

#include <array>
#include <cctype>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace peg
{

// ---------------------------------------------------------------------------
// Meta-grammar: a PEG grammar that parses PEG text into PegAstNode trees.
//
// Post-parse action model makes this dramatically simpler than the old
// value-stack version. Each action receives a ParseTreeNodePtr whose
// children are the sub-rule results. No Marker sentinels, no pop/push.
//
// Transparent rules: action returns nullptr → tree set to null by
// NonTerminal::parse → parent's SequenceExpr skips it in children list.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Tree search helpers. Combinators (OptionalExpr, OneOrMoreExpr, etc.)
// create anonymous wrapper nodes, so a named sub-rule's result may be
// nested several levels deep inside the action's ParseTreeNode. These
// helpers let actions find sub-results by rule name without hand-rolled
// recursion in every lambda.
// ---------------------------------------------------------------------------

// Recursively find the first node named `name` that has a value.
inline PegParseCtx::ParseTreeNodePtr find_named(const PegParseCtx::ParseTreeNodePtr& root,
                                                std::string_view name)
{
    if (!root)
        return nullptr;
    if (root->name == name && root->value)
        return root;
    for (auto& c : root->children) {
        if (auto r = find_named(c, name))
            return r;
    }
    return nullptr;
}

// Check whether any node named `name` exists in the subtree (regardless
// of whether it has a value). Used to detect operator tokens like
// STAR, QUESTION, AND, NOT, SLASH.
inline bool has_named(const PegParseCtx::ParseTreeNodePtr& root, std::string_view name)
{
    if (!root)
        return false;
    if (root->name == name)
        return true;
    for (auto& c : root->children) {
        if (has_named(c, name))
            return true;
    }
    return false;
}

// Collect values from all nodes named `name` (pre-order traversal).
// Uses an overload pair to avoid std::function overhead.
inline void collect_named_impl(const PegParseCtx::ParseTreeNodePtr& root,
                               std::string_view name,
                               std::vector<PegAstNodePtr>& out)
{
    if (!root)
        return;
    if (root->name == name && root->value) {
        out.push_back(root->value);
        return; // don't descend into a match's own children
    }
    for (auto& c : root->children)
        collect_named_impl(c, name, out);
}

inline std::vector<PegAstNodePtr> collect_named(const PegParseCtx::ParseTreeNodePtr& root,
                                                std::string_view name)
{
    std::vector<PegAstNodePtr> out;
    collect_named_impl(root, name, out);
    return out;
}

inline const Grammar<PegParseCtx>& meta_grammar()
{
    using Ctx = PegParseCtx;
    using NodePtr = PegAstNodePtr;
    using TreePtr = PegParseCtx::ParseTreeNodePtr;

    using CTerm = TerminalExpr<Ctx, char>;
    using CTermRange = TerminalExpr<Ctx, std::array<char, 2>>;
    using CTermPred = TerminalExpr<Ctx, bool (*)(char)>;
    using CEmpty = EmptyExpr<Ctx>;

    static Grammar<PegParseCtx> g = []() {
        Grammar<PegParseCtx> g;

        // Helper: pass-through action. NonTerminal may reuse the body's tree
        // (when body is a single NonTerminal or AlternationExpr returning a
        // branch). In that case node->value is already set by the body.
        // Otherwise we search the body's children for the first sub-rule
        // that produced a value.
        auto pass_through = [](Ctx&, TreePtr node) -> NodePtr {
            if (node->value)
                return node->value;
            for (auto& child : node->children) {
                if (child && child->value)
                    return child->value;
            }
            return nullptr;
        };

        auto any_char = CTermPred([](char c) { return c != '\0'; });
        auto non_nl = CTermPred([](char c) { return c != '\n' && c != '\0'; });

        // ==================================================================
        // Whitespace and comments (transparent — return nullptr)
        // ==================================================================
        g["Comment"] = CTerm('#') >> *non_nl >> -CTerm('\n');
        g["Comment"].set_action([](Ctx&, TreePtr) -> NodePtr { return nullptr; });

        g["WSChar"] = CTerm(' ') | CTerm('\t') | CTerm('\r') | CTerm('\n');
        g["WSChar"].set_action([](Ctx&, TreePtr) -> NodePtr { return nullptr; });

        g["Spacing"] = *(g["WSChar"] | g["Comment"]);
        g["Spacing"].set_action([](Ctx&, TreePtr) -> NodePtr { return nullptr; });

        g["EndOfFile"] = !any_char;
        g["EndOfFile"].set_action([](Ctx&, TreePtr) -> NodePtr { return nullptr; });

        // ==================================================================
        // Char — transparent (decoded by Literal/Class actions via offsets)
        // ==================================================================
        g["Escape"] = CTerm('\\') >> (CTerm('n') | CTerm('r') | CTerm('t') | CTerm('\'') |
                                      CTerm('"') | CTerm('\\') | CTerm('[') | CTerm(']'));
        g["Escape"].set_action([](Ctx&, TreePtr) -> NodePtr { return nullptr; });

        g["RawChar"] = CTermPred([](char c) { return c != '\\' && c != '\n' && c != '\0'; });
        g["RawChar"].set_action([](Ctx&, TreePtr) -> NodePtr { return nullptr; });

        g["Char"] = g["Escape"] | g["RawChar"];
        g["Char"].set_action([](Ctx&, TreePtr) -> NodePtr { return nullptr; });

        // ==================================================================
        // Identifier
        // ==================================================================
        g["IdentStart"] = CTermRange({'a', 'z'}) | CTermRange({'A', 'Z'}) | CTerm('_');
        g["IdentStart"].set_action([](Ctx&, TreePtr) -> NodePtr { return nullptr; });

        g["IdentCont"] = g["IdentStart"] | CTermRange({'0', '9'});
        g["IdentCont"].set_action([](Ctx&, TreePtr) -> NodePtr { return nullptr; });

        g["IdentifierRaw"] = g["IdentStart"] >> *g["IdentCont"];
        g["IdentifierRaw"].set_action([](Ctx& ctx, TreePtr node) -> NodePtr {
            auto& input = ctx.get_input();
            std::string name(input.begin() + node->start_offset, input.begin() + node->end_offset);
            return PegAstNode::make(NodeKind::RuleRef, std::move(name),
                                    node->start_offset, node->end_offset);
        });

        // Identifier: pass through IdentifierRaw's value. The body is
        // (IdentifierRaw >> Spacing). NonTerminal reuses the body's tree
        // (a SequenceExpr anonymous node). IdentifierRaw's tree is a child.
        g["Identifier"] = g["IdentifierRaw"] >> g["Spacing"];
        g["Identifier"].set_action(pass_through);

        // ==================================================================
        // Literal — single- or double-quoted string.
        // ==================================================================
        auto decode_peg_escapes = [](std::string_view src) {
            std::string out;
            out.reserve(src.size());
            for (std::size_t i = 0; i < src.size(); ++i) {
                if (src[i] == '\\' && i + 1 < src.size()) {
                    switch (src[i + 1]) {
                    case 'n':
                        out += '\n';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    default:
                        out += src[i + 1];
                        break;
                    }
                    ++i;
                } else {
                    out += src[i];
                }
            }
            return out;
        };

        g["SingleQuotedCore"] = CTerm('\'') >> *(!CTerm('\'') >> g["Char"]) >> CTerm('\'');
        g["SingleQuotedCore"].set_action([decode_peg_escapes](Ctx& ctx, TreePtr node) -> NodePtr {
            auto& input = ctx.get_input();
            // node spans the full 'xyz' (with quotes). Body is [1, -1).
            std::size_t s = node->start_offset + 1;
            std::size_t e = node->end_offset - 1;
            std::string_view body(input.data() + s, e - s);
            return PegAstNode::make(NodeKind::Literal, decode_peg_escapes(body),
                                    node->start_offset, node->end_offset);
        });

        g["DoubleQuotedCore"] = CTerm('"') >> *(!CTerm('"') >> g["Char"]) >> CTerm('"');
        g["DoubleQuotedCore"].set_action([decode_peg_escapes](Ctx& ctx, TreePtr node) -> NodePtr {
            auto& input = ctx.get_input();
            std::size_t s = node->start_offset + 1;
            std::size_t e = node->end_offset - 1;
            std::string_view body(input.data() + s, e - s);
            return PegAstNode::make(NodeKind::Literal, decode_peg_escapes(body),
                                    node->start_offset, node->end_offset);
        });

        // LiteralCore: pass through SingleQuotedCore / DoubleQuotedCore value.
        // AlternationExpr returns the successful branch's ParseResult; the
        // branch is a NonTerminal whose value is already set. So node->value
        // already carries the Literal PegAstNode — just forward it.
        g["LiteralCore"] = g["SingleQuotedCore"] | g["DoubleQuotedCore"];
        g["LiteralCore"].set_action(pass_through);

        // Literal: pass through LiteralCore's value.
        g["Literal"] = g["LiteralCore"] >> g["Spacing"];
        g["Literal"].set_action(pass_through);

        // ==================================================================
        // Class — character class with optional negation.
        // Action captures the raw bracketed source for the compiler to decode.
        // ==================================================================
        g["Range"] = g["Char"] >> -(CTerm('-') >> g["Char"]);
        g["Range"].set_action([](Ctx&, TreePtr) -> NodePtr { return nullptr; });

        g["ClassCore"] = CTerm('[') >> -CTerm('^') >> *(!CTerm(']') >> g["Range"]) >> CTerm(']');
        g["ClassCore"].set_action([](Ctx& ctx, TreePtr node) -> NodePtr {
            auto& input = ctx.get_input();
            std::string raw(input.begin() + node->start_offset, input.begin() + node->end_offset);
            return PegAstNode::make(NodeKind::CharClass, std::move(raw),
                                    node->start_offset, node->end_offset);
        });

        // Class: pass through ClassCore's value.
        g["Class"] = g["ClassCore"] >> g["Spacing"];
        g["Class"].set_action(pass_through);

        // ==================================================================
        // Punctuation tokens — match + trailing Spacing, never push.
        // ==================================================================
        auto make_punct = [&](const char* name, auto expr) {
            g[name] = expr;
            g[name].set_action([](Ctx&, TreePtr) -> NodePtr { return nullptr; });
        };

        make_punct("LEFTARROW", CTerm('<') >> CTerm('-') >> g["Spacing"]);
        make_punct("SLASH", CTerm('/') >> g["Spacing"]);
        make_punct("AND", CTerm('&') >> g["Spacing"]);
        make_punct("NOT", CTerm('!') >> g["Spacing"]);
        make_punct("QUESTION", CTerm('?') >> g["Spacing"]);
        make_punct("STAR", CTerm('*') >> g["Spacing"]);
        make_punct("PLUS", CTerm('+') >> g["Spacing"]);
        make_punct("OPEN", CTerm('(') >> g["Spacing"]);
        make_punct("CLOSE", CTerm(')') >> g["Spacing"]);

        g["DOT"] = CTerm('.') >> g["Spacing"];
        g["DOT"].set_action(
            [](Ctx&, TreePtr node) -> NodePtr {
                return PegAstNode::make(NodeKind::Dot, {}, node->start_offset, node->end_offset);
            });

        // ==================================================================
        // Primary
        //   Primary <- Identifier !LEFTARROW
        //             / OPEN Expression CLOSE
        //             / Literal / Class / DOT
        // ==================================================================
        g["Primary"] = (g["Identifier"] >> !g["LEFTARROW"]) |
                       (g["OPEN"] >> g["Expression"] >> g["CLOSE"]) | g["Literal"] | g["Class"] |
                       g["DOT"];

        // Primary: pass through the meaningful child's value.
        g["Primary"].set_action(pass_through);

        // ==================================================================
        // Suffixed — Primary with optional postfix (? * +).
        // ==================================================================
        g["Suffixed"] = g["Primary"] >> -(g["QUESTION"] | g["STAR"] | g["PLUS"]);
        g["Suffixed"].set_action([](Ctx&, TreePtr node) -> NodePtr {
            auto primary = find_named(node, "Primary");
            NodePtr child_val = primary ? primary->value : node->value;
            if (!child_val)
                return nullptr;

            // Postfix operators are siblings of Primary (inside the
            // OptionalExpr wrapper). Search only those siblings — NOT
            // Primary's subtree, so operators inside groups like
            // ('a'?)* don't leak to the outer level.
            NodeKind k = NodeKind::Optional;
            bool has_op = false;
            for (auto& child : node->children) {
                if (!child || child->name == "Primary")
                    continue;
                if (has_named(child, "QUESTION")) {
                    k = NodeKind::Optional;
                    has_op = true;
                    break;
                }
                if (has_named(child, "STAR")) {
                    k = NodeKind::Star;
                    has_op = true;
                    break;
                }
                if (has_named(child, "PLUS")) {
                    k = NodeKind::Plus;
                    has_op = true;
                    break;
                }
            }
            if (!has_op)
                return child_val;

            auto n = PegAstNode::make(k, {}, node->start_offset, node->end_offset);
            n->children.push_back(child_val);
            return n;
        });

        // ==================================================================
        // Prefixed — optional prefix (& !) then Suffixed.
        // ==================================================================
        g["Prefixed"] = -(g["AND"] | g["NOT"]) >> g["Suffixed"];
        g["Prefixed"].set_action([](Ctx&, TreePtr node) -> NodePtr {
            auto suffixed = find_named(node, "Suffixed");
            NodePtr child_val = suffixed ? suffixed->value : node->value;
            if (!child_val)
                return nullptr;

            // Prefix operators are siblings of Suffixed. Don't descend
            // into Suffixed's subtree — operators inside groups must
            // not leak to the outer level.
            NodeKind k = NodeKind::AndPred;
            bool has_op = false;
            for (auto& child : node->children) {
                if (!child || child->name == "Suffixed")
                    continue;
                if (has_named(child, "AND")) {
                    k = NodeKind::AndPred;
                    has_op = true;
                    break;
                }
                if (has_named(child, "NOT")) {
                    k = NodeKind::NotPred;
                    has_op = true;
                    break;
                }
            }
            if (!has_op)
                return child_val;

            auto n = PegAstNode::make(k, {}, node->start_offset, node->end_offset);
            n->children.push_back(child_val);
            return n;
        });

        // ==================================================================
        // Sequence — one or more Prefixed.
        // ==================================================================
        g["Sequence"] = +g["Prefixed"];
        g["Sequence"].set_action([](Ctx&, TreePtr node) -> NodePtr {
            std::vector<NodePtr> items;
            for (auto& child : node->children) {
                if (child && child->value)
                    items.push_back(child->value);
            }
            if (items.empty())
                return nullptr;
            if (items.size() == 1)
                return items[0];
            auto n = PegAstNode::make(NodeKind::Sequence, {}, node->start_offset, node->end_offset);
            n->children = std::move(items);
            return n;
        });

        // ==================================================================
        // Expression — Sequence (SLASH Sequence)*
        // ==================================================================
        g["Expression"] = g["Sequence"] >> *(g["SLASH"] >> g["Sequence"]);
        g["Expression"].set_action([](Ctx&, TreePtr node) -> NodePtr {
            auto alts = collect_named(node, "Sequence");
            if (alts.empty())
                return nullptr;
            if (alts.size() == 1)
                return alts[0];
            auto n = PegAstNode::make(NodeKind::Choice, {}, node->start_offset, node->end_offset);
            n->children = std::move(alts);
            return n;
        });

        // ==================================================================
        // Definition — Identifier LEFTARROW Expression
        //
        // All three children are present in node->children; find_named
        // locates Identifier and Expression by rule name.
        // ==================================================================
        g["Definition"] = g["Identifier"] >> g["LEFTARROW"] >> g["Expression"];
        g["Definition"].set_action([](Ctx&, TreePtr node) -> NodePtr {
            auto id = find_named(node, "Identifier");
            auto expr = find_named(node, "Expression");
            if (!id || !id->value)
                return nullptr;
            // Span the whole Definition (name <- body); position diagnostics
            // (e.g. undefined-rule references inside the body) at the def.
            auto def = PegAstNode::make(NodeKind::Definition, id->value->text,
                                        node->start_offset, node->end_offset);
            if (expr && expr->value)
                def->children.push_back(expr->value);
            return def;
        });

        // ==================================================================
        // Grammar — Spacing Definition+ EndOfFile
        // ==================================================================
        g["Grammar"] = g["Spacing"] >> +g["Definition"] >> g["EndOfFile"];
        // Grammar: pass through all Definition children's values.
        g["Grammar"].set_action([](Ctx&, TreePtr node) -> NodePtr {
            // Return the first definition; caller can inspect the full tree
            // for multiple definitions. This is a convention: the tree's
            // children carry all Definition nodes.
            return nullptr;
        });

        g.set_start("Grammar");
        return g;
    }();
    return g;
}

} // namespace peg
