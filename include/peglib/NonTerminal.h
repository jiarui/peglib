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

template<typename Context>
struct NonTerminalRef;

// ---------------------------------------------------------------------------
// NonTerminal: a named, memoizable rule. Supports packrat memoization
// and left-recursion (seed-grow algorithm).
//
// Error reporting:
//   set_name("foo")    — records "foo" as an expected item on failure.
//   set_label("a foo") — records "a foo" instead (takes priority over name).
// ---------------------------------------------------------------------------
template<typename Context>
struct NonTerminal : ParsingExpr<Context, NonTerminal<Context>>
{
public:
    // Default construction leaves the rule unassigned. Useful for
    // forward-declared mutually-recursive rules; calling parse() on
    // an unassigned NonTerminal is undefined (asserted).
    NonTerminal() = default;

    template<typename ExprType>
    NonTerminal(const ParsingExpr<Context, ExprType>& rhs)
        : m_rule(std::make_shared<ExprType>(static_cast<const ExprType&>(rhs)))
    {}

    template<typename ExprType>
    NonTerminal& operator=(const ParsingExpr<Context, ExprType>& rhs)
    {
        m_rule = std::make_shared<ExprType>(static_cast<const ExprType&>(rhs));
        return *this;
    }

    // Set the rule name (used for error reporting). Usually called via
    // the PEG_RULE macro, which expands to `rule.set_name(#rule)`.
    void set_name(std::string name) { m_name = std::move(name); }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    // Set a human-readable label (takes priority over name in error messages).
    // Use when the rule name is not user-friendly (e.g. an auto-generated name).
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
                // Brace-init works for both match_range instantiations:
                //   std::span<const T>  {ptr, ptr}      (contiguous source)
                //   std::pair<It, It>   {it, it}        (FileSource)
                auto node = ParsingExpr<Context, NonTerminal<Context>>::m_action(
                    context, typename Context::match_range{start_pos, end_pos});
                context.push_node(std::move(node));
            }
            if (!result) {
                // Record expected item for error reporting: prefer label
                // over name. If neither is set, record nothing (leaf-level
                // terminals below will provide more specific expectations).
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
                        // cut operator may erase ruleState records
                        return res;
                    }
                } else {
                    ruleState.m_last_return = res;
                    // Seed matched but did not advance (e.g. zero-width match
                    // like `*ws`). We must still persist the result to the
                    // memo map, otherwise a second lookup at the same position
                    // returns the stale initial state {pos, false}.
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
// NonTerminalRef: wraps a reference to a NonTerminal for use in expressions.
// ---------------------------------------------------------------------------
template<typename Context>
struct NonTerminalRef : ParsingExpr<Context, NonTerminalRef<Context>>
{
    NonTerminalRef(const NonTerminal<Context>& rhs) : m_nonterm{rhs} {}
    bool parse(Context& context) const override { return m_nonterm.parse(context); }

protected:
    const NonTerminal<Context>& m_nonterm;
};

} // namespace parsers
} // namespace peg
