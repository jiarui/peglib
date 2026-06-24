#pragma once
#include "peglib/Grammar.h"
#include "peglib/PegAst.h"
#include "peglib/Rule.h"

#include <array>
#include <cctype>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace peg
{

// ---------------------------------------------------------------------------
// Meta-grammar: a PEG grammar that parses PEG text into PegAstNode trees.
//
// Uses peglib's typed-action model (Model A). Each rule's body is assigned
// via `auto h = (g["r"] = body);` which yields a RuleHandle carrying the
// body's static type; h.set_action<F> is compile-time-checked against the
// body's derived result type and bridges into the type-erased storage.
//
// Terminal model:
//   - g.terminal(x)  → void   (filtered; punctuation tokens never appear as
//                              action parameters — no find_named needed)
//   - g.token(x)     → char   (kept; would be used if a token's identity
//                              mattered to the action — the meta-grammar
//                              happens to build AST from source spans instead)
//
// Transparent rules (Comment/Spacing/Char/punctuation) appear as null typed
// arguments where they're sub-rules of a sequence: e.g. Identifier's body is
// (IdentifierRaw >> Spacing), so its action receives
// (Ctx&, Span, NodePtr idraw, NodePtr /*spacing*/) — Spacing's null value
// is an honest, positionally-stable placeholder. The old find_named/pass_through
// string-search workarounds are gone: every child result is now positional.
// ---------------------------------------------------------------------------

inline const Grammar<char, PegAstNodePtr>& meta_grammar()
{
    using Ctx = PegParseCtx;
    using NodePtr = PegAstNodePtr;

    static Grammar<char, PegAstNodePtr> grammar = []() {
        Grammar<char, PegAstNodePtr> g;

        auto any_char = g.terminal([](char c) { return c != '\0'; });
        auto non_nl = g.terminal([](char c) { return c != '\n' && c != '\0'; });

        // ==================================================================
        // Whitespace and comments (transparent — return nullptr).
        //
        // These rules' bodies contain transparent sub-rules (WSChar/Comment)
        // whose static result type is NodePtr (a Rule always has result
        // node_type, even when its action returns null at runtime). So e.g.
        // Spacing = *(WSChar|Comment) has result_of vector<NodePtr>. The
        // action doesn't care about those (all-null) values, so we use the
        // untyped hook for the transparent rules — cleaner than spelling out
        // null vector args.
        // ==================================================================
        g["Comment"] = g.terminal('#') >> *non_nl >> -g.terminal('\n');
        g["Comment"].set_action(
            [](Ctx&, const PegParseCtx::ParseTreeNodePtr&) -> NodePtr { return nullptr; });

        g["WSChar"] = g.terminal(' ') | g.terminal('\t') | g.terminal('\r') | g.terminal('\n');
        g["WSChar"].set_action(
            [](Ctx&, const PegParseCtx::ParseTreeNodePtr&) -> NodePtr { return nullptr; });

        g["Spacing"] = *(g["WSChar"] | g["Comment"]);
        g["Spacing"].set_action(
            [](Ctx&, const PegParseCtx::ParseTreeNodePtr&) -> NodePtr { return nullptr; });

        g["EndOfFile"] = !any_char;
        g["EndOfFile"].set_action(
            [](Ctx&, const PegParseCtx::ParseTreeNodePtr&) -> NodePtr { return nullptr; });

        // ==================================================================
        // Char — transparent (decoded by Literal/Class actions via spans).
        // ==================================================================
        g["Escape"] = g.terminal('\\') >>
                      (g.terminal('n') | g.terminal('r') | g.terminal('t') | g.terminal('\'') |
                       g.terminal('"') | g.terminal('\\') | g.terminal('[') | g.terminal(']'));
        g["Escape"].set_action(
            [](Ctx&, const PegParseCtx::ParseTreeNodePtr&) -> NodePtr { return nullptr; });

        g["RawChar"] = g.terminal([](char c) { return c != '\\' && c != '\n' && c != '\0'; });
        g["RawChar"].set_action(
            [](Ctx&, const PegParseCtx::ParseTreeNodePtr&) -> NodePtr { return nullptr; });

        g["Char"] = g["Escape"] | g["RawChar"];
        g["Char"].set_action(
            [](Ctx&, const PegParseCtx::ParseTreeNodePtr&) -> NodePtr { return nullptr; });

        // ==================================================================
        // Identifier
        // ==================================================================
        g["IdentStart"] = g.terminal('a', 'z') | g.terminal('A', 'Z') | g.terminal('_');
        g["IdentStart"].set_action(
            [](Ctx&, const PegParseCtx::ParseTreeNodePtr&) -> NodePtr { return nullptr; });

        g["IdentCont"] = g["IdentStart"] | g.terminal('0', '9');
        g["IdentCont"].set_action(
            [](Ctx&, const PegParseCtx::ParseTreeNodePtr&) -> NodePtr { return nullptr; });

        // IdentifierRaw: IdentStart >> *IdentCont. IdentStart/IdentCont are
        // transparent Rules (result_of NodePtr, runtime null), so result_of is
        // (NodePtr, vector<NodePtr>). The action only needs the span → untyped.
        g["IdentifierRaw"] = g["IdentStart"] >> *g["IdentCont"];
        g["IdentifierRaw"].set_action(
            [](Ctx& ctx, const PegParseCtx::ParseTreeNodePtr& node) -> NodePtr {
                std::string name =
                    ctx.input().slice(node->start_offset, node->end_offset - node->start_offset);
                return PegAstNode::make(
                    NodeKind::RuleRef, std::move(name), node->start_offset, node->end_offset);
            });
        // Identifier: IdentifierRaw >> Spacing. Spacing is a transparent Rule
        // (result_of NodePtr, runtime null). Typed action forwards idraw.
        {
            auto h = (g["Identifier"] = g["IdentifierRaw"] >> g["Spacing"]);
            h.set_action(
                [](Ctx&, Span, const NodePtr& idraw, const NodePtr& /*spacing*/) -> NodePtr {
                    return idraw;
                });
        }

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

        // SingleQuotedCore/DoubleQuotedCore: quote *(!quote Char) quote.
        // The body references Char (a transparent Rule), so result_of is a
        // vector<NodePtr>; the action only needs the source span → untyped.
        g["SingleQuotedCore"] =
            g.terminal('\'') >> *(!g.terminal('\'') >> g["Char"]) >> g.terminal('\'');
        g["SingleQuotedCore"].set_action(
            [decode_peg_escapes](Ctx& ctx, const PegParseCtx::ParseTreeNodePtr& node) -> NodePtr {
                std::size_t s = node->start_offset + 1;
                std::size_t e = node->end_offset - 1;
                std::string body = ctx.input().slice(s, e - s);
                return PegAstNode::make(NodeKind::Literal,
                                        decode_peg_escapes(body),
                                        node->start_offset,
                                        node->end_offset);
            });

        g["DoubleQuotedCore"] =
            g.terminal('"') >> *(!g.terminal('"') >> g["Char"]) >> g.terminal('"');
        g["DoubleQuotedCore"].set_action(
            [decode_peg_escapes](Ctx& ctx, const PegParseCtx::ParseTreeNodePtr& node) -> NodePtr {
                std::size_t s = node->start_offset + 1;
                std::size_t e = node->end_offset - 1;
                std::string body = ctx.input().slice(s, e - s);
                return PegAstNode::make(NodeKind::Literal,
                                        decode_peg_escapes(body),
                                        node->start_offset,
                                        node->end_offset);
            });

        // LiteralCore: SingleQuotedCore | DoubleQuotedCore. Typed (forward).
        {
            auto h = (g["LiteralCore"] = g["SingleQuotedCore"] | g["DoubleQuotedCore"]);
            h.set_action([](Ctx&, Span, NodePtr core) -> NodePtr { return core; });
        }
        // Literal: LiteralCore >> Spacing. Typed (forward core, ignore spacing).
        {
            auto h = (g["Literal"] = g["LiteralCore"] >> g["Spacing"]);
            h.set_action(
                [](Ctx&, Span, const NodePtr& core, const NodePtr& /*spacing*/) -> NodePtr {
                    return core;
                });
        }

        // ==================================================================
        // Class — character class. Action captures the raw bracketed source.
        // ==================================================================
        g["Range"] = g["Char"] >> -(g.terminal('-') >> g["Char"]);
        g["Range"].set_action(
            [](Ctx&, const PegParseCtx::ParseTreeNodePtr&) -> NodePtr { return nullptr; });

        // ClassCore: "[" -"^"? *(!"]" Range) "]". References Range (transparent
        // Rule) → result_of has a vector<NodePtr>; action reads span → untyped.
        g["ClassCore"] = g.terminal('[') >> -g.terminal('^') >> *(!g.terminal(']') >> g["Range"]) >>
                         g.terminal(']');
        g["ClassCore"].set_action(
            [](Ctx& ctx, const PegParseCtx::ParseTreeNodePtr& node) -> NodePtr {
                std::string raw =
                    ctx.input().slice(node->start_offset, node->end_offset - node->start_offset);
                return PegAstNode::make(
                    NodeKind::CharClass, std::move(raw), node->start_offset, node->end_offset);
            });

        // Class: ClassCore >> Spacing. Typed (forward core).
        {
            auto h = (g["Class"] = g["ClassCore"] >> g["Spacing"]);
            h.set_action(
                [](Ctx&, Span, const NodePtr& core, const NodePtr& /*spacing*/) -> NodePtr {
                    return core;
                });
        }

        // ==================================================================
        // Punctuation tokens — match + trailing Spacing, transparent.
        // Bodies reference Spacing (a transparent Rule, result_of NodePtr), so
        // these are NOT void-typed; use the untyped hook (return nullptr).
        // ==================================================================
        auto make_punct = [&](const char* name, const auto& expr) {
            g[name] = expr;
            g[name].set_action(
                [](Ctx&, const PegParseCtx::ParseTreeNodePtr&) -> NodePtr { return nullptr; });
        };

        make_punct("LEFTARROW", g.terminal('<') >> g.terminal('-') >> g["Spacing"]);
        make_punct("SLASH", g.terminal('/') >> g["Spacing"]);
        make_punct("AND", g.terminal('&') >> g["Spacing"]);
        make_punct("NOT", g.terminal('!') >> g["Spacing"]);
        make_punct("QUESTION", g.terminal('?') >> g["Spacing"]);
        make_punct("STAR", g.terminal('*') >> g["Spacing"]);
        make_punct("PLUS", g.terminal('+') >> g["Spacing"]);
        make_punct("OPEN", g.terminal('(') >> g["Spacing"]);
        make_punct("CLOSE", g.terminal(')') >> g["Spacing"]);

        // CUT — tilde. A standalone primary (leaf). Emits a Cut AST node.
        g["CUT"] = g.terminal('~') >> g["Spacing"];
        g["CUT"].set_action([](Ctx&, const PegParseCtx::ParseTreeNodePtr& node) -> NodePtr {
            return PegAstNode::make(NodeKind::Cut, {}, node->start_offset, node->end_offset);
        });

        make_punct("LESSTHAN", g.terminal('<') >> !g.terminal('-') >> g["Spacing"]);
        make_punct("GREATERTHAN", g.terminal('>') >> g["Spacing"]);
        make_punct("PERCENT_RECOVER",
                   g.terminal('%') >> g.terminal('r') >> g.terminal('e') >> g.terminal('c') >>
                       g.terminal('o') >> g.terminal('v') >> g.terminal('e') >> g.terminal('r') >>
                       g["Spacing"]);

        g["DOT"] = g.terminal('.') >> g["Spacing"];
        g["DOT"].set_action([](Ctx&, const PegParseCtx::ParseTreeNodePtr& node) -> NodePtr {
            return PegAstNode::make(NodeKind::Dot, {}, node->start_offset, node->end_offset);
        });

        // ==================================================================
        // Primary
        //   Primary <- Identifier !LEFTARROW
        //             / OPEN Expression CLOSE
        //             / LESSTHAN Expression GREATERTHAN   (lexeme)
        //             / Literal / Class / DOT / CUT
        //
        // This is the trickiest rule: the alternation has 7 branches with
        // heterogeneous result types. The old action used find_named/has_named
        // to disambiguate. With typed actions, each branch must share a result
        // type (AlternationExpr requires it). All branches ultimately produce
        // a NodePtr, but their *intermediate* seq_result shapes differ:
        //   - Identifier !LEFTARROW           → (NodePtr)        // !term void
        //   - OPEN Expression CLOSE           → (NodePtr,NodePtr,NodePtr)
        //   - LESSTHAN Expression GREATERTHAN → (NodePtr,NodePtr,NodePtr)
        //   - Literal / Class / DOT / CUT     → NodePtr
        // Alternation requires all branches' result_of to be identical. They
        // are NOT (some are NodePtr scalars, some are tuples). So we cannot
        // attach a single typed action to this alternation directly.
        //
        // Solution: keep this one rule on the untyped hook (Rule::set_action),
        // which is the documented escape hatch for cases the positional model
        // can't express (heterogeneous alternation branches). The old
        // find_named/has_named helpers are removed from the library; this rule
        // reads node->children directly (a local, contained walk).
        // ==================================================================
        g["Primary"] = (g["Identifier"] >> !g["LEFTARROW"]) |
                       (g["OPEN"] >> g["Expression"] >> g["CLOSE"]) |
                       (g["LESSTHAN"] >> g["Expression"] >> g["GREATERTHAN"]) | g["Literal"] |
                       g["Class"] | g["DOT"] | g["CUT"];
        g["Primary"].set_action([](Ctx&, const PegParseCtx::ParseTreeNodePtr& node) -> NodePtr {
            // Lexeme form: wrap the inner Expression's value in a Lexeme node.
            for (auto& child : node->children) {
                if (child && child->name == "LESSTHAN") {
                    // Find the Expression sibling.
                    for (auto& c2 : node->children) {
                        if (c2 && c2->name == "Expression" && c2->value) {
                            auto n = PegAstNode::make(
                                NodeKind::Lexeme, {}, node->start_offset, node->end_offset);
                            n->children.push_back(c2->value);
                            return n;
                        }
                    }
                }
            }
            // All other forms: forward the single meaningful child's value.
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
        //
        // The postfix operators (QUESTION/STAR/PLUS) are transparent
        // punctuation rules: their NodePtr value is null, so the typed arg
        // can't carry the operator *identity* — only its presence. Since the
        // action must distinguish *, +, ? to emit the right NodeKind, we read
        // the operator's source span. This is the documented escape hatch
        // (untyped Rule::set_action) for a rule whose action needs data the
        // positional model doesn't surface (here: which of several
        // transparent tokens matched).
        // ==================================================================
        g["Suffixed"] = g["Primary"] >> -(g["QUESTION"] | g["STAR"] | g["PLUS"]);
        g["Suffixed"].set_action(
            [](Ctx& ctx, const PegParseCtx::ParseTreeNodePtr& node) -> NodePtr {
                NodePtr child_val;
                std::string op_text;
                for (auto& child : node->children) {
                    if (!child)
                        continue;
                    if (child->name == "Primary") {
                        child_val = child->value;
                    } else if (op_text.empty()) {
                        op_text = ctx.input().slice(child->start_offset,
                                                    child->end_offset - child->start_offset);
                    }
                }
                if (!child_val)
                    return nullptr;
                if (op_text.empty())
                    return child_val;
                NodeKind k = NodeKind::Optional;
                if (op_text.find('*') != std::string::npos)
                    k = NodeKind::Star;
                else if (op_text.find('+') != std::string::npos)
                    k = NodeKind::Plus;
                auto n = PegAstNode::make(k, {}, node->start_offset, node->end_offset);
                n->children.push_back(child_val);
                return n;
            });

        // ==================================================================
        // Prefixed — optional prefix (& !) then Suffixed. Same operator-
        // identity issue as Suffixed: use the untyped hook, read source.
        // ==================================================================
        g["Prefixed"] = -(g["AND"] | g["NOT"]) >> g["Suffixed"];
        g["Prefixed"].set_action(
            [](Ctx& ctx, const PegParseCtx::ParseTreeNodePtr& node) -> NodePtr {
                NodePtr child_val;
                std::string op_text;
                for (auto& child : node->children) {
                    if (!child)
                        continue;
                    if (child->name == "Suffixed") {
                        child_val = child->value;
                    } else if (op_text.empty()) {
                        op_text = ctx.input().slice(child->start_offset,
                                                    child->end_offset - child->start_offset);
                    }
                }
                if (!child_val)
                    return nullptr;
                if (op_text.empty())
                    return child_val;
                NodeKind k = (op_text.find('!') != std::string::npos) ? NodeKind::NotPred
                                                                      : NodeKind::AndPred;
                auto n = PegAstNode::make(k, {}, node->start_offset, node->end_offset);
                n->children.push_back(child_val);
                return n;
            });

        // ==================================================================
        // Sequence — one or more Prefixed. Typed: vector<NodePtr>.
        // ==================================================================
        {
            auto h = (g["Sequence"] = +g["Prefixed"]);
            h.set_action([](Ctx&, Span sp, std::vector<NodePtr> items) -> NodePtr {
                // Filter out nulls (transparent Prefixed results shouldn't
                // occur, but be defensive).
                std::vector<NodePtr> filtered;
                for (auto& it : items)
                    if (it)
                        filtered.push_back(std::move(it));
                if (filtered.empty())
                    return nullptr;
                if (filtered.size() == 1)
                    return std::move(filtered[0]);
                auto n = PegAstNode::make(NodeKind::Sequence, {}, sp.start, sp.end);
                n->children = std::move(filtered);
                return n;
            });
        }

        // ==================================================================
        // Expression — Sequence (SLASH Sequence)*
        // result_of = (NodePtr, vector<pair<NodePtr,NodePtr>>)
        //   first Sequence → NodePtr
        //   each (SLASH Sequence) iteration → (NodePtr /*slash*/, NodePtr)
        //     SLASH is transparent → null; Sequence → value.
        // ==================================================================
        {
            auto h = (g["Expression"] = g["Sequence"] >> *(g["SLASH"] >> g["Sequence"]));
            h.set_action([](Ctx&,
                            Span sp,
                            NodePtr first,
                            std::vector<std::tuple<NodePtr, NodePtr>> rest) -> NodePtr {
                if (!first)
                    return nullptr;
                if (rest.empty())
                    return first;
                auto n = PegAstNode::make(NodeKind::Choice, {}, sp.start, sp.end);
                n->children.push_back(first);
                for (auto& alt : rest) {
                    // tuple<SLASH (null), Sequence value>
                    auto& seq = std::get<1>(alt);
                    if (seq)
                        n->children.push_back(seq);
                }
                return n;
            });
        }

        // ==================================================================
        // Recovery suffix (peglib extension).
        // ==================================================================
        // RECOVER_EOF/EOL: keyword + !IdentCont + Spacing. References Spacing
        // (transparent Rule) → untyped.
        g["RECOVER_EOF"] = g.terminal('e') >> g.terminal('o') >> g.terminal('f') >>
                           !g["IdentCont"] >> g["Spacing"];
        g["RECOVER_EOF"].set_action(
            [](Ctx&, const PegParseCtx::ParseTreeNodePtr&) -> NodePtr { return nullptr; });
        g["RECOVER_EOL"] = g.terminal('e') >> g.terminal('o') >> g.terminal('l') >>
                           !g["IdentCont"] >> g["Spacing"];
        g["RECOVER_EOL"].set_action(
            [](Ctx&, const PegParseCtx::ParseTreeNodePtr&) -> NodePtr { return nullptr; });

        // SyncChar: "'" Char "'" Spacing. References Char/Spacing → untyped.
        g["SyncChar"] = g.terminal('\'') >> g["Char"] >> g.terminal('\'') >> g["Spacing"];
        g["SyncChar"].set_action(
            [decode_peg_escapes](Ctx& ctx, const PegParseCtx::ParseTreeNodePtr& node) -> NodePtr {
                std::size_t s = node->start_offset + 1;
                std::size_t e = node->end_offset - 1;
                std::string body = ctx.input().slice(s, e - s);
                return PegAstNode::make(NodeKind::Literal,
                                        decode_peg_escapes(body),
                                        node->start_offset,
                                        node->end_offset);
            });

        make_punct("OPENBRACE", g.terminal('{') >> g["Spacing"]);
        make_punct("CLOSEBRACE", g.terminal('}') >> g["Spacing"]);
        make_punct("COMMA", g.terminal(',') >> g["Spacing"]);

        // RecoverSpec: eof / eol / { SyncChar (COMMA? SyncChar)* }
        // Heterogeneous branches → untyped hook reading source (the old comment
        // noted has_named was unreliable; source-reading is robust).
        g["RecoverSpec"] =
            g["RECOVER_EOF"] | g["RECOVER_EOL"] |
            (g["OPENBRACE"] >> -g["SyncChar"] >>
             *(g["COMMA"] >> g["SyncChar"] | !g["CLOSEBRACE"] >> g["SyncChar"]) >> g["CLOSEBRACE"]);
        g["RecoverSpec"].set_action(
            [](Ctx& ctx, const PegParseCtx::ParseTreeNodePtr& node) -> NodePtr {
                auto span =
                    ctx.input().slice(node->start_offset, node->end_offset - node->start_offset);
                if (span == "eof")
                    return PegAstNode::make(
                        NodeKind::Recovery, "eof", node->start_offset, node->end_offset);
                if (span == "eol")
                    return PegAstNode::make(
                        NodeKind::Recovery, "eol", node->start_offset, node->end_offset);
                // Set form: concatenate SyncChar children's text into "set:XYZ".
                std::string spec = "set:";
                // Pre-order DFS that stops at each SyncChar match (don't descend).
                std::function<void(const PegParseCtx::ParseTreeNodePtr&)> collect =
                    [&](const PegParseCtx::ParseTreeNodePtr& n) {
                        if (!n)
                            return;
                        if (n->name == "SyncChar" && n->value) {
                            if (!n->value->text.empty())
                                spec += n->value->text;
                            return;
                        }
                        for (auto& c : n->children)
                            collect(c);
                    };
                collect(node);
                return PegAstNode::make(
                    NodeKind::Recovery, std::move(spec), node->start_offset, node->end_offset);
            });

        {
            // Recovery = PERCENT_RECOVER OPEN RecoverSpec CLOSE
            // result_of = (NodePtr,NodePtr,NodePtr,NodePtr) — PERCENT_RECOVER,
            // OPEN, CLOSE are transparent (null); RecoverSpec carries the value.
            auto h = (g["Recovery"] =
                          g["PERCENT_RECOVER"] >> g["OPEN"] >> g["RecoverSpec"] >> g["CLOSE"]);
            h.set_action([](Ctx&,
                            Span,
                            const NodePtr& /*pct*/,
                            const NodePtr& /*open*/,
                            const NodePtr& spec,
                            const NodePtr& /*close*/) -> NodePtr { return spec; });
        }

        // ==================================================================
        // Definition — Identifier LEFTARROW Expression Recovery?
        // result_of = (NodePtr, NodePtr, NodePtr, optional<NodePtr>)
        //   Identifier → value; LEFTARROW → null; Expression → value;
        //   Recovery → optional (present/absent).
        // ==================================================================
        {
            auto h = (g["Definition"] =
                          g["Identifier"] >> g["LEFTARROW"] >> g["Expression"] >> -g["Recovery"]);
            h.set_action([](Ctx&,
                            Span sp,
                            const NodePtr& id,
                            const NodePtr& /*leftarrow*/,
                            const NodePtr& expr,
                            std::optional<NodePtr> recovery) -> NodePtr {
                if (!id)
                    return nullptr;
                auto def = PegAstNode::make(NodeKind::Definition, id->text, sp.start, sp.end);
                if (expr)
                    def->children.push_back(expr);
                if (recovery && *recovery)
                    def->children.push_back(*recovery);
                return def;
            });
        }

        // ==================================================================
        // Grammar — Spacing Definition+ EndOfFile
        // Each reference is a Rule → result_of NodePtr. So result_of =
        // (NodePtr /*spacing*/, vector<NodePtr>, NodePtr /*eof*/).
        // ==================================================================
        {
            auto h = (g["Grammar"] = g["Spacing"] >> +g["Definition"] >> g["EndOfFile"]);
            h.set_action([](Ctx&,
                            Span,
                            const NodePtr& /*spacing*/,
                            std::vector<NodePtr> defs,
                            const NodePtr& /*eof*/) -> NodePtr {
                // Return the first definition; GrammarCompiler walks the tree
                // for multiple definitions.
                return defs.empty() ? nullptr : defs[0];
            });
        }

        g.set_start("Grammar");
        return g;
    }();
    return grammar;
}

} // namespace peg
