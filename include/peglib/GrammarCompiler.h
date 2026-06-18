#pragma once
#include "peglib/DynExpr.h"
#include "peglib/Grammar.h"
#include "peglib/MetaGrammar.h"
#include "peglib/ParseError.h"
#include "peglib/PegAst.h"
#include "peglib/Rule.h"
#include "peglib/Terminals.h"

#include <cctype>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace peg
{

// ---------------------------------------------------------------------------
// RuleRefWrapper: wraps a RuleProxy so it can be stored as a
// shared_ptr<ParsingExprInterface> in Dyn* expression trees.
// This is needed because RuleProxy is a ParsingExpr (CRTP) but not
// polymorphically compatible with ParsingExprInterface without wrapping.
// ---------------------------------------------------------------------------
template<typename Context>
struct RuleRefWrapper : parsers::ParsingExpr<Context, RuleRefWrapper<Context>>
{
    using ParseResult = typename Context::ParseResult;
    parsers::RuleProxy<Context> proxy;

    explicit RuleRefWrapper(parsers::RuleProxy<Context> p) : proxy(std::move(p)) {}

    ParseResult parse(Context& ctx) const override { return proxy.parse(ctx); }
};

// ---------------------------------------------------------------------------
// GrammarCompiler: compiles a PEG AST (produced by meta_grammar) into a
// runtime Grammar<> that can parse input text.
//
// Two entry points:
//   from_string(text)           — throws peg::ParseError on bad grammar
//   try_from_string(text, out)  — returns false, fills Diagnostic on error
//
// The compiler walks each Definition's body AST recursively, building
// DynExpr nodes (type-erased parsing expressions) that reference rules
// via Grammar::operator[]. The first definition becomes the start rule.
// ---------------------------------------------------------------------------

class GrammarCompiler
{
public:
    using DefaultCtx = Context<std::span<const char>>;
    using DefaultGrammar = Grammar<DefaultCtx>;

    // Parse PEG text and compile into a working Grammar.
    // Throws peg::ParseError if the text is not valid PEG.
    static DefaultGrammar from_string(std::string_view text)
    {
        DefaultGrammar out;
        Diagnostic err{0, {}};
        if (!try_from_string(text, out, err)) {
            throw ParseError{err.position(), err.expected()};
        }
        return out;
    }

    // Non-throwing version. Returns true on success; on failure returns
    // false and fills `err` with diagnostic details.
    static bool try_from_string(std::string_view text, DefaultGrammar& out, Diagnostic& err)
    {
        // Phase 1: parse PEG text with the meta-grammar.
        auto& mg = meta_grammar();
        std::string input{text};
        PegParseCtx ctx{input};
        auto tree = mg.parse_tree("Grammar", ctx);
        if (!tree) {
            if (auto d = ctx.take_error()) {
                err = std::move(*d);
            } else {
                err = Diagnostic{0, {}};
            }
            return false;
        }

        // Phase 2: collect all Definition AST nodes.
        auto defs = collect_definitions(tree);
        if (defs.empty()) {
            err = Diagnostic{0, {}};
            return false;
        }

        // Phase 3: compile each definition into the output Grammar.
        // First pass: create all rule names (forward declarations).
        for (const auto& def : defs) {
            out[def->text]; // lazily creates the rule
        }
        // Second pass: assign compiled bodies.
        bool first = true;
        for (const auto& def : defs) {
            auto body = compile(def->children[0], out);
            out[def->text] = body;
            if (first) {
                out.set_start(def->text);
                first = false;
            }
        }
        return true;
    }

private:
    using NodePtr = PegAstNodePtr;
    using InterfacePtr = std::shared_ptr<parsers::ParsingExprInterface<DefaultCtx>>;

    // Recursively collect all Definition nodes from the parse tree.
    static std::vector<NodePtr> collect_definitions(const PegParseCtx::ParseTreeNodePtr& tree)
    {
        std::vector<NodePtr> defs;
        std::function<void(const PegParseCtx::ParseTreeNodePtr&)> walk =
            [&](const PegParseCtx::ParseTreeNodePtr& n) {
                if (!n)
                    return;
                if (n->name == "Definition" && n->value) {
                    defs.push_back(n->value);
                    return;
                }
                for (auto& c : n->children)
                    walk(c);
            };
        walk(tree);
        return defs;
    }

    // Compile an AST node into a DynExpr. The Grammar reference is used
    // to resolve RuleRef nodes via operator[].
    static parsers::DynExpr<DefaultCtx> compile(const NodePtr& node, DefaultGrammar& g)
    {
        if (!node) {
            throw std::runtime_error{"GrammarCompiler: null AST node"};
        }
        switch (node->kind) {
        case NodeKind::RuleRef:
            return compile_ruleref(node, g);
        case NodeKind::Literal:
            return compile_literal(node);
        case NodeKind::CharClass:
            return compile_charclass(node);
        case NodeKind::Dot:
            return compile_dot();
        case NodeKind::Sequence:
            return compile_sequence(node, g);
        case NodeKind::Choice:
            return compile_choice(node, g);
        case NodeKind::Optional:
            return compile_optional(node, g);
        case NodeKind::Star:
            return compile_repeat(node, g, 0, -1);
        case NodeKind::Plus:
            return compile_repeat(node, g, 1, -1);
        case NodeKind::AndPred:
            return compile_andpred(node, g);
        case NodeKind::NotPred:
            return compile_notpred(node, g);
        default:
            throw std::runtime_error{"GrammarCompiler: unhandled NodeKind " +
                                     std::to_string(static_cast<int>(node->kind))};
        }
    }

    // RuleRef: wrap the Grammar's RuleProxy as a DynExpr.
    static parsers::DynExpr<DefaultCtx> compile_ruleref(const NodePtr& node, DefaultGrammar& g)
    {
        // operator[] returns a RuleProxy which is itself a ParsingExpr.
        // We type-erase it by wrapping in DynExpr.
        auto proxy = g[node->text];
        // RuleProxy inherits ParsingExpr, but DynExpr needs a
        // shared_ptr<ParsingExprInterface>. We create a DynExpr that
        // captures the RuleProxy by value (it's cheap — shared_ptr + name).
        return parsers::DynExpr<DefaultCtx>{
            std::make_shared<RuleRefWrapper<DefaultCtx>>(std::move(proxy))};
    }

    // Literal: TerminalSeqExpr matching the decoded string.
    static parsers::DynExpr<DefaultCtx> compile_literal(const NodePtr& node)
    {
        auto impl = std::make_shared<parsers::TerminalSeqExpr<DefaultCtx, std::string>>(node->text);
        return parsers::DynExpr<DefaultCtx>{impl};
    }

    // CharClass: parse the raw [a-z^0-9_] source and build a terminal
    // backed by a std::set<char> (symbolConsumable already supports sets).
    static parsers::DynExpr<DefaultCtx> compile_charclass(const NodePtr& node)
    {
        std::set<char> chars;
        bool negated = false;
        std::size_t i = 1; // skip '['
        if (i < node->text.size() && node->text[i] == '^') {
            negated = true;
            ++i;
        }
        while (i < node->text.size() && node->text[i] != ']') {
            char lo = decode_char(node->text, i);
            if (i + 1 < node->text.size() && node->text[i] == '-' && node->text[i + 1] != ']') {
                ++i; // skip '-'
                char hi = decode_char(node->text, i);
                for (int c = static_cast<unsigned char>(lo); c <= static_cast<unsigned char>(hi);
                     ++c) {
                    chars.insert(static_cast<char>(c));
                }
            } else {
                chars.insert(lo);
            }
        }
        if (negated) {
            // Build the complement set.
            std::set<char> complement;
            for (int c = 0; c < 256; ++c) {
                if (chars.find(static_cast<char>(c)) == chars.end()) {
                    complement.insert(static_cast<char>(c));
                }
            }
            chars = std::move(complement);
        }
        auto impl = std::make_shared<parsers::TerminalExpr<DefaultCtx, std::set<char>>>(chars);
        return parsers::DynExpr<DefaultCtx>{impl};
    }

    // Dot: any character (except NUL).
    static parsers::DynExpr<DefaultCtx> compile_dot()
    {
        using PredType = bool (*)(char);
        auto impl = std::make_shared<parsers::TerminalExpr<DefaultCtx, PredType>>(
            +[](char c) { return c != '\0'; });
        return parsers::DynExpr<DefaultCtx>{impl};
    }

    // Sequence: DynSequenceExpr with compiled children.
    static parsers::DynExpr<DefaultCtx> compile_sequence(const NodePtr& node, DefaultGrammar& g)
    {
        std::vector<InterfacePtr> children;
        for (auto& c : node->children) {
            children.push_back(extract_impl(compile(c, g)));
        }
        auto impl = std::make_shared<parsers::DynSequenceExpr<DefaultCtx>>(std::move(children));
        return parsers::DynExpr<DefaultCtx>{impl};
    }

    // Choice: DynAlternationExpr with compiled alternatives.
    static parsers::DynExpr<DefaultCtx> compile_choice(const NodePtr& node, DefaultGrammar& g)
    {
        std::vector<InterfacePtr> children;
        for (auto& c : node->children) {
            children.push_back(extract_impl(compile(c, g)));
        }
        auto impl = std::make_shared<parsers::DynAlternationExpr<DefaultCtx>>(std::move(children));
        return parsers::DynExpr<DefaultCtx>{impl};
    }

    // Optional: DynRepeatExpr with min=0, max=1.
    static parsers::DynExpr<DefaultCtx> compile_optional(const NodePtr& node, DefaultGrammar& g)
    {
        auto child = extract_impl(compile(node->children[0], g));
        auto impl = std::make_shared<parsers::DynRepeatExpr<DefaultCtx>>(child, 0, 1);
        return parsers::DynExpr<DefaultCtx>{impl};
    }

    // Star/Plus: DynRepeatExpr.
    static parsers::DynExpr<DefaultCtx>
    compile_repeat(const NodePtr& node, DefaultGrammar& g, std::size_t min, std::int64_t max)
    {
        auto child = extract_impl(compile(node->children[0], g));
        auto impl = std::make_shared<parsers::DynRepeatExpr<DefaultCtx>>(child, min, max);
        return parsers::DynExpr<DefaultCtx>{impl};
    }

    // AndPred: DynAndExpr.
    static parsers::DynExpr<DefaultCtx> compile_andpred(const NodePtr& node, DefaultGrammar& g)
    {
        auto child = extract_impl(compile(node->children[0], g));
        auto impl = std::make_shared<parsers::DynAndExpr<DefaultCtx>>(child);
        return parsers::DynExpr<DefaultCtx>{impl};
    }

    // NotPred: DynNotExpr.
    static parsers::DynExpr<DefaultCtx> compile_notpred(const NodePtr& node, DefaultGrammar& g)
    {
        auto child = extract_impl(compile(node->children[0], g));
        auto impl = std::make_shared<parsers::DynNotExpr<DefaultCtx>>(child);
        return parsers::DynExpr<DefaultCtx>{impl};
    }

    // Extract the shared_ptr<ParsingExprInterface> from a DynExpr.
    static InterfacePtr extract_impl(parsers::DynExpr<DefaultCtx> expr) { return expr.impl(); }

    // Decode one character from src at position i, advancing i.
    // Handles escape sequences: \n \r \t \' \" \\ \[ \]
    static char decode_char(const std::string& src, std::size_t& i)
    {
        if (src[i] == '\\' && i + 1 < src.size()) {
            ++i;
            char c = src[i];
            ++i;
            switch (c) {
            case 'n':
                return '\n';
            case 'r':
                return '\r';
            case 't':
                return '\t';
            default:
                return c; // ', ", \, [, ]
            }
        }
        return src[i++];
    }
};

} // namespace peg
