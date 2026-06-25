#pragma once
#include "peglib/ParserFwd.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
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
    using ParseResult =
        typename ParsingExpr<Context, SequenceExpr<Context, Children...>>::ParseResult;
    using ParseTreeNodePtr =
        typename ParsingExpr<Context, SequenceExpr<Context, Children...>>::ParseTreeNodePtr;

    SequenceExpr(const std::tuple<Children...>& children) : m_children{children} {}

    const std::tuple<Children...>& children() const { return m_children; }

    ParseResult parse(Context& context) const override
    {
        auto state = context.state();
        auto node = std::make_shared<typename Context::ParseTreeNode>();
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
            // Auto-skip between adjacent sequence children (not before the
            // first: leading whitespace, if desired, must be matched
            // explicitly in the start rule — pest/yhirose share this
            // convention). No-op when no skipper is configured.
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

// ---------------------------------------------------------------------------
// AlternationExpr: tries each alternative in order; first success wins.
// Returns the successful branch's ParseResult directly (including its tree).
// Cut-committed failure throws peg::ParseError (hard error).
// ---------------------------------------------------------------------------
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
                // Stamp the winning branch index so the typed fold can dispatch
                // on the winner's static type at runtime.
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

// ---------------------------------------------------------------------------
// repeat_parse_impl: the seed-grow loop behind every Repetition subclass.
// `parse_child` is any callable returning ParseResult for one child match.
//
// Cut escalation is asymmetric: unbounded (`*`/`+`, max_rep < 0) rethrows a
// cut-committed child failure as peg::ParseError; bounded (`?`/`n*e`) stops
// the loop on failure and returns the iterations matched so far, since a
// bounded repetition legitimately admits fewer matches.
// ---------------------------------------------------------------------------
template<typename Context, typename ChildOp>
    requires std::invocable<ChildOp&, Context&>
typename Context::ParseResult
repeat_parse_impl(Context& context, ChildOp parse_child, std::size_t min_rep, std::int64_t max_rep)
{
    context.init_cut();
    ScopeGuard _{[&context]() { context.remove_cut(); }};
    auto initState = context.state();
    auto node = std::make_shared<typename Context::ParseTreeNode>();
    node->start_offset = context.mark();

    std::size_t loopCount = 0;
    bool exited_via_failure = false;
    typename Context::State lastSuccessState = initState;

    while (true) {
        // Auto-skip between iterations (not before the first). No-op when
        // no skipper is configured. NB: the zero-width termination guard
        // below (startState == state()) is unaffected because a skipper
        // written as *e always succeeds and may legitimately consume
        // zero input — the child's own advancement is what the guard
        // measures, and that happens after this skip.
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
        // Not advancing, stop
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
        // Trim children to only the successful iterations.
        node->children.resize(loopCount);
    }
    if (max_rep < 0) {
        // Unbounded repetition: a cut-committed child failure escalates
        // to ParseError. Bounded repetitions (max_rep >= 0) deliberately
        // do NOT escalate — see the cut-semantics note above.
        if (exited_via_failure && context.cut()) {
            throw ParseError{context.furthest_failure_pos(), context.expected()};
        }
    }
    node->end_offset = context.mark();
    return {true, node};
}

// ---------------------------------------------------------------------------
// Repetition: matches a child expression between min_rep and max_rep times.
// Delegates its parse loop to repeat_parse_impl.
//
// CRTP self-hook: `Self` is the concrete subclass type (passed by the four
// subclasses below). This gives each subclass a DISTINCT CRTP identity while
// inheriting parse(), collect_rule_refs(), and the child/min/max members from
// this single base — no duplicated logic, no duplicated member declarations.
// The distinct identity is required by the typed-action model: result_of<E>
// and the fold dispatch on E's static type, so `*e`, `+e`, `n*e`, `-e` must
// be distinguishable.
//
// `Self` has no default: Repetition is never instantiated bare. The four
// subclasses always pass their own type.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Repetition subclasses: each fixes min/max and carries its own CRTP identity
// via the Self hook above. One line of construction each; everything else is
// inherited from Repetition. Distinct static types → distinct result_of/extract.
// ---------------------------------------------------------------------------

// ZeroOrMoreExpr: `*e` — min=0, max=unbounded.
template<typename Context, typename Child>
struct ZeroOrMoreExpr : Repetition<Context, Child, ZeroOrMoreExpr<Context, Child>>
{
    explicit ZeroOrMoreExpr(const Child& child)
        : Repetition<Context, Child, ZeroOrMoreExpr<Context, Child>>(child, 0, -1)
    {}
};

// OneOrMoreExpr: `+e` — min=1, max=unbounded.
template<typename Context, typename Child>
struct OneOrMoreExpr : Repetition<Context, Child, OneOrMoreExpr<Context, Child>>
{
    explicit OneOrMoreExpr(const Child& child)
        : Repetition<Context, Child, OneOrMoreExpr<Context, Child>>(child, 1, -1)
    {}
};

// NTimesExpr: `n*e` — min=max=n (exactly n matches).
template<typename Context, typename Child>
struct NTimesExpr : Repetition<Context, Child, NTimesExpr<Context, Child>>
{
    NTimesExpr(const Child& child, std::size_t n_reps)
        : Repetition<Context, Child, NTimesExpr<Context, Child>>(
              child, n_reps, static_cast<std::int64_t>(n_reps))
    {}
};

// OptionalExpr: `-e` / `e?` — min=0, max=1.
template<typename Context, typename Child>
struct OptionalExpr : Repetition<Context, Child, OptionalExpr<Context, Child>>
{
    explicit OptionalExpr(const Child& child)
        : Repetition<Context, Child, OptionalExpr<Context, Child>>(child, 0, 1)
    {}
};

// ---------------------------------------------------------------------------
// predicate_parse_impl: shared body for the lookahead (`&e`) and negation
// (`!e`) predicates. `parse_child` is any callable returning ParseResult for
// the operand expression.
//
// The operand is executed speculatively and its consumed input rewound; the
// result tree is always discarded.
//   - negate == false (& / AndExpr): success follows the operand's success.
//   - negate == true  (! / NotExpr): success is the operand's failure.
// ---------------------------------------------------------------------------
template<typename Context, typename ChildOp>
    requires std::invocable<ChildOp&, Context&>
typename Context::ParseResult
predicate_parse_impl(Context& context, ChildOp parse_child, bool negate)
{
    auto initState = context.state();
    auto result = parse_child(context);
    context.state(initState);
    // Predicate: no tree, no consumed input.
    return {negate ? !result.success : result.success, nullptr};
}

// ---------------------------------------------------------------------------
// NotExpr: negation predicate — succeeds if child fails, consumes nothing.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// AndExpr: lookahead predicate — succeeds if child succeeds, consumes nothing.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// LexemeExpr: disable auto-skip for a sub-expression's subtree.
//
// When a skipper is configured (Grammar::set_skipper), auto-skip fires
// between adjacent sequence children and between repetition iterations.
// For tokens whose characters must be contiguous (numbers, identifiers,
// string literals), wrap the token body in lexeme(...) to suppress
// auto-skip within it:
//
//   g["number"]  = g.lexeme(+g.terminal('0', '9'));   // "12 34" -> matches "12"
//   g["ident"]   = g.lexeme(g.terminal('a','z') >> *g.terminal('a','z'));
//
// Implemented as a save/restore of Context::skip_enabled via ScopeGuard,
// so lexeme nests safely (lexeme inside lexeme is a no-op-on-the-flag).
// Even if the wrapped expression throws peg::ParseError, the ScopeGuard
// destructor restores the prior skip_enabled before stack unwinding
// continues.
// ---------------------------------------------------------------------------
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
