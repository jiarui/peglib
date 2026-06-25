#pragma once
#include <array>
#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <set>
#include <string>

#include "Context.h"

namespace peg
{
namespace parsers
{

// ---------------------------------------------------------------------------
// ScopeGuard: RAII cleanup for parser internals (cut stack management, etc.)
//
// Templated on the cleanup callable so small captures (e.g. [&context]) are
// stored inline with no std::function heap allocation. CTAD deduces F from
// the constructor argument, so call sites read identically to before:
//   ScopeGuard _{[&context]() { context.remove_cut(); }};
// ---------------------------------------------------------------------------
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
// Deduction guide so ScopeGuard{lambda} deduces ScopeGuard<decltype(lambda)>.
template<typename FuncType>
ScopeGuard(FuncType) -> ScopeGuard<FuncType>;

// ---------------------------------------------------------------------------
// ParsingExprInterface: abstract base for all parsing expressions.
// ---------------------------------------------------------------------------
template<typename Context>
struct ParsingExprInterface
{
    friend NonTerminal<Context>;
    using ElementType = typename Context::value_type;
    using ParseResult = typename Context::ParseResult;
    virtual ~ParsingExprInterface() = default;
    virtual ParseResult parse(Context& context) const = 0;

    // Collect names of rules directly referenced by this expression.
    // Default is no-op (leaves: terminals, empty, cut). Container types
    // and rule-reference types override this. The caller (Grammar) handles
    // transitive closure.
    virtual void collect_rule_refs(std::set<std::string>&) const {}
};

// ---------------------------------------------------------------------------
// ParsingExpr: CRTP base for every parsing expression type.
//
// Carries the derived-type tag (ExprType) and the shared typedefs the
// combining operators and fold driver need. It holds no semantic-action
// storage: value computation lives on NonTerminal as a typed fold
// (ResultType.h), and parse-time side-effects as on_match (NonTerminal.h).
// Keeping action storage off the base avoids a dead std::function on every
// leaf/combinator expression object.
// ---------------------------------------------------------------------------
template<typename Context, typename ExprType>
struct ParsingExpr : ParsingExprInterface<Context>
{
    using ParseExprType = ExprType;
    // Exposed so combining operators and factories (e.g. Grammar::lexeme)
    // can recover the Context type from any ParsingExpr-derived expression.
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

// ---------------------------------------------------------------------------
// symbolConsumable: predicate helpers for terminal matching.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// record_terminal_expected: record the diagnostic for a failed terminal/token
// match. Shared by TerminalExpr and TokenExpr (identical match semantics;
// only the fallback placeholder differs). Shapes handled in order of
// specificity: single value → 'x'; array/pair of 2 → 'lo'..'hi'; iterable
// container → comma-joined literals; else → `fallback`.
// ---------------------------------------------------------------------------
template<typename Context, typename V>
void record_terminal_expected(Context& context, const V& value, std::string_view fallback)
{
    std::size_t pos = context.mark();
    if constexpr (std::is_same_v<V, typename Context::value_type>) {
        context.record_failure(
            pos,
            ExpectedItem{.kind = ExpectedKind::Literal, .text = escape_char_for_expected(value)});
    } else if constexpr (requires {
                             std::get<0>(value);
                             std::get<1>(value);
                         }) {
        // Array-of-2 / pair: range terminal. Produces "'lo'..'hi'". Keep the
        // element types intact so escape_char_for_expected renders them
        // correctly for any value_type (char, char32_t, ...).
        auto lo = std::get<0>(value);
        auto hi = std::get<1>(value);
        std::string text = escape_char_for_expected(lo) + ".." + escape_char_for_expected(hi);
        context.record_failure(pos,
                               ExpectedItem{.kind = ExpectedKind::Range, .text = std::move(text)});
    } else if constexpr (requires {
                             value.begin();
                             value.end();
                         }) {
        // Set-like container: comma-joined literals.
        std::string text;
        bool first = true;
        for (const auto& v : value) {
            if (!first)
                text += ", ";
            first = false;
            text += escape_char_for_expected(v);
        }
        context.record_failure(pos,
                               ExpectedItem{.kind = ExpectedKind::Range, .text = std::move(text)});
    } else {
        context.record_failure(
            pos, ExpectedItem{.kind = ExpectedKind::Literal, .text = std::string{fallback}});
    }
}

} // namespace parsers
} // namespace peg
