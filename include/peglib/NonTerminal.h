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
// key and seed-grow anchor. Users interact via Rule (a shared_ptr handle),
// never directly with NonTerminal.
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
            rule_state.m_last_return = false;
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
        node->start_offset = context.offset_of(start_pos);
        node->end_offset = context.offset_of(context.mark());

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

protected:
    ParseResult parseImpl(Context& context,
                          typename Context::iterator start_pos,
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
                    rule_state.m_last_return = true;
                    best = result;
                    // Cache intermediate result so recursive memo hits
                    // during seed-grow see the latest successful match.
                    rule_state.m_cached_result = result;
                    if (!context.update_rule_state(this, start_pos, rule_state)) {
                        break;
                    }
                } else {
                    rule_state.m_last_return = true;
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
// Rule: user-facing handle wrapping shared_ptr<NonTerminal>.
//
// Copy is shallow (shared ownership) — multiple Rule objects can point to
// the same NonTerminal identity. This is essential for:
//   - Memo key stability (m_mem[pos][const NonTerminal*])
//   - Left-recursion seed identity (seed-grow writes to a specific NonTerminal)
//   - Semantic action sharing (m_action lives on the NonTerminal entity)
// ---------------------------------------------------------------------------
template<typename Context>
struct Rule : ParsingExpr<Context, Rule<Context>>
{
public:
    using Impl = NonTerminal<Context>;
    using ParseResult = typename ParsingExpr<Context, Rule<Context>>::ParseResult;
    using SemanticAction = typename ParsingExpr<Context, Rule<Context>>::SemanticAction;

    Rule() : m_impl(std::make_shared<Impl>()) {}

    template<typename ExprType>
    Rule(const ParsingExpr<Context, ExprType>& rhs) : m_impl(std::make_shared<Impl>(rhs))
    {}

    Rule(const Rule&) = default;
    Rule& operator=(const Rule&) = default;

    template<typename ExprType>
    Rule& operator=(const ParsingExpr<Context, ExprType>& rhs)
    {
        *m_impl = rhs;
        return *this;
    }

    void set_name(std::string name) { m_impl->set_name(std::move(name)); }
    [[nodiscard]] const std::string& name() const noexcept { return m_impl->name(); }

    void set_label(std::string label) { m_impl->set_label(std::move(label)); }
    [[nodiscard]] const std::string& label() const noexcept { return m_impl->label(); }

    [[nodiscard]] bool is_defined() const noexcept { return m_impl->is_defined(); }

    void set_action(SemanticAction action) { m_impl->set_action(std::move(action)); }

    ParseResult parse(Context& context) const override { return m_impl->parse(context); }

protected:
    std::shared_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------
// RuleProxy: transient handle returned by Grammar::operator[].
//
// Carries the rule name (for auto-naming on assignment). Copy is shallow —
// the underlying Rule (shared_ptr<NonTerminal>) is shared. Expression trees
// store RuleProxy copies (~40 bytes per node: Rule + name string).
// ---------------------------------------------------------------------------
template<typename Context>
struct RuleProxy : ParsingExpr<Context, RuleProxy<Context>>
{
    using Impl = Rule<Context>;
    using ParseResult = typename ParsingExpr<Context, RuleProxy<Context>>::ParseResult;
    using SemanticAction = typename ParsingExpr<Context, RuleProxy<Context>>::SemanticAction;

    RuleProxy(Impl rule, std::string name) : m_rule(std::move(rule)), m_name(std::move(name)) {}

    RuleProxy(const RuleProxy&) = default;
    RuleProxy(RuleProxy&&) = default;

    template<typename ExprType>
        requires(!std::same_as<std::remove_cvref_t<ExprType>, RuleProxy<Context>>)
    RuleProxy& operator=(const ParsingExpr<Context, ExprType>& rhs)
    {
        m_rule = rhs;
        m_rule.set_name(m_name);
        return *this;
    }

    RuleProxy& operator=(RuleProxy rhs)
    {
        m_rule = rhs;
        m_rule.set_name(m_name);
        return *this;
    }

    RuleProxy& set_action(SemanticAction action)
    {
        m_rule.set_action(std::move(action));
        return *this;
    }

    RuleProxy& set_label(std::string label)
    {
        m_rule.set_label(std::move(label));
        return *this;
    }

    ParseResult parse(Context& context) const override { return m_rule.parse(context); }

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    const Impl& rule() const noexcept { return m_rule; }
    Impl& rule() noexcept { return m_rule; }

protected:
    Impl m_rule;
    std::string m_name;
};

} // namespace parsers
} // namespace peg
