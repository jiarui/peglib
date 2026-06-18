#pragma once
#include "peglib/Concepts.h"
#include "peglib/NonTerminal.h"
#include "peglib/Terminals.h"

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

// ---------------------------------------------------------------------------
// Grammar: user-facing container of named rules. Sole owner of all
// NonTerminals (held via shared_ptr).
//
//   Grammar<> g;
//   g["number"] = +terminal('0', '9');
//   g["expr"]   = g["number"] | g["expr"] >> '+' >> g["number"];
//   g.set_start("expr");
//   g.parse_string("1+2+3");  // convenience: creates a Context internally
//
// Rules are lazily created on first access via operator[]. Assignment
// auto-names the rule from the map key. The same Grammar can parse many
// inputs — each parse gets a fresh Context (fresh memo cache, position).
//
// **Design**: Rule (the handle returned by operator[]) stores a bare
// NonTerminal*, not a shared_ptr. This eliminates shared_ptr cycles in
// recursive grammars at the source — ~Grammar() needs no special handling.
// The constraint is that Rule cannot outlive its Grammar (intentional).
// ---------------------------------------------------------------------------
template<PegContext Ctx = Context<std::span<const char>>>
class Grammar
{
public:
    using Context = Ctx;
    using Rule = parsers::Rule<Context>;
    using NonTerminalType = parsers::NonTerminal<Context>;

    Grammar() = default;
    Grammar(const Grammar&) = delete;
    Grammar& operator=(const Grammar&) = delete;
    Grammar(Grammar&&) = default;
    Grammar& operator=(Grammar&&) = default;
    ~Grammar()
    {
#ifndef NDEBUG
        // Debug lifetime aid: poison every NonTerminal's body before the
        // shared_ptr map releases them. If a Rule handle escapes its
        // Grammar's lifetime, its next parse() trips the
        // `assert(m_rule && ...)` in NonTerminal::parseImpl instead of
        // silently using freed memory. (Under ASan, the same misuse is
        // additionally caught as a use-after-free.) No-op in release builds.
        for (auto& [_, nt] : m_rules) {
            nt->clear_body_for_debug();
        }
#endif
    }

    // Access a rule by name. This is **get-or-create**: if the rule does
    // not yet exist it is lazily inserted as a forward declaration (an
    // undefined NonTerminal). Returns a non-owning Rule handle for
    // assignment / chaining / introspection.
    //
    // Caveat: because operator[] inserts on miss, using it for an existence
    // check pollutes undefined_rules() — `if (g["typo"].is_defined())` creates
    // the rule "typo". For read-only existence queries that must NOT insert,
    // use find() or has_rule() instead.
    Rule operator[](std::string name)
    {
        auto [it, inserted] = m_rules.try_emplace(name);
        if (inserted) {
            it->second = std::make_shared<NonTerminalType>();
        }
        return Rule{it->second.get(), it->first};
    }

    // Read-only rule lookup. Returns std::nullopt if the rule does not
    // exist — and, unlike operator[], does NOT insert it. Use this (or
    // has_rule) when you need to inspect a rule's existence/body without
    // side-effecting the grammar.
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

    // Start rule management
    void set_start(std::string name) { m_start = std::move(name); }
    [[nodiscard]] const std::string& start_rule() const noexcept { return m_start; }

    // Parse using the start rule. Returns true on success, false on any
    // failure (including cut-committed failures). On failure, the Context
    // holds the diagnostic — retrieve it with `ctx.take_error()`. This method
    // does not throw for parse failures; it only throws `std::logic_error` if
    // no start rule is set.
    bool parse(Context& ctx) const
    {
        if (m_start.empty()) {
            throw std::logic_error{"Grammar::parse: no start rule set"};
        }
        return parse(m_start, ctx);
    }

    // Parse using an explicit rule name. Returns true on success, false on
    // any failure (regular or cut-committed). Cut-committed failures (which
    // manifest internally as a thrown peg::ParseError from the
    // Alternation/Repetition that owned the cut scope) are caught here and
    // surfaced as a normal failure: the Context's furthest-failure state —
    // already populated by record_failure() calls made before the throw — is
    // queryable via ctx.take_error(). Throws std::out_of_range if `rule`
    // is not defined.
    bool parse(std::string_view rule, Context& ctx) const
    {
        auto it = m_rules.find(std::string{rule});
        if (it == m_rules.end()) {
            throw std::out_of_range{"Grammar::parse: rule '" + std::string{rule} + "' not found"};
        }
        try {
            return it->second->parse(ctx).success;
        } catch (const ParseError&) {
            // The Context's error state was already updated by record_failure
            // calls along the failing path before the cut committed. Nothing
            // to do here but report failure.
            return false;
        }
    }

    // Parse and return the parse tree (nullptr on failure or if the start
    // rule is transparent). Useful for AST construction. Like parse(), this
    // catches cut-committed failures and returns nullptr; retrieve the
    // diagnostic via ctx.take_error(). Throws std::out_of_range if `rule`
    // is not defined.
    typename Context::ParseTreeNodePtr parse_tree(std::string_view rule, Context& ctx) const
    {
        auto it = m_rules.find(std::string{rule});
        if (it == m_rules.end()) {
            throw std::out_of_range{"Grammar::parse_tree: rule '" + std::string{rule} +
                                    "' not found"};
        }
        try {
            return it->second->parse(ctx).tree;
        } catch (const ParseError&) {
            return nullptr;
        }
    }

    // Convenience: parse a string input using the start rule.
    bool parse_string(std::string_view input) const
    {
        std::string s{input};
        Context ctx{s};
        return parse(ctx);
    }

    // -----------------------------------------------------------------------
    // Validation helpers
    // -----------------------------------------------------------------------

    // Returns names of rules that were accessed via operator[] but never
    // assigned a definition (still empty NonTerminal). These would silently
    // fail to match at parse time.
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

    // Returns names of rules that are defined but not reachable from the
    // start rule. These are dead code — they can be removed without
    // changing the grammar's behaviour.
    [[nodiscard]] std::vector<std::string> unreachable_rules() const
    {
        if (m_start.empty())
            return {};
        auto start_it = m_rules.find(m_start);
        if (start_it == m_rules.end())
            return {};

        // Collect all rule names transitively referenced from the start
        // rule's body.
        std::set<std::string> reachable;
        reachable.insert(m_start);

        // DFS: for each reachable rule, collect its body's direct references.
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

        // Unreachable = all defined rules minus reachable.
        std::vector<std::string> result;
        for (const auto& [name, nt] : m_rules) {
            if (nt->is_defined() && reachable.find(name) == reachable.end()) {
                result.push_back(name);
            }
        }
        return result;
    }

protected:
    // Grammar is the sole owner of all NonTerminals. Rule handles (returned
    // by operator[]) hold bare NonTerminal* — no shared_ptr cycle possible.
    std::map<std::string, std::shared_ptr<NonTerminalType>> m_rules;
    std::string m_start;
};

} // namespace peg
