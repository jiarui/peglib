#pragma once
#include <array>
#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <set>

#include "Context.h"

namespace peg
{
namespace parsers
{

// ---------------------------------------------------------------------------
// ScopeGuard: RAII cleanup for parser internals (cut stack management, etc.)
// ---------------------------------------------------------------------------
struct ScopeGuard
{
    template<typename FuncType>
    ScopeGuard(FuncType f) : m_cleanup{f}
    {}
    ~ScopeGuard() { m_cleanup(); }

protected:
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    std::function<void()> m_cleanup;
};

// ---------------------------------------------------------------------------
// ParsingExprInterface: abstract base for all parsing expressions.
// ---------------------------------------------------------------------------
template<typename Context>
struct ParsingExprInterface
{
    friend NonTerminal<Context>;
    using ElementType = typename Context::value_type;
    virtual ~ParsingExprInterface() = default;
    virtual bool parse(Context& context) const = 0;
};

// ---------------------------------------------------------------------------
// ParsingExpr: CRTP base providing semantic-action storage.
// NodeType is derived from the Context (defaults to std::monostate).
// ---------------------------------------------------------------------------
template<typename Context, typename ExprType>
struct ParsingExpr : ParsingExprInterface<Context>
{
    using ParseExprType = ExprType;
    using NodeType = typename Context::node_type;
    using SemanticAction =
        std::function<NodeType(Context&, typename Context::match_range match_range)>;
    void setAction(SemanticAction action) { m_action = action; }
    ParsingExpr() = default;
    ParsingExpr(SemanticAction action) : m_action(action) {}
    ParsingExpr(const ParsingExpr&) = default;
    ParsingExpr(ParsingExpr&&) = default;
    ParsingExpr& operator=(const ParsingExpr&) = default;
    ParsingExpr& operator=(ParsingExpr&&) = default;

protected:
    SemanticAction m_action;
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

} // namespace parsers
} // namespace peg
