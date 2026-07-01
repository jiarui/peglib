// Combinator expression types: SequenceExpr, AlternationExpr, the
// Repetition family (* + n* ?), predicates (! &), CutExpr, LexemeExpr.
// All derive from ParsingExpr<Context, Derived> (ParserFwd.h).
#pragma once
#include "peglib/ParserFwd.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>

namespace peg
{
namespace parsers
{

// Matches child expressions in order; all must succeed. Auto-skip fires
// between adjacent children (Index > 0); see Context::run_skipper.
template<typename Context, typename... Children>
struct SequenceExpr : ParsingExpr<Context, SequenceExpr<Context, Children...>>
{
    using ParseResult =
        typename ParsingExpr<Context, SequenceExpr<Context, Children...>>::ParseResult;
    using ParseTreeNodePtr =
        typename ParsingExpr<Context, SequenceExpr<Context, Children...>>::ParseTreeNodePtr;

    SequenceExpr(const std::tuple<Children...>& children) : m_children{children} {}

    const std::tuple<Children...>& children() const { return m_children; }

    ParseResult parse(Context& context) const override
    {
        auto state = context.state();
        auto node = context.make_node();
        node->start_offset = context.mark();
        if (parseSeq<0>(context, node)) {
            node->end_offset = context.mark();
            return {true, node};
        }
        context.state(state);
        return {false, nullptr};
    }

    void collect_rule_refs(std::set<std::string>& refs) const override
    {
        std::apply([&refs](const auto&... c) { (c.collect_rule_refs(refs), ...); }, m_children);
    }

protected:
    template<size_t Index>
    bool parseSeq(Context& context, ParseTreeNodePtr& node) const
    {
        if constexpr (Index < sizeof...(Children)) {
            if constexpr (Index > 0) {
                context.run_skipper();
            }
            auto result = std::get<Index>(m_children).parse(context);
            if (result.success) {
                if (result.tree)
                    node->children.push_back(result.tree);
                return parseSeq<Index + 1>(context, node);
            }
            return false;
        }
        return true;
    }
    std::tuple<Children...> m_children;
};

// Tries each alternative in order; first success wins. Cut-committed failure
// throws peg::ParseError. The winning branch index is stamped on the node
// (alt_winner) so the typed fold can dispatch on the winning branch's type.
template<typename Context, typename... Children>
struct AlternationExpr : ParsingExpr<Context, AlternationExpr<Context, Children...>>
{
    using ParseResult =
        typename ParsingExpr<Context, AlternationExpr<Context, Children...>>::ParseResult;

    AlternationExpr(const std::tuple<Children...>& children) : m_children(children) {}
    const std::tuple<Children...>& children() const { return m_children; }

    ParseResult parse(Context& context) const override
    {
        context.init_cut();
        ScopeGuard s{[&context]() { context.remove_cut(); }};
        return parseAlt<0>(context);
    }

