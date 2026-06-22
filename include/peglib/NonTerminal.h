#pragma once
#include "peglib/ParserFwd.h"

#include <cassert>
#include <memory>
#include <string>

namespace peg
{
namespace parsers
{

// ---------------------------------------------------------------------------
// NonTerminal: internal grammar-tree node with stable identity.
//
// Supports packrat memoization and left-recursion (seed-grow algorithm).
// NonTerminal is non-copyable — identity (the `this` pointer) is the memo
// key and seed-grow anchor. Users interact via Rule (a bare-pointer,
// non-owning handle), never directly with NonTerminal.
//
// Post-parse action model (Phase 2 refactor):
//   parse() returns ParseResult { success, tree }. On the first successful
//   match at a given position, a ParseTreeNode is built from the body's
//   result tree, the semantic action is invoked (receiving the node so it
//   can read children->value), and the complete ParseResult is cached in
//   RuleState::m_cached_result. Subsequent memo hits return the cached
//   result directly — no action re-execution, no value stack, no conflict.
//
// Tree retention: the tree node is always kept (even if the action returns
//   a null value). Parent actions skip children whose ->value is null.
// ---------------------------------------------------------------------------
template<typename Context>
struct NonTerminal : ParsingExpr<Context, NonTerminal<Context>>
{
public:
    using ParseResult = typename ParsingExpr<Context, NonTerminal<Context>>::ParseResult;
    using ParseTreeNodePtr = typename ParsingExpr<Context, NonTerminal<Context>>::ParseTreeNodePtr;
    using NodeType = typename Context::node_type;

    NonTerminal() = default;

    template<typename ExprType>
    NonTerminal(const ParsingExpr<Context, ExprType>& rhs)
        : m_rule(std::make_shared<ExprType>(static_cast<const ExprType&>(rhs)))
    {}

    NonTerminal(const NonTerminal&) = delete;
    NonTerminal& operator=(const NonTerminal&) = delete;

    template<typename ExprType>
    NonTerminal& operator=(const ParsingExpr<Context, ExprType>& rhs)
    {
        m_rule = std::make_shared<ExprType>(static_cast<const ExprType&>(rhs));
        return *this;
    }

    void set_name(std::string name) { m_name = std::move(name); }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    void set_label(std::string label) { m_label = std::move(label); }
    [[nodiscard]] const std::string& label() const noexcept { return m_label; }

    [[nodiscard]] bool is_defined() const noexcept { return m_rule != nullptr; }

    // Debug-only lifetime aid: ~Grammar() calls this on every NonTerminal
    // before releasing its owning shared_ptr. Clearing the body makes any
    // dangling Rule handle (one that outlived its Grammar) trip the
    // `assert(m_rule && ...)` in parseImpl instead of silently calling into
    // freed memory. No-op in release builds (NDEBUG defined).
    void clear_body_for_debug() noexcept { m_rule.reset(); }

    ParseResult parse(Context& context) const override
    {
        auto start_pos = context.mark();
        auto [ok, rule_state] = context.rule_state(this, start_pos);

        if (!ok) {
            // Memo hit: return the cached result (tree + action value).
            context.reset(rule_state.m_last_pos);
            return rule_state.m_cached_result;
        }

        // First-time parse: seed-grow loop to find the longest match.
        auto inner = parseImpl(context, start_pos, rule_state);

        if (!inner.success) {
            if (!m_label.empty()) {
                context.record_failure(
                    start_pos, ExpectedItem{.kind = ExpectedKind::RuleLabel, .text = m_label});
            } else if (!m_name.empty()) {
                context.record_failure(
                    start_pos, ExpectedItem{.kind = ExpectedKind::RuleName, .text = m_name});
            }
            ParseResult fail{false, nullptr};
            rule_state.m_cached_result = fail;
            context.update_rule_state(this, start_pos, rule_state);
            return fail;
        }

        // Use the body's tree directly as this rule's tree. If the body
        // produced no tree (e.g. only predicates/terminals), create a fresh
        // node. The action can read body sub-results from node->children.
        auto node = inner.tree;
        if (!node) {
            node = std::make_shared<typename Context::ParseTreeNode>();
        }
        node->name = m_name;
        node->start_offset = start_pos;
        node->end_offset = context.mark();

        // Execute semantic action (if any). The action receives the node
        // and can read node->children[i]->value to access sub-rule results.
        ParseResult result{true, node};
        if (this->m_action) {
            node->value = this->m_action(context, node);
        }
        // Note: even if the action returns null (transparent rule), we keep
        // the tree node. This lets parent actions reliably access children
        // by position. Parents that build user-facing AST skip children
        // whose ->value is null.

        rule_state.m_cached_result = result;
        context.update_rule_state(this, start_pos, rule_state);
        return result;
    }

