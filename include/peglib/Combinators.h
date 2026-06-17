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
// On success, returns an anonymous grouping node whose children are the
// non-null trees produced by each child expression.
// ---------------------------------------------------------------------------
template<typename Context, typename... Children>
struct SequenceExpr : ParsingExpr<Context, SequenceExpr<Context, Children...>>
{
    using ParseResult = typename ParsingExpr<Context, SequenceExpr<Context, Children...>>::ParseResult;
    using ParseTreeNodePtr = typename ParsingExpr<Context, SequenceExpr<Context, Children...>>::ParseTreeNodePtr;

    SequenceExpr(const std::tuple<Children...>& children) : m_children{children} {}

    const std::tuple<Children...>& children() const { return m_children; }

    ParseResult parse(Context& context) const override
    {
        auto state = context.state();
        auto node = std::make_shared<typename Context::ParseTreeNode>();
        node->start_offset = context.offset_of(context.mark());
        if (parseSeq<0>(context, node)) {
            node->end_offset = context.offset_of(context.mark());
            return {true, node};
        }
        context.state(state);
        return {false, nullptr};
    }

protected:
    template<size_t Index>
    bool parseSeq(Context& context, ParseTreeNodePtr& node) const
    {
        if constexpr (Index < sizeof...(Children)) {
            auto result = std::get<Index>(m_children).parse(context);
            if (result.success) {
                if (result.tree) node->children.push_back(result.tree);
                return parseSeq<Index + 1>(context, node);
            }
            return false;
        }
        return true;
    }
    std::tuple<Children...> m_children;
};

// ---------------------------------------------------------------------------
// AlternationExpr: tries each alternative in order; first success wins.
// Returns the successful branch's ParseResult directly (including its tree).
// Cut-committed failure throws peg::ParseError (hard error).
// ---------------------------------------------------------------------------
template<typename Context, typename... Children>
struct AlternationExpr : ParsingExpr<Context, AlternationExpr<Context, Children...>>
{
    using ParseResult = typename ParsingExpr<Context, AlternationExpr<Context, Children...>>::ParseResult;

    AlternationExpr(const std::tuple<Children...>& children) : m_children(children) {}
    const std::tuple<Children...>& children() const { return m_children; }

    ParseResult parse(Context& context) const override
    {
        context.init_cut();
        ScopeGuard s{[&context]() { context.remove_cut(); }};
        return parseAlt<0>(context);
    }

protected:
    template<size_t Index>
    ParseResult parseAlt(Context& context) const
    {
        if constexpr (Index < sizeof...(Children)) {
            auto result = std::get<Index>(m_children).parse(context);
            if (result.success) {
                return result;
            }
            if (context.cut()) {
                throw ParseError{context.furthest_failure_pos(), context.expected()};
            }
            return parseAlt<Index + 1>(context);
        }
        return {false, nullptr};
    }
    std::tuple<Children...> m_children;
};

// ---------------------------------------------------------------------------
// Repetition: matches a child expression between min_rep and max_rep times.
// On success, returns an anonymous grouping node whose children are the
// non-null trees from each successful iteration.
// Cut-committed failure in unbounded repetition throws peg::ParseError.
// ---------------------------------------------------------------------------
template<typename Context, typename Child>
struct Repetition
{
    using ParseResult = typename Context::ParseResult;
    using ParseTreeNodePtr = typename Context::ParseTreeNodePtr;

    Repetition(const Child& child, size_t min_r, std::int64_t max_r = -1)
        : m_child(child), min_rep(min_r), max_rep(max_r)
    {
        if (!((max_rep < 0) || ((max_rep >= 0) && (min_rep <= max_rep)))) {
            throw std::invalid_argument("rep not correct");
        }
    }

    const Child& child() { return m_child; }
    std::tuple<size_t, std::int64_t> reps() const { return {min_rep, max_rep}; }

    ParseResult parse(Context& context) const
    {
        context.init_cut();
        ScopeGuard _{[&context]() { context.remove_cut(); }};
        auto initState = context.state();
        auto node = std::make_shared<typename Context::ParseTreeNode>();
        node->start_offset = context.offset_of(context.mark());

        size_t loopCount = 0;
        bool exited_via_failure = false;
        typename Context::State lastSuccessState = initState;

        while (true) {
            auto startState = context.state();
            context.cut(false);
            auto result = m_child.parse(context);
            if (result.success) {
                loopCount++;
                lastSuccessState = context.state();
                if (result.tree) node->children.push_back(result.tree);
            } else {
                exited_via_failure = true;
                break;
            }
            if ((max_rep > 0) && (loopCount >= static_cast<size_t>(max_rep))) {
                break;
            }
            // Not advancing, stop
            if (context.state().m_pos == startState.m_pos) {
                break;
            }
        }

        if (loopCount < min_rep) {
            context.state(initState);
            return {false, nullptr};
        }
        if (exited_via_failure) {
            context.state(lastSuccessState);
            // Trim children to only the successful iterations.
            node->children.resize(loopCount);
        }
        if (max_rep < 0) {
            if (exited_via_failure && context.cut()) {
                throw ParseError{context.furthest_failure_pos(), context.expected()};
            }
        }
        node->end_offset = context.offset_of(context.mark());
        return {true, node};
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
    typename Context::ParseResult parse(Context& context) const override
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
    typename Context::ParseResult parse(Context& context) const override
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
    typename Context::ParseResult parse(Context& context) const override
    {
        return Repetition<Context, Child>::parse(context);
    }
};

template<typename Context, typename Child>
struct OptionalExpr : ParsingExpr<Context, OptionalExpr<Context, Child>>, Repetition<Context, Child>
{
    OptionalExpr(const Child& child) : Repetition<Context, Child>(child, 0, 1) {}
    using Repetition<Context, Child>::child;
    typename Context::ParseResult parse(Context& context) const override
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
    typename Context::ParseResult parse(Context& context) const override
    {
        auto initState = context.state();
        auto result = m_child.parse(context);
        context.state(initState);
        // Predicate: no tree, no consumed input.
        return {!result.success, nullptr};
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
    typename Context::ParseResult parse(Context& context) const override
    {
        auto initState = context.state();
        auto result = m_child.parse(context);
        context.state(initState);
        // Predicate: no tree, no consumed input.
        return {result.success, nullptr};
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
    typename Context::ParseResult parse(Context& context) const override
    {
        context.cut(true);
        return {true, nullptr};
    }
};

} // namespace parsers
} // namespace peg
