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

inline const Grammar<char, PegAstNodePtr>& meta_grammar()
{
    using Ctx = PegParseCtx;
    using NodePtr = PegAstNodePtr;
    using TreePtr = PegParseCtx::ParseTreeNodePtr;

    using CTerm = TerminalExpr<Ctx, char>;
    using CTermRange = TerminalExpr<Ctx, std::array<char, 2>>;
    using CTermPred = TerminalExpr<Ctx, bool (*)(char)>;

    static Grammar<char, PegAstNodePtr> g = []() {
        Grammar<char, PegAstNodePtr> g;

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
            std::string name =
                ctx.substr(node->start_offset, node->end_offset - node->start_offset);
            return PegAstNode::make(
                NodeKind::RuleRef, std::move(name), node->start_offset, node->end_offset);
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
            // node spans the full 'xyz' (with quotes). Body is [1, -1).
            std::size_t s = node->start_offset + 1;
            std::size_t e = node->end_offset - 1;
            std::string body = ctx.substr(s, e - s);
            return PegAstNode::make(
                NodeKind::Literal, decode_peg_escapes(body), node->start_offset, node->end_offset);
        });

        g["DoubleQuotedCore"] = CTerm('"') >> *(!CTerm('"') >> g["Char"]) >> CTerm('"');
        g["DoubleQuotedCore"].set_action([decode_peg_escapes](Ctx& ctx, TreePtr node) -> NodePtr {
            std::size_t s = node->start_offset + 1;
            std::size_t e = node->end_offset - 1;
            std::string body = ctx.substr(s, e - s);
            return PegAstNode::make(
                NodeKind::Literal, decode_peg_escapes(body), node->start_offset, node->end_offset);
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
            std::string raw = ctx.substr(node->start_offset, node->end_offset - node->start_offset);
            return PegAstNode::make(
                NodeKind::CharClass, std::move(raw), node->start_offset, node->end_offset);
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

        // CUT — tilde. A standalone primary (leaf), not a prefix operator
        // (unlike & and !). Emits a Cut AST node directly. Action is set
        // below (it produces a value, so it can't use the transparent
        // make_punct helper).
        make_punct("CUT", CTerm('~') >> g["Spacing"]);

        // LESSTHAN / GREATERTHAN — lexeme delimiters `< e >`. LESSTHAN must
        // disambiguate from LEFTARROW (`<-`): the `!CTerm('-')` lookahead
        // rejects the arrow form so `<-` is not misparsed as a lexeme open.
        make_punct("LESSTHAN", CTerm('<') >> !CTerm('-') >> g["Spacing"]);
        make_punct("GREATERTHAN", CTerm('>') >> g["Spacing"]);

        // PERCENT_RECOVER — the directive prefix `%recover`. Matched as a
        // contiguous keyword so `%recoverX` doesn't partially match; the
        // trailing Spacing is the natural token boundary.
        make_punct("PERCENT_RECOVER",
                   CTerm('%') >> CTerm('r') >> CTerm('e') >> CTerm('c') >> CTerm('o') >> CTerm('v') >>
                       CTerm('e') >> CTerm('r') >> g["Spacing"]);

        g["DOT"] = CTerm('.') >> g["Spacing"];
        g["DOT"].set_action([](Ctx&, TreePtr node) -> NodePtr {
            return PegAstNode::make(NodeKind::Dot, {}, node->start_offset, node->end_offset);
        });

        // CUT action: emit a Cut AST node (leaf, no payload, no children).
        g["CUT"].set_action([](Ctx&, TreePtr node) -> NodePtr {
            return PegAstNode::make(NodeKind::Cut, {}, node->start_offset, node->end_offset);
        });

        // ==================================================================
        // Primary
        //   Primary <- Identifier !LEFTARROW
        //             / OPEN Expression CLOSE
        //             / LESSTHAN Expression GREATERTHAN   (lexeme)
        //             / Literal / Class / DOT / CUT
        //
        // CUT is a leaf primary (no child). Lexeme wraps its inner
        // Expression in a Lexeme AST node.
        // ==================================================================
        g["Primary"] = (g["Identifier"] >> !g["LEFTARROW"]) |
                       (g["OPEN"] >> g["Expression"] >> g["CLOSE"]) |
                       (g["LESSTHAN"] >> g["Expression"] >> g["GREATERTHAN"]) | g["Literal"] |
                       g["Class"] | g["DOT"] | g["CUT"];

        // Primary: pass through the meaningful child's value, except for
        // Lexeme (which needs to wrap its inner Expression) — detect that
        // by finding LESSTHAN among the children.
        g["Primary"].set_action([](Ctx&, TreePtr node) -> NodePtr {
            // Lexeme form: wrap the inner Expression's value in a Lexeme node.
            for (auto& child : node->children) {
                if (child && has_named(child, "LESSTHAN")) {
                    auto inner = find_named(node, "Expression");
                    if (!inner || !inner->value)
                        return nullptr;
                    auto n = PegAstNode::make(
                        NodeKind::Lexeme, {}, node->start_offset, node->end_offset);
                    n->children.push_back(inner->value);
                    return n;
                }
            }
            // All other forms: pass through the meaningful child's value
            // (Identifier/Literal/Class/DOT/CUT/OPEN-Expression-CLOSE all
            // either set node->value directly via their own action or have
            // a single meaningful child whose value we forward).
            if (node->value)
                return node->value;
            for (auto& child : node->children) {
                if (child && child->value)
                    return child->value;
            }
            return nullptr;
        });

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
        // Recovery suffix (peglib extension).
        //
        //   Definition <- Identifier LEFTARROW Expression Recovery?
        //   Recovery   <- PERCENT_RECOVER OPEN RecoverSpec CLOSE
        //   RecoverSpec <- eof
        //                 / eol
        //                 / OPENBRACE (SyncChar (COMMA? SyncChar)*)? CLOSEBRACE
        //
        // The sync spec is encoded into the Recovery node's text:
        //   "set:XY" — recover_set on chars X, Y, ...
        //   "eof"    — recover_eof
        //   "eol"    — recover_eol
        // (No predicate form in text — recover_predicate stays C++-API-only.)
        //
        // SyncChar reuses the existing Char rule (so escape sequences work),
        // but restricts to single characters — the action reads the matched
        // span directly from the source.
        // ==================================================================

        // eof / eol keywords inside RecoverSpec. Matched as contiguous
        // identifiers followed by a word boundary (next char is not an
        // ident-continuation) so `eofx` is not partially matched.
        g["RECOVER_EOF"] = CTerm('e') >> CTerm('o') >> CTerm('f') >> !g["IdentCont"] >> g["Spacing"];
        g["RECOVER_EOF"].set_action([](Ctx&, TreePtr) -> NodePtr { return nullptr; });
        g["RECOVER_EOL"] = CTerm('e') >> CTerm('o') >> CTerm('l') >> !g["IdentCont"] >> g["Spacing"];
        g["RECOVER_EOL"].set_action([](Ctx&, TreePtr) -> NodePtr { return nullptr; });

        // A single sync character: a single-quoted Char. Reuse the existing
        // Char rule so escapes (\n, \t, ...) work. The action extracts the
        // decoded character from the source span (quotes stripped).
        g["SyncChar"] = CTerm('\'') >> g["Char"] >> CTerm('\'') >> g["Spacing"];
        g["SyncChar"].set_action([decode_peg_escapes](Ctx& ctx, TreePtr node) -> NodePtr {
            // node spans 'x' (with quotes). Body is [1, -1).
            std::size_t s = node->start_offset + 1;
            std::size_t e = node->end_offset - 1;
            std::string body = ctx.substr(s, e - s);
            return PegAstNode::make(
                NodeKind::Literal, decode_peg_escapes(body), node->start_offset, node->end_offset);
        });

        // RecoverSpec: eof / eol / { SyncChar (COMMA? SyncChar)* }
        // The optional COMMA between sync chars is purely cosmetic.
        make_punct("OPENBRACE", CTerm('{') >> g["Spacing"]);
        make_punct("CLOSEBRACE", CTerm('}') >> g["Spacing"]);
        make_punct("COMMA", CTerm(',') >> g["Spacing"]);

        g["RecoverSpec"] =
            g["RECOVER_EOF"] | g["RECOVER_EOL"] |
            (g["OPENBRACE"] >> -g["SyncChar"] >>
             *(g["COMMA"] >> g["SyncChar"] | !g["CLOSEBRACE"] >> g["SyncChar"]) >> g["CLOSEBRACE"]);
        g["RecoverSpec"].set_action([](Ctx& ctx, TreePtr node) -> NodePtr {
            // Distinguish eof/eol/set by reading the matched source text
            // directly. Relying on has_named("RECOVER_EOF"/"RECOVER_EOL")
            // is unreliable because a failed first-alternative attempt can
            // leave a named node in the parse subtree.
            auto span = ctx.substr(node->start_offset, node->end_offset - node->start_offset);
            if (span == "eof")
                return PegAstNode::make(
                    NodeKind::Recovery, "eof", node->start_offset, node->end_offset);
            if (span == "eol")
                return PegAstNode::make(
                    NodeKind::Recovery, "eol", node->start_offset, node->end_offset);
            // Set form: concatenate all SyncChar values into "set:XYZ".
            auto chars = collect_named(node, "SyncChar");
            std::string spec = "set:";
            for (auto& c : chars) {
                if (c && !c->text.empty())
                    spec += c->text;
            }
            return PegAstNode::make(
                NodeKind::Recovery, std::move(spec), node->start_offset, node->end_offset);
        });

        // Recovery wrapper: %recover ( RecoverSpec ).
        g["Recovery"] = g["PERCENT_RECOVER"] >> g["OPEN"] >> g["RecoverSpec"] >> g["CLOSE"];
        g["Recovery"].set_action(pass_through);

        // ==================================================================
        // Definition — Identifier LEFTARROW Expression Recovery?
        //
        // Children in node->children: Identifier, LEFTARROW, Expression,
        // optional Recovery. find_named locates them by rule name. The
        // body Expression goes into def->children[0]; if Recovery is
        // present, it goes into def->children[1].
        // ==================================================================
        g["Definition"] = g["Identifier"] >> g["LEFTARROW"] >> g["Expression"] >> -g["Recovery"];
        g["Definition"].set_action([](Ctx&, TreePtr node) -> NodePtr {
            auto id = find_named(node, "Identifier");
            auto expr = find_named(node, "Expression");
            auto recovery = find_named(node, "Recovery");
            if (!id || !id->value)
                return nullptr;
            // Span the whole Definition (name <- body); position diagnostics
            // (e.g. undefined-rule references inside the body) at the def.
            auto def = PegAstNode::make(
                NodeKind::Definition, id->value->text, node->start_offset, node->end_offset);
            if (expr && expr->value)
                def->children.push_back(expr->value);
            if (recovery && recovery->value)
                def->children.push_back(recovery->value);
            return def;
        });

        // ==================================================================
        // Grammar — Spacing Definition+ EndOfFile
        // ==================================================================
        g["Grammar"] = g["Spacing"] >> +g["Definition"] >> g["EndOfFile"];
        // Grammar: pass through all Definition children's values.
        g["Grammar"].set_action([](Ctx&, TreePtr /*node*/) -> NodePtr {
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