    void collect_rule_refs(std::set<std::string>& refs) const override
    {
        if (m_rule)
            m_rule->collect_rule_refs(refs);
    }

protected:
    ParseResult parseImpl(Context& context,
                          std::size_t start_pos,
                          typename Context::RuleState& rule_state) const
    {
        assert(m_rule && "NonTerminal::parse called on an unassigned rule");
        auto current_pos = context.mark();
        context.update_rule_state(this, start_pos, rule_state);

        ParseResult best{false, nullptr};

        while (true) {
            context.reset(current_pos);
            auto result = m_rule->parse(context);
            auto end_pos = context.mark();
            if (result.success) {
                if (end_pos > rule_state.m_last_pos) {
                    rule_state.m_last_pos = end_pos;
                    best = result;
                    // Cache intermediate result so recursive memo hits
                    // during seed-grow see the latest successful match.
                    rule_state.m_cached_result = result;
                    if (!context.update_rule_state(this, start_pos, rule_state)) {
                        break;
                    }
                } else {
                    best = std::move(result);
                    rule_state.m_cached_result = best;
                    context.update_rule_state(this, start_pos, rule_state);
                    break;
                }
            } else {
                break;
            }
        }
        context.reset(rule_state.m_last_pos);
        return best;
    }

protected:
    std::shared_ptr<ParsingExprInterface<Context>> m_rule;
    std::string m_name;
    std::string m_label;
};

// ---------------------------------------------------------------------------
// Rule: non-owning handle to a NonTerminal, returned by Grammar::operator[].
//
// Stores a bare NonTerminal* (Grammar is the sole owner via shared_ptr) plus
// a copied rule name (std::string). Copy is shallow — multiple Rule copies
// can point to the same NonTerminal. Expression trees (SequenceExpr,
// DynSequenceExpr, etc.) store Rule copies by value (~40 bytes on libstdc++:
// 8-byte pointer + SSO string).
//
// **Design constraint**: a Rule cannot outlive its Grammar. This is
// intentional — it eliminates shared_ptr cycles at the source (recursive
// grammars no longer form reference cycles, so ~Grammar needs no special
// handling).
//
// **Lifetime of handles**:
//   - After `~Grammar()`, any outstanding Rule handle dangles. In debug
//     builds `~Grammar()` poisons each NonTerminal's body, so a dangling
//     Rule's next parse() hits `assert(m_rule && ...)` in parseImpl; under
//     ASan the same misuse is caught as a use-after-free.
//   - After `Grammar` move (`Grammar g2 = std::move(g1)`), Rule handles
//     obtained from `g1` *remain valid*: `shared_ptr` move leaves the source
//     map empty but the owned NonTerminal objects stay alive (re-homed in
//     `g2`) at the same addresses, so the bare pointers in existing Rule
//     handles still resolve correctly.
//
// Two assignment semantics:
//   - operator=(ParsingExpr<...>) : assign a body to the underlying
//     NonTerminal (auto-names from m_name).
//   - operator=(Rule rhs)         : alias — make this rule's body delegate to
//     rhs's NonTerminal (g["A"] = g["B"] makes A an alias for B).
// ---------------------------------------------------------------------------
template<typename Context>
struct Rule : ParsingExpr<Context, Rule<Context>>
{
    using Impl = NonTerminal<Context>;
    using ParseResult = typename ParsingExpr<Context, Rule<Context>>::ParseResult;
    using SemanticAction = typename ParsingExpr<Context, Rule<Context>>::SemanticAction;

    Rule(Impl* impl, std::string name) : m_impl(impl), m_name(std::move(name)) {}

    Rule(const Rule&) = default;
    Rule(Rule&&) = default;
    // NOTE: copy/move *assignment* have alias semantics (see below), not
    // the usual shallow rebind. This is because Rule handles are views
    // returned by Grammar::operator[]; assigning one view to another
    // mutates the underlying NonTerminal, not the view itself. Expression
    // trees only copy-construct Rule (via std::make_tuple), never
    // copy-assign, so this is safe.

    // Assign a body expression to the underlying NonTerminal. Auto-names
    // the rule from m_name.
    template<typename ExprType>
        requires(!std::same_as<std::remove_cvref_t<ExprType>, Rule<Context>>)
    Rule& operator=(const ParsingExpr<Context, ExprType>& rhs)
    {
        *m_impl = rhs;
        m_impl->set_name(m_name);
        return *this;
    }

    // Alias assignment: make this rule's body delegate to rhs's NonTerminal.
    // The body becomes a Rule copy pointing at rhs's NonTerminal. This is
    // **lazy**: if rhs's body is later reassigned, parsing this rule sees
    // the update (because parsing delegates to rhs's NonTerminal, not a
    // snapshot of its body).
    // Example: g["A"] = g["B"];  // A delegates to B
    Rule& operator=(const Rule& rhs)
    {
        *m_impl = rhs; // NonTerminal template operator= stores a Rule body
        m_impl->set_name(m_name);
        return *this;
    }
    // Not noexcept: NonTerminal::template operator= does make_shared (can
    // throw bad_alloc), and set_name does a std::string assign.
    Rule& operator=(Rule&& rhs)
    {
        *m_impl = rhs;
        m_impl->set_name(m_name);
        return *this;
    }

    Rule& set_action(SemanticAction action)
    {
        m_impl->set_action(std::move(action));
        return *this;
    }

    Rule& set_label(std::string label)
    {
        m_impl->set_label(std::move(label));
        return *this;
    }

    ParseResult parse(Context& context) const override { return m_impl->parse(context); }

    void collect_rule_refs(std::set<std::string>& refs) const override { refs.insert(m_name); }

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] const std::string& label() const noexcept { return m_impl->label(); }
    [[nodiscard]] bool is_defined() const noexcept { return m_impl->is_defined(); }

    // Direct access to the underlying NonTerminal (for Grammar internals and
    // GrammarCompiler). Non-owning.
    [[nodiscard]] const Impl* impl() const noexcept { return m_impl; }
    [[nodiscard]] Impl* impl() noexcept { return m_impl; }

protected:
    Impl* m_impl;
    std::string m_name;
};

} // namespace parsers
} // namespace peg