    void collect_rule_refs(std::set<std::string>& refs) const override
    {
        std::apply([&refs](const auto&... c) { (c.collect_rule_refs(refs), ...); }, m_children);
    }

protected:
    template<size_t Index>
    ParseResult parseAlt(Context& context) const
    {
        if constexpr (Index < sizeof...(Children)) {
            auto result = std::get<Index>(m_children).parse(context);
            if (result.success) {
                if (result.tree)
                    result.tree->alt_winner = Index;
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

// Seed-grow loop behind every Repetition subclass.
//
// Cut escalation is asymmetric: unbounded (* +, max_rep < 0) rethrows a
// cut-committed child failure as peg::ParseError; bounded (? n*e) stops on
// failure and returns the iterations matched so far, since a bounded
// repetition legitimately admits fewer matches.
template<typename Context, typename ChildOp>
    requires std::invocable<ChildOp&, Context&>
typename Context::ParseResult
repeat_parse_impl(Context& context, ChildOp parse_child, std::size_t min_rep, std::int64_t max_rep)
{
    context.init_cut();
    ScopeGuard _{[&context]() { context.remove_cut(); }};
    auto initState = context.state();
    auto node = context.make_node();
    node->start_offset = context.mark();

    std::size_t loopCount = 0;
    bool exited_via_failure = false;
    typename Context::State lastSuccessState = initState;

    while (true) {
        if (loopCount > 0) {
            context.run_skipper();
        }
        auto startState = context.state();
        context.cut(false);
        auto result = parse_child(context);
        if (result.success) {
            loopCount++;
            lastSuccessState = context.state();
            if (result.tree)
                node->children.push_back(result.tree);
        } else {
            exited_via_failure = true;
            break;
        }
        if ((max_rep > 0) && (loopCount >= static_cast<std::size_t>(max_rep))) {
            break;
        }
        if (context.state() == startState) {
            break;
        }
    }

    if (loopCount < min_rep) {
        context.state(initState);
        return {false, nullptr};
    }
    if (exited_via_failure) {
        context.state(lastSuccessState);
        node->children.resize(loopCount);
    }
    if (max_rep < 0) {
        if (exited_via_failure && context.cut()) {
            throw ParseError{context.furthest_failure_pos(), context.expected()};
        }
    }
    node->end_offset = context.mark();
    return {true, node};
}

// Matches a child between min_rep and max_rep times. The four subclasses below
// fix min/max and carry their own CRTP identity (Self) so result_of<E> and the
// fold dispatch on E's static type can distinguish *e / +e / n*e / -e.
template<typename Context, typename Child, typename Self>
struct Repetition : ParsingExpr<Context, Self>
{
    using ParseResult = typename ParsingExpr<Context, Self>::ParseResult;

    Repetition(Child child, std::size_t min_r, std::int64_t max_r = -1)
        : m_child(std::move(child)), min_rep(min_r), max_rep(max_r)
    {
        if (!((max_rep < 0) ||
              ((max_rep >= 0) && (min_rep <= static_cast<std::size_t>(max_rep))))) {
            throw std::invalid_argument("rep not correct");
        }
    }

    [[nodiscard]] const Child& child() const noexcept { return m_child; }
    [[nodiscard]] std::tuple<std::size_t, std::int64_t> reps() const noexcept
    {
        return {min_rep, max_rep};
    }

    ParseResult parse(Context& context) const override
    {
        return repeat_parse_impl(
            context, [this](Context& c) { return m_child.parse(c); }, min_rep, max_rep);
    }

    void collect_rule_refs(std::set<std::string>& refs) const override
    {
        m_child.collect_rule_refs(refs);
    }

protected:
    Child m_child;
    std::size_t min_rep;
    std::int64_t max_rep;
};

template<typename Context, typename Child>
struct ZeroOrMoreExpr : Repetition<Context, Child, ZeroOrMoreExpr<Context, Child>>
{
    explicit ZeroOrMoreExpr(const Child& child)
        : Repetition<Context, Child, ZeroOrMoreExpr<Context, Child>>(child, 0, -1)
    {}
};

template<typename Context, typename Child>
struct OneOrMoreExpr : Repetition<Context, Child, OneOrMoreExpr<Context, Child>>
{
    explicit OneOrMoreExpr(const Child& child)
        : Repetition<Context, Child, OneOrMoreExpr<Context, Child>>(child, 1, -1)
    {}
};

template<typename Context, typename Child>
struct NTimesExpr : Repetition<Context, Child, NTimesExpr<Context, Child>>
{
    NTimesExpr(const Child& child, std::size_t n_reps)
        : Repetition<Context, Child, NTimesExpr<Context, Child>>(
              child, n_reps, static_cast<std::int64_t>(n_reps))
    {}
};

template<typename Context, typename Child>
struct OptionalExpr : Repetition<Context, Child, OptionalExpr<Context, Child>>
{
    explicit OptionalExpr(const Child& child)
        : Repetition<Context, Child, OptionalExpr<Context, Child>>(child, 0, 1)
    {}
};

// Shared body for lookahead (&) and negation (!) predicates. The operand is
// executed speculatively and its consumed input rewound; the result tree is
// always discarded.
template<typename Context, typename ChildOp>
    requires std::invocable<ChildOp&, Context&>
typename Context::ParseResult
predicate_parse_impl(Context& context, ChildOp parse_child, bool negate)
{
    auto initState = context.state();
    auto result = parse_child(context);
    context.state(initState);
    return {negate ? !result.success : result.success, nullptr};
}

template<typename Context, typename Child>
struct NotExpr : ParsingExpr<Context, NotExpr<Context, Child>>
{
    NotExpr(Child child) : m_child(std::move(child)) {}
    const Child& child() { return m_child; }
    typename Context::ParseResult parse(Context& context) const override
    {
        return predicate_parse_impl(context, [this](Context& c) { return m_child.parse(c); }, true);
    }

    void collect_rule_refs(std::set<std::string>& refs) const override
    {
        m_child.collect_rule_refs(refs);
    }

protected:
    Child m_child;
};

template<typename Context, typename Child>
struct AndExpr : ParsingExpr<Context, AndExpr<Context, Child>>
{
    AndExpr(Child child) : m_child(std::move(child)) {}
    const Child& child() { return m_child; }
    typename Context::ParseResult parse(Context& context) const override
    {
        return predicate_parse_impl(
            context, [this](Context& c) { return m_child.parse(c); }, false);
    }

    void collect_rule_refs(std::set<std::string>& refs) const override
    {
        m_child.collect_rule_refs(refs);
    }

protected:
    Child m_child;
};

// Commits the current alternative/repetition scope. Subsequent failure in the
// same scope throws peg::ParseError.
template<typename Context>
struct CutExpr : ParsingExpr<Context, CutExpr<Context>>
{
    typename Context::ParseResult parse(Context& context) const override
    {
        context.cut(true);
        return {true, nullptr};
    }
};

// Disables auto-skip for a sub-expression's subtree. Save/restore of
// skip_enabled via ScopeGuard makes lexeme(...) nest safely and exception-safe.
template<typename Context, typename Child>
struct LexemeExpr : ParsingExpr<Context, LexemeExpr<Context, Child>>
{
    using ParseResult = typename ParsingExpr<Context, LexemeExpr<Context, Child>>::ParseResult;

    explicit LexemeExpr(Child child) : m_child(std::move(child)) {}

    [[nodiscard]] const Child& child() const noexcept { return m_child; }

    ParseResult parse(Context& context) const override
    {
        bool prev = context.skip_enabled();
        context.skip_enabled(false);
        ScopeGuard restore{[&context, prev]() { context.skip_enabled(prev); }};
        return m_child.parse(context);
    }

    void collect_rule_refs(std::set<std::string>& refs) const override
    {
        m_child.collect_rule_refs(refs);
    }

protected:
    Child m_child;
};

} // namespace parsers
} // namespace peg
