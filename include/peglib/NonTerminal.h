#pragma once
#include "peglib/ParserFwd.h"

#include <cassert>
#include <memory>
#include <string>
#include <tuple>

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
// Error reporting:
//   set_name("foo")    — records "foo" as an expected item on failure.
//   set_label("a foo") — records "a foo" instead (takes priority over name).
// ---------------------------------------------------------------------------
template<typename Context>
struct NonTerminal : ParsingExpr<Context, NonTerminal<Context>>
{
public:
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

    bool operator()(Context& context) const { return parse(context); }

    bool parse(Context& context) const override
    {
        auto start_pos = context.mark();
        std::tuple<bool, typename Context::RuleState> rs = context.rule_state(this, start_pos);
        typename Context::RuleState rule_state = std::get<1>(rs);
        bool result = false;
        if (!std::get<0>(rs)) {
            context.reset(rule_state.m_last_pos);
            result = rule_state.m_last_return;
            return result;
        } else {
            result = parseImpl(context, start_pos, rule_state);
            if (result && ParsingExpr<Context, NonTerminal<Context>>::m_action) {
                auto end_pos = context.mark();
                auto node = ParsingExpr<Context, NonTerminal<Context>>::m_action(
                    context, typename Context::match_range{start_pos, end_pos});
                context.push_node(std::move(node));
            }
            if (!result) {
                if (!m_label.empty()) {
                    context.record_failure(
                        start_pos, ExpectedItem{.kind = ExpectedKind::RuleLabel, .text = m_label});
                } else if (!m_name.empty()) {
                    context.record_failure(
                        start_pos, ExpectedItem{.kind = ExpectedKind::RuleName, .text = m_name});
                }
            }
            return result;
        }
    }

protected:
    bool parseImpl(Context& context,
                   typename Context::iterator start_pos,
                   typename Context::RuleState& rule_state) const
    {
        assert(m_rule && "NonTerminal::parse called on an unassigned rule");
        auto current_pos = context.mark();
        context.update_rule_state(this, start_pos, rule_state);
        while (true) {
            context.reset(current_pos);
            bool res = m_rule->parse(context);
            auto end_pos = context.mark();
            if (res) {
                if (end_pos > rule_state.m_last_pos) {
                    rule_state.m_last_pos = end_pos;
                    rule_state.m_last_return = res;
                    bool update_res = context.update_rule_state(this, start_pos, rule_state);
                    if (!update_res) {
                        return res;
                    }
                } else {
                    rule_state.m_last_return = res;
                    context.update_rule_state(this, start_pos, rule_state);
                    break;
                }
            } else {
                break;
            }
        }
        bool result = rule_state.m_last_return;
        context.reset(rule_state.m_last_pos);
        return rule_state.m_last_return;
    }

protected:
    std::shared_ptr<ParsingExprInterface<Context>> m_rule;
    std::string m_name;
    std::string m_label;
};

// ---------------------------------------------------------------------------
// Rule: user-facing handle wrapping shared_ptr<NonTerminal>.
//
// Copy is shallow (shared ownership) — multiple Rule objects can point to the
// same NonTerminal identity. This is essential for:
//   - Memo key stability (m_mem[pos][const NonTerminal*])
//   - Left-recursion seed identity (seed-grow writes to a specific NonTerminal)
//   - Semantic action sharing (m_action lives on the NonTerminal entity)
//
// Reference cycles: self-referential rules (e.g. r = r >> 'b' | 'a') create a
// shared_ptr cycle (Rule → NonTerminal → expression tree → Rule → NonTerminal).
// This is intentional — grammar rules are long-lived and the "leak" is bounded
// by the grammar size. Local non-self-referential Rule variables are reclaimed
// normally when the last handle goes out of scope.
// ---------------------------------------------------------------------------
template<typename Context>
struct Rule : ParsingExpr<Context, Rule<Context>>
{
public:
    using Impl = NonTerminal<Context>;
    using SemanticAction = typename ParsingExpr<Context, Rule<Context>>::SemanticAction;

    Rule() : m_impl(std::make_shared<Impl>()) {}

    template<typename ExprType>
    Rule(const ParsingExpr<Context, ExprType>& rhs)
        : m_impl(std::make_shared<Impl>(rhs))
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

    bool operator()(Context& context) const { return parse(context); }

    bool parse(Context& context) const override { return m_impl->parse(context); }

protected:
    std::shared_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------
// RuleProxy: transient handle returned by Grammar::operator[].
//
// Carries the rule name (for auto-naming on assignment). Copy is shallow —
// the underlying Rule (shared_ptr<NonTerminal>) is shared. Expression trees
// store RuleProxy copies (~40 bytes per node: Rule + name string). This is
// acceptable for typical grammars.
// ---------------------------------------------------------------------------
template<typename Context>
struct RuleProxy : ParsingExpr<Context, RuleProxy<Context>>
{
    using Impl = Rule<Context>;
    using SemanticAction = typename ParsingExpr<Context, RuleProxy<Context>>::SemanticAction;

    RuleProxy(Impl rule, std::string name)
        : m_rule(std::move(rule)), m_name(std::move(name))
    {}

    RuleProxy(const RuleProxy&) = default;
    RuleProxy(RuleProxy&&) = default;
    // No copy/move assignment operators — all assignment goes through the
    // operator= overloads below, which forward to the underlying NonTerminal.
    // A defaulted copy/move assignment would just copy/move members without
    // updating the NonTerminal, breaking g["y"] = g["z"].

    // Assignment from any ParsingExpr (including RuleProxy from another rule).
    // This modifies the underlying NonTerminal in-place and auto-names it.
    // Note: when rhs is a RuleProxy, the defaulted copy assignment would
    // normally win (better match). We constrain this template to exclude
    // RuleProxy itself, then provide a separate RuleProxy overload below.
    template<typename ExprType>
        requires (!std::same_as<std::remove_cvref_t<ExprType>, RuleProxy<Context>>)
    RuleProxy& operator=(const ParsingExpr<Context, ExprType>& rhs)
    {
        m_rule = rhs;
        m_rule.set_name(m_name);
        return *this;
    }

    // RuleProxy-to-RuleProxy assignment: treat as forwarding to the underlying
    // NonTerminal. This makes g["y"] = g["z"] define y's NonTerminal to
    // reference z's expression tree. Handles both lvalue and rvalue RHS.
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

    bool operator()(Context& context) const { return parse(context); }

    bool parse(Context& context) const override { return m_rule.parse(context); }

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    const Impl& rule() const noexcept { return m_rule; }
    Impl& rule() noexcept { return m_rule; }

protected:
    Impl m_rule;
    std::string m_name;
};

} // namespace parsers
} // namespace peg
