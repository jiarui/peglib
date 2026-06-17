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

    bool operator()(Context& context) const { return parse(context); }

    bool parse(Context& context) const override
    {
        auto start_pos = context.mark();
        std::tuple<bool, typename Context::RuleState> rs = context.ruleState(this, start_pos);
        typename Context::RuleState ruleState = std::get<1>(rs);
        bool result = false;
        if (!std::get<0>(rs)) {
            context.reset(ruleState.m_last_pos);
            result = ruleState.m_last_return;
            return result;
        } else {
            result = parseImpl(context, start_pos, ruleState);
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
                   typename Context::RuleState& ruleState) const
    {
        assert(m_rule && "NonTerminal::parse called on an unassigned rule");
        auto current_pos = context.mark();
        context.updateRuleState(this, start_pos, ruleState);
        while (true) {
            context.reset(current_pos);
            bool res = m_rule->parse(context);
            auto end_pos = context.mark();
            if (res) {
                if (end_pos > ruleState.m_last_pos) {
                    ruleState.m_last_pos = end_pos;
                    ruleState.m_last_return = res;
                    bool update_res = context.updateRuleState(this, start_pos, ruleState);
                    if (!update_res) {
                        return res;
                    }
                } else {
                    ruleState.m_last_return = res;
                    context.updateRuleState(this, start_pos, ruleState);
                    break;
                }
            } else {
                break;
            }
        }
        bool result = ruleState.m_last_return;
        context.reset(ruleState.m_last_pos);
        return ruleState.m_last_return;
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

    void setAction(SemanticAction action) { m_impl->setAction(std::move(action)); }

    bool operator()(Context& context) const { return parse(context); }

    bool parse(Context& context) const override { return m_impl->parse(context); }

protected:
    std::shared_ptr<Impl> m_impl;
};

} // namespace parsers
} // namespace peg
