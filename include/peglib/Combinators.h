#pragma once
#include "peglib/ParserFwd.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <tuple>

namespace peg
{
namespace parsers
{

// ---------------------------------------------------------------------------
// SequenceExpr: matches child expressions in order; all must succeed.
// ---------------------------------------------------------------------------
template<typename Context, typename... Children>
struct SequenceExpr : ParsingExpr<Context, SequenceExpr<Context, Children...>>
{
    SequenceExpr(const std::tuple<Children...>& children) : m_children{children} {}

    const std::tuple<Children...>& children() const { return m_children; }
    bool parse(Context& context) const override
    {
        auto state = context.state();
        bool result = parseSeq<0>(context);
        if (!result) {
            context.state(state);
        }
        return result;
    }

protected:
    template<size_t Index>
    bool parseSeq(Context& context) const
    {
        if constexpr (Index < sizeof...(Children)) {
            bool result = std::get<Index>(m_children).parse(context);
            if (result) {
                return parseSeq<Index + 1>(context);
            } else {
                return false;
            }
        } else {
            return true;
        }
    }
    std::tuple<Children...> m_children;
};

// ---------------------------------------------------------------------------
// AlternationExpr: tries each alternative in order; first success wins.
// Cut-committed failure throws peg::ParseError (hard error).
// ---------------------------------------------------------------------------
template<typename Context, typename... Children>
struct AlternationExpr : ParsingExpr<Context, AlternationExpr<Context, Children...>>
{
    AlternationExpr(const std::tuple<Children...>& children) : m_children(children) {}
    const std::tuple<Children...>& children() const { return m_children; }
    bool parse(Context& context) const override
    {
        context.init_cut();
        ScopeGuard s{[&context]() {
            context.remove_cut();
        }};
        return parse<0>(context);
    }

protected:
    template<size_t Index>
    bool parse(Context& context) const
    {
        if constexpr (Index < sizeof...(Children)) {
            bool result = std::get<Index>(m_children).parse(context);
            if (result) {
                return true;
            } else {
                if (context.cut()) {
                    // Cut-committed branch failed: escalate to a hard error.
                    // The caller (user code) catches peg::ParseError.
                    throw ParseError{context.furthest_failure_pos(), context.expected()};
                }
                return parse<Index + 1>(context);
            }
        }
        return false;
    }
    std::tuple<Children...> m_children;
};

// ---------------------------------------------------------------------------
// Repetition: matches a child expression between min_rep and max_rep times.
// Cut-committed failure in unbounded repetition throws peg::ParseError.
// ---------------------------------------------------------------------------
template<typename Context, typename Child>
struct Repetition
{
    Repetition(const Child& child, size_t min_r, std::int64_t max_r = -1)
        : m_child(child), min_rep(min_r), max_rep(max_r)
    {
        if (!((max_rep < 0) || ((max_rep >= 0) && (min_rep <= max_rep)))) {
            throw std::invalid_argument("rep not correct");
        }
    }

    const Child& child() { return m_child; }
    std::tuple<size_t, std::int64_t> reps() const { return {min_rep, max_rep}; }

    bool parse(Context& context) const
    {
        context.init_cut();
        ScopeGuard _{[&context]() {
            context.remove_cut();
        }};
        auto initState = context.state();
        bool result = true;
        size_t loopCount = 0;
        bool exited_via_failure = false;
        while (true) {
            auto startState = context.state();
            context.cut(false);
            result = m_child.parse(context);
            if (result) {
                loopCount++;
            } else {
                exited_via_failure = true;
                break;
            }
            if ((max_rep > 0) && (loopCount >= max_rep)) {
                break;
            }
            // Not advancing, stop
            if (context.state().m_pos == startState.m_pos) {
                break;
            }
        }
        if (loopCount < min_rep) {
            context.state(initState);
            return false;
        } else if (max_rep < 0) {
            // Only throw if the loop exited via a cut-committed child failure.
            // A successful no-progress iteration that happened to set cut
            // (e.g. via a lookahead) must NOT escalate to a hard error.
            if (exited_via_failure && context.cut()) {
                throw ParseError{context.furthest_failure_pos(), context.expected()};
            } else {
                return true;
            }
        }
        return true;
    }

protected:
    Child m_child;
    size_t min_rep;
    std::int64_t max_rep;
};

template<typename Context, typename Child>
struct ZeroOrMoreExpr : ParsingExpr<Context, ZeroOrMoreExpr<Context, Child>>,
                        Repetition<Context, Child>
{
    ZeroOrMoreExpr(const Child& child) : Repetition<Context, Child>(child, 0, -1) {}
    using Repetition<Context, Child>::child;
    bool parse(Context& context) const override
    {
        return Repetition<Context, Child>::parse(context);
    }
};

template<typename Context, typename Child>
struct OneOrMoreExpr : ParsingExpr<Context, OneOrMoreExpr<Context, Child>>,
                       Repetition<Context, Child>
{
    OneOrMoreExpr(const Child& child) : Repetition<Context, Child>(child, 1, -1) {}
    using Repetition<Context, Child>::child;
    bool parse(Context& context) const override
    {
        return Repetition<Context, Child>::parse(context);
    }
};

template<typename Context, typename Child>
struct NTimesExpr : ParsingExpr<Context, NTimesExpr<Context, Child>>, Repetition<Context, Child>
{
    NTimesExpr(const Child& child, size_t n_reps)
        : Repetition<Context, Child>(child, n_reps, n_reps)
    {}
    using Repetition<Context, Child>::child;
    bool parse(Context& context) const override
    {
        return Repetition<Context, Child>::parse(context);
    }
};

template<typename Context, typename Child>
struct OptionalExpr : ParsingExpr<Context, OptionalExpr<Context, Child>>, Repetition<Context, Child>
{
    OptionalExpr(const Child& child) : Repetition<Context, Child>(child, 0, 1) {}
    using Repetition<Context, Child>::child;
    bool parse(Context& context) const override
    {
        return Repetition<Context, Child>::parse(context);
    }
};

// ---------------------------------------------------------------------------
// NotExpr: negation predicate — succeeds if child fails, consumes nothing.
// ---------------------------------------------------------------------------
template<typename Context, typename Child>
struct NotExpr : ParsingExpr<Context, NotExpr<Context, Child>>
{
    NotExpr(const Child& child) : m_child(child) {}
    const Child& child() { return m_child; }
    bool parse(Context& context) const override
    {
        auto initState = context.state();
        bool result = !m_child.parse(context);
        context.state(initState);
        return result;
    }

protected:
    Child m_child;
};

// ---------------------------------------------------------------------------
// AndExpr: lookahead predicate — succeeds if child succeeds, consumes nothing.
// ---------------------------------------------------------------------------
template<typename Context, typename Child>
struct AndExpr : ParsingExpr<Context, AndExpr<Context, Child>>
{
    AndExpr(const Child& child) : m_child(child) {}
    const Child& child() { return m_child; }
    bool parse(Context& context) const override
    {
        auto initState = context.state();
        bool result = m_child.parse(context);
        context.state(initState);
        return result;
    }

protected:
    Child m_child;
};

// ---------------------------------------------------------------------------
// CutExpr: commits the current alternative/repetition scope.
// On subsequent failure in the same scope, peg::ParseError is thrown.
// ---------------------------------------------------------------------------
template<typename Context>
struct CutExpr : ParsingExpr<Context, CutExpr<Context>>
{
    bool parse(Context& context) const override
    {
        context.cut(true);
        return true;
    }
};

} // namespace parsers
} // namespace peg
