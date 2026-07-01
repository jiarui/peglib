// ScopeGuard, ParsingExprInterface / ParsingExpr CRTP base, and the
// symbolConsumable terminal-matching helpers shared by all expression types.
#pragma once
#include <array>
#include <cassert>
#include <concepts>
#include <memory>
#include <set>
#include <string>

#include "Context.h"

namespace peg
{
namespace parsers
{

// RAII cleanup for parser internals (cut stack management, lexeme save/restore).
template<typename FuncType>
struct ScopeGuard
{
    ScopeGuard(FuncType f) : m_cleanup{std::move(f)} {}
    ~ScopeGuard() { m_cleanup(); }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

protected:
    FuncType m_cleanup;
};
template<typename FuncType>
ScopeGuard(FuncType) -> ScopeGuard<FuncType>;

// Abstract base for all parsing expressions.
template<typename Context>
struct ParsingExprInterface
{
    friend NonTerminal<Context>;
    using ElementType = typename Context::value_type;
    using ParseResult = typename Context::ParseResult;
    virtual ~ParsingExprInterface() = default;
    virtual ParseResult parse(Context& context) const = 0;

    // Collect names of rules directly referenced by this expression. Default
    // is no-op (leaves); container types and rule-reference types override.
    virtual void collect_rule_refs(std::set<std::string>&) const {}
};

// CRTP base for every parsing expression type. Carries the derived-type tag
// and shared typedefs. Holds no semantic-action storage — value computation
// lives on NonTerminal (ResultType.h), side-effects via on_match (NonTerminal.h).
template<typename Context, typename ExprType>
struct ParsingExpr : ParsingExprInterface<Context>
{
    using ParseExprType = ExprType;
    using context_type = Context;
    using NodeType = typename Context::node_type;
    using ParseResult = typename Context::ParseResult;
    using ParseTreeNodePtr = typename Context::ParseTreeNodePtr;
    ParsingExpr() = default;
    ParsingExpr(const ParsingExpr&) = default;
    ParsingExpr(ParsingExpr&&) = default;
    ParsingExpr& operator=(const ParsingExpr&) = default;
    ParsingExpr& operator=(ParsingExpr&&) = default;
};

// Terminal-matching predicates.
template<typename elem>
bool symbolConsumable(const elem& v, const elem& value)
{
    return v == value;
}

template<typename elem>
bool symbolConsumable(const elem& v, const std::set<elem>& values)
{
    return values.find(v) != values.end();
}

template<typename elem>
bool symbolConsumable(const elem& v, const std::array<elem, 2>& values)
{
    assert(values[0] <= values[1] && "symbolConsumable: range min > max");
    return (v >= values[0]) && (v <= values[1]);
}

template<typename elem, typename Functor>
    requires std::predicate<Functor, elem>
bool symbolConsumable(const elem& v, const Functor& f)
{
    return f(v);
}

// Record the diagnostic for a failed terminal/token match. Shared by
// TerminalExpr and TokenExpr. Shapes handled in order of specificity:
// single value → 'x'; array-of-2 → 'lo'..'hi'; iterable → comma-joined;
// else → fallback.
//
// The escaped display string is built LAZILY: record_failure_lazy invokes the
// producer only when `pos` is furthest-or-tied (i.e. the item would actually
// be retained). Under ordered-choice backtracking, terminal/token failures
// fire at every non-matching branch but only the furthest-position ones are
// kept — so the eager string construction (escape_char_for_expected + snprintf)
// that the old code did unconditionally was pure waste on every discarded
// failure, which the profile showed as ~7% of instruction refs.
template<typename Context, typename V>
void record_terminal_expected(Context& context, const V& value, std::string_view fallback)
{
    std::size_t pos = context.mark();
    if constexpr (std::is_same_v<V, typename Context::value_type>) {
        context.record_failure_lazy(pos, [&]() {
            return ExpectedItem{.kind = ExpectedKind::Literal,
                                .text = escape_char_for_expected(value)};
        });
    } else if constexpr (requires {
                             std::get<0>(value);
                             std::get<1>(value);
                         }) {
        auto lo = std::get<0>(value);
        auto hi = std::get<1>(value);
        context.record_failure_lazy(pos, [&]() {
            std::string text = escape_char_for_expected(lo) + ".." + escape_char_for_expected(hi);
            return ExpectedItem{.kind = ExpectedKind::Range, .text = std::move(text)};
        });
    } else if constexpr (requires {
                             value.begin();
                             value.end();
                         }) {
        context.record_failure_lazy(pos, [&]() {
            std::string text;
            bool first = true;
            for (const auto& v : value) {
                if (!first)
                    text += ", ";
                first = false;
                text += escape_char_for_expected(v);
            }
            return ExpectedItem{.kind = ExpectedKind::Range, .text = std::move(text)};
        });
    } else {
        context.record_failure_lazy(pos, [&]() {
            return ExpectedItem{.kind = ExpectedKind::Literal, .text = std::string{fallback}};
        });
    }
}

} // namespace parsers
} // namespace peg
