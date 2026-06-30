// Grammar: user-facing container of named rules. Sole owner of all
// NonTerminals (held via shared_ptr).
//
//   Grammar<> g;
//   g["number"] = +g.terminal('0', '9');
//   g["expr"]   = g["number"] | g["expr"] >> '+' >> g["number"];
//   g.set_start("expr");
//   g.parse_string("1+2+3");  // convenience: creates a Context internally
//
// Rules are lazily created on first access via operator[]. The same Grammar
// can parse many inputs — each parse gets a fresh Context.
//
// **Lifetime constraint**: Rule (the handle returned by operator[]) stores a
// bare NonTerminal*, not a shared_ptr. This eliminates shared_ptr cycles in
// recursive grammars at the source. A Rule cannot outlive its Grammar.
#pragma once
#include "peglib/Combinators.h"
#include "peglib/Concepts.h"
#include "peglib/NonTerminal.h"
#include "peglib/Terminals.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace peg
{

template<typename CharT = char, typename NodeType = std::monostate>
    requires PegContext<Context<CharT, NodeType>>
class Grammar
{
public:
    using Context = peg::Context<CharT, NodeType>;
    using Rule = parsers::Rule<Context>;
    using NonTerminalType = parsers::NonTerminal<Context>;

    // -----------------------------------------------------------------------
    // Expression factories. These close over this Grammar's Context type, so
    // every expression they build carries the correct NodeType and composes
    // freely with g[...] rule handles.
    // -----------------------------------------------------------------------

private:
    // Shared overload set for terminal(...) and token(...). ExprT is either
    // TerminalExpr (void result) or TokenExpr (value_type result).
    template<template<typename, typename> class ExprT, typename Pred>
        requires std::predicate<Pred, CharT>
    auto make_matcher(const Pred& f) const
    {
        return ExprT<Context, Pred>(f);
    }
    template<template<typename, typename> class ExprT, typename V>
        requires PegValue<V> && std::convertible_to<V, CharT>
    auto make_matcher(V value) const
    {
        return ExprT<Context, CharT>(static_cast<CharT>(value));
    }
    template<template<typename, typename> class ExprT>
    auto make_matcher(const std::set<CharT>& values) const
    {
        return ExprT<Context, std::set<CharT>>(values);
    }
    template<template<typename, typename> class ExprT>
    auto make_matcher(const std::array<CharT, 2>& values) const
    {
        return ExprT<Context, std::array<CharT, 2>>(values);
    }

public:
    // terminal(...): void-result matcher (filtered from sequence results) —
    // for structural tokens (parens, keywords) that must never reach actions.
    template<typename T>
    auto terminal(T&& v) const -> decltype(make_matcher<parsers::TerminalExpr>(std::forward<T>(v)))
    {
        return make_matcher<parsers::TerminalExpr>(std::forward<T>(v));
    }
    auto terminal(const CharT& value_min, const CharT& value_max) const
    {
        return terminal(std::array<CharT, 2>{value_min, value_max});
    }

    // token(...): value_type-result matcher (kept; surfaced to typed actions)
    // — for tokens whose identity the action needs.
    template<typename T>
    auto token(T&& v) const -> decltype(make_matcher<parsers::TokenExpr>(std::forward<T>(v)))
    {
        return make_matcher<parsers::TokenExpr>(std::forward<T>(v));
    }
    auto token(const CharT& value_min, const CharT& value_max) const
    {
        return token(std::array<CharT, 2>{value_min, value_max});
    }

    // terminalSeq(range): match a contiguous run of elements.
    template<typename SeqType>
        requires PegValueSeq<SeqType> && std::same_as<typename SeqType::value_type, CharT>
    auto terminalSeq(const SeqType& valueSeq) const
    {
        return parsers::TerminalSeqExpr<Context, SeqType>(valueSeq);
    }

    // terminalSeq(literal): well-formed when CharT has a char_traits
    // specialisation; for token-level grammars use the range overload above.
    auto terminalSeq(const CharT* str) const
    {
        return parsers::TerminalSeqExpr<Context, std::basic_string<CharT>>(
            std::basic_string<CharT>{str});
    }

    auto empty() const { return parsers::EmptyExpr<Context>(); }

    // Commit the current alternative/repetition scope.
    auto cut() const { return parsers::CutExpr<Context>(); }

    // Disable auto-skip within `expr`'s subtree. Token bodies (numbers,
    // identifiers) whose characters must stay contiguous are wrapped in
    // lexeme(...).
    template<typename Expr>
        requires requires { typename std::remove_cvref_t<Expr>::context_type; } &&
                 std::same_as<typename std::remove_cvref_t<Expr>::context_type, Context>
    auto lexeme(const Expr& expr) const
    {
        return parsers::LexemeExpr<Context, Expr>(expr);
    }

    // Match-time primitive (weakened lpeg.Cmt). `fn` reads the context
    // READ-ONLY and returns the span it consumed, or std::nullopt to reject;
    // MatcherExpr advances the position to span.end. Result is void (a
    // recognizer); observe what it matched via on_match reading the node's
    // span. Use it for matches that depend on runtime information (Lua long
    // brackets, balanced delimiters, indentation-sensitive blocks).
    template<typename Fn>
        requires std::invocable<const Fn&, Context&, Span>
    auto matcher(Fn fn) const
    {
        return parsers::MatcherExpr<Context, Fn>(std::move(fn));
    }

    Grammar() = default;
    Grammar(const Grammar&) = delete;
    Grammar& operator=(const Grammar&) = delete;
    Grammar(Grammar&&) = default;
    Grammar& operator=(Grammar&&) = default;
    ~Grammar()
    {
#ifndef NDEBUG
        // Debug lifetime aid: poison every NonTerminal's body before the
        // shared_ptr map releases them. A dangling Rule handle (one that
        // outlived its Grammar) trips the assert in parseImpl instead of
        // silently using freed memory. Under ASan, the same misuse is caught
        // as use-after-free.
        for (auto& [_, nt] : m_rules) {
            nt->clear_body_for_debug();
        }
#endif
    }

    // Get-or-create rule access. **Caveat**: this inserts on miss — using it
    // for an existence check pollutes undefined_rules(). For read-only
    // existence queries use find() or has_rule().
    Rule operator[](const std::string& name)
    {
        auto [it, inserted] = m_rules.try_emplace(name);
        if (inserted) {
            it->second = std::make_shared<NonTerminalType>();
        }
        return Rule{it->second.get(), it->first};
    }

    [[nodiscard]] std::optional<Rule> find(std::string_view name) const
    {
        auto it = m_rules.find(std::string{name});
        if (it == m_rules.end())
            return std::nullopt;
        return Rule{it->second.get(), it->first};
    }

    [[nodiscard]] bool has_rule(std::string_view name) const
    {
        return m_rules.find(std::string{name}) != m_rules.end();
    }

    [[nodiscard]] std::vector<std::string> rule_names() const
    {
        std::vector<std::string> names;
        names.reserve(m_rules.size());
        for (const auto& [name, _] : m_rules) {
            names.push_back(name);
        }
        return names;
    }

    void set_start(std::string name) { m_start = std::move(name); }
    [[nodiscard]] const std::string& start_rule() const noexcept { return m_start; }

    // -----------------------------------------------------------------------
    // Auto-skip. Set a transparent skipper rule that fires automatically
    //   - between adjacent children of a Sequence (static and Dyn),
    //   - between iterations of a repetition,
    // and nowhere else. Typically:
    //   g["ws"] = *g.terminal([](char c){ return c==' '||c=='\t'||...});
    //   g.set_skipper(g["ws"]);
    // After this, sequences no longer need `>> g["ws"] >>` threading. To
    // disable for a sub-expression, wrap it in lexeme(...). To disable
    // globally, clear_skipper() (or never call set_skipper).
    //
    // Argument must be a *defined* rule of this Grammar. Grammar stores a
    // non-owning pointer (the body lives in m_rules for the Grammar's whole
    // lifetime).
    // -----------------------------------------------------------------------
    void set_skipper(Rule r)
    {
        if (!r.is_defined()) {
            throw std::invalid_argument{"Grammar::set_skipper: rule is not defined"};
        }
        m_skipper = r.impl();
    }

    void clear_skipper() noexcept { m_skipper = nullptr; }
    [[nodiscard]] bool has_skipper() const noexcept { return m_skipper != nullptr; }

    // Parse using the start rule. Returns true on success, false on any
    // failure (regular or cut-committed). Cut-committed failures (thrown
    // internally as peg::ParseError from the Alternation/Repetition that owned
    // the cut scope) are caught and surfaced as a normal failure: retrieve
    // the diagnostic via ctx.take_error(). Throws std::logic_error if no
    // start rule is set; std::out_of_range if `rule` is not defined.
    bool parse(Context& ctx) const
    {
        if (m_start.empty()) {
            throw std::logic_error{"Grammar::parse: no start rule set"};
        }
        return parse(m_start, ctx);
    }

    bool parse(std::string_view rule, Context& ctx) const
    {
        auto it = m_rules.find(std::string{rule});
        if (it == m_rules.end()) {
            throw std::out_of_range{"Grammar::parse: rule '" + std::string{rule} + "' not found"};
        }
        ctx.internal_set_skipper(m_skipper);
        // Pest-style leading whitespace: consume at the grammar boundary so
        // users don't need `g["ws"] >>` prefix. Trailing whitespace is
        // intentionally NOT consumed (partial-match); for full-input
        // consumption append `>> !.` (EndOfFile) to the start rule.
        ctx.run_skipper();
        try {
            return it->second->parse(ctx).success;
        } catch (const ParseError&) {
            return false;
        }
    }

    // Parse and return the tree (nullptr on failure). Pure structure for
    // introspection (offsets, children, names) — no value slot, no hooks fire.
    typename Context::ParseTreeNodePtr parse_tree(std::string_view rule, Context& ctx) const
    {
        auto it = m_rules.find(std::string{rule});
        if (it == m_rules.end()) {
            throw std::out_of_range{"Grammar::parse_tree: rule '" + std::string{rule} +
                                    "' not found"};
        }
        ctx.internal_set_skipper(m_skipper);
        ctx.run_skipper();
        try {
            return it->second->parse(ctx).tree;
        } catch (const ParseError&) {
            return nullptr;
        }
    }

    // Parse and fold the result tree into a typed AST value (the two-phase
    // model). parse_tree() builds the tree (pure structure); fold_start walks
    // it once via the START rule's typed fold, owning child values as locals
    // and moving them up. Unconditionally move-safe — no value is stored at a
    // shared location. Also the ONLY entry point that fires on_match
    // side-effect hooks (once per committed-tree node, in tree order).
    // Returns std::nullopt on parse failure or a null tree.
    std::optional<NodeType> parse_ast(std::string_view rule, Context& ctx) const
    {
        auto it = m_rules.find(std::string{rule});
        if (it == m_rules.end()) {
            throw std::out_of_range{"Grammar::parse_ast: rule '" + std::string{rule} +
                                    "' not found"};
        }
        auto tree = parse_tree(rule, ctx);
        if (!tree)
            return std::nullopt;
        parsers::fire_on_match<Context, typename Context::ParseTreeNodePtr>(ctx, tree, it->second);
        return parsers::fold_start<Context, typename Context::ParseTreeNodePtr>(
            ctx, tree, it->second);
    }

    // Convenience: parse a string input using the start rule. Partial-match
    // semantics: returns true if the start rule matches at the beginning of
    // `input`, EVEN IF input remains unconsumed. To require the whole input
    // be consumed, append `!.` (EndOfFile) to the start rule. Makes its own
    // copy of the input, so temporaries are safe to pass.
    bool parse_string(std::string_view input) const
    {
        std::string s{input};
        Context ctx{s};
        return parse(ctx);
    }

    // -----------------------------------------------------------------------
    // Validation helpers
    // -----------------------------------------------------------------------

    // Rules accessed via operator[] but never assigned (would silently fail).
    [[nodiscard]] std::vector<std::string> undefined_rules() const
    {
        std::vector<std::string> result;
        for (const auto& [name, nt] : m_rules) {
            if (!nt->is_defined()) {
                result.push_back(name);
            }
        }
        return result;
    }

    // Rules defined but not reachable from the start rule (dead code).
    [[nodiscard]] std::vector<std::string> unreachable_rules() const
    {
        if (m_start.empty())
            return {};
        auto start_it = m_rules.find(m_start);
        if (start_it == m_rules.end())
            return {};

        std::set<std::string> reachable;
        reachable.insert(m_start);

        std::vector<std::string> queue{m_start};
        while (!queue.empty()) {
            auto name = queue.back();
            queue.pop_back();
            auto it = m_rules.find(name);
            if (it == m_rules.end())
                continue;

            std::set<std::string> refs;
            it->second->collect_rule_refs(refs);
            for (const auto& ref : refs) {
                if (reachable.insert(ref).second) {
                    queue.push_back(ref);
                }
            }
        }

        std::vector<std::string> result;
        for (const auto& [name, nt] : m_rules) {
            if (nt->is_defined() && reachable.find(name) == reachable.end()) {
                result.push_back(name);
            }
        }
        return result;
    }

    // -----------------------------------------------------------------------
    // to_dot: Graphviz DOT digraph of rule dependencies. Every defined rule
    // is a node (start rule gets peripheries=2); every rule-reference is a
    // directed edge. Undefined references appear as dangling edge targets —
    // useful for spotting typos. Suitable for piping through `dot -Tsvg`:
    //   std::cout << g.to_dot();
    //   // then:  ./my_parser | dot -Tsvg > grammar.svg
    // -----------------------------------------------------------------------
    [[nodiscard]] std::string to_dot() const
    {
        std::string out;
        out.reserve(256);
        out += "digraph peglib_grammar {\n";
        out += "  node [shape=box];\n";
        out += "  rankdir=LR;\n";

        for (const auto& [name, nt] : m_rules) {
            if (!nt->is_defined())
                continue;
            out += "  \"";
            out += dot_escape(name);
            out += "\"";
            if (name == m_start) {
                out += " [peripheries=2]";
            }
            out += ";\n";
        }

        // Visit every defined rule (dead-code branches render too).
        std::set<std::string> visited;
        std::vector<std::string> queue;
        for (const auto& [name, nt] : m_rules) {
            if (nt->is_defined()) {
                queue.push_back(name);
            }
        }
        while (!queue.empty()) {
            auto name = queue.back();
            queue.pop_back();
            if (!visited.insert(name).second)
                continue;
            auto it = m_rules.find(name);
            if (it == m_rules.end())
                continue;

            std::set<std::string> refs;
            it->second->collect_rule_refs(refs);
            for (const auto& ref : refs) {
                out += "  \"";
                out += dot_escape(name);
                out += "\" -> \"";
                out += dot_escape(ref);
                out += "\";\n";
                if (m_rules.count(ref)) {
                    queue.push_back(ref);
                }
            }
        }
        out += "}\n";
        return out;
    }

protected:
    std::map<std::string, std::shared_ptr<NonTerminalType>> m_rules;
    std::string m_start;
    NonTerminalType* m_skipper = nullptr;

    // Escape a rule name for DOT string literal.
    static std::string dot_escape(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += c;
            }
        }
        return out;
    }
};

} // namespace peg
