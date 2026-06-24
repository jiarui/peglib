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
// repeat_parse_impl: the single source of truth for repetition semantics
// (seed-grow loop, cut handling, zero-width termination, failure rollback).
//
// Both the static Repetition<C,Child> and the type-erased DynRepeatExpr<C>
// delegate here, so a fix only has to be applied once. `parse_child` is any
// callable returning ParseResult for one child match — typically a lambda
// capturing `this` and forwarding to the concrete child's parse().
//
// Cut semantics (intentional asymmetry between bounded and unbounded):
//   - Unbounded (max_rep < 0, i.e. `*` / `+`): a cut-committed failure of
//     the child escalates to peg::ParseError — the loop is "infinite" and a
//     committed failure cannot be recovered by trying fewer iterations.
//   - Bounded (max_rep >= 0, i.e. `?` / `n*e`): a cut inside the child
//     commits only to THIS repetition's scope. On failure the loop simply
//     stops (returning the iterations matched so far); it does NOT throw,
//     because a bounded repetition legitimately admits "fewer matches".
//     So `2 * ('a' >> cut() >> 'b')` on "ax" stops after iteration 1 and
//     returns success, not a ParseError.
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
// Directly polymorphic (derives from ParsingExpr), delegating its parse loop
// to repeat_parse_impl. ZeroOrMoreExpr / OneOrMoreExpr / NTimesExpr /
// OptionalExpr are thin subclasses that only fix min/max — they inherit
// parse() and collect_rule_refs(), so there is no duplicated logic and no
// multiple-inheritance bridging.
// ---------------------------------------------------------------------------
template<typename Context, typename Child>
struct Repetition : ParsingExpr<Context, Repetition<Context, Child>>
{
    using ParseResult = typename ParsingExpr<Context, Repetition<Context, Child>>::ParseResult;

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

// ZeroOrMoreExpr: `*e` — Repetition with min=0, max=unbounded.
template<typename Context, typename Child>
struct ZeroOrMoreExpr : Repetition<Context, Child>
{
    ZeroOrMoreExpr(const Child& child) : Repetition<Context, Child>(child, 0, -1) {}
};

// OneOrMoreExpr: `+e` — Repetition with min=1, max=unbounded.
template<typename Context, typename Child>
struct OneOrMoreExpr : Repetition<Context, Child>
{
    OneOrMoreExpr(const Child& child) : Repetition<Context, Child>(child, 1, -1) {}
};

// NTimesExpr: `n*e` — Repetition with min=max=n (exactly n matches).
template<typename Context, typename Child>
struct NTimesExpr : Repetition<Context, Child>
{
    NTimesExpr(const Child& child, std::size_t n_reps)
        : Repetition<Context, Child>(child, n_reps, n_reps)
    {}
};

// OptionalExpr: `-e` / `e?` — Repetition with min=0, max=1.
template<typename Context, typename Child>
struct OptionalExpr : Repetition<Context, Child>
{
    OptionalExpr(const Child& child) : Repetition<Context, Child>(child, 0, 1) {}
};

// ---------------------------------------------------------------------------
// predicate_parse_impl: the single source of truth for predicate semantics
// (lookahead `&e` and negation `!e`).
//
// Both the static AndExpr/NotExpr and the type-erased DynAndExpr/DynNotExpr
// delegate here, so a fix only has to be applied once. `parse_child` is any
// callable returning ParseResult for the operand expression — typically a
// lambda capturing `this` and forwarding to the concrete child's parse().
//
// Predicate contract: the operand is executed speculatively; whatever it
// consumed is rewound (the input position is restored to the pre-state);
// the result tree is always discarded (no tree is built for predicates).
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
// sequence_parse_impl: the single source of truth for sequence semantics
// across a runtime-known number of children.
//
// Used by the type-erased DynSequenceExpr. The static SequenceExpr keeps its
// compile-time recursive `parseSeq<Index>` template — forcing the static path
// into this indexed form would introduce a runtime branch on `i > 0` for the
// skipper inside what is currently a fully-inlined recursion, destroying the
// static DSL's devirtualization (see TODO.md "ChildContainer Concept" row).
// This impl is the indexed-child counterpart that future DynExpr types
// (DynCaptureExpr, DynRecoverExpr, …) can reuse.
//
// `parse_at` is any callable returning ParseResult for child `i`.
// `count` is the number of children. The skipper is run between adjacent
// children (not before the first) — no-op when no skipper is configured.
// ---------------------------------------------------------------------------
template<typename Context, typename ChildOp>
    requires std::invocable<ChildOp&, Context&, std::size_t>
typename Context::ParseResult
sequence_parse_impl(Context& context, ChildOp parse_at, std::size_t count)
{
    auto state = context.state();
    auto node = std::make_shared<typename Context::ParseTreeNode>();
    node->start_offset = context.mark();
    bool first = true;
    for (std::size_t i = 0; i < count; ++i) {
        // Auto-skip between adjacent children (not before the first),
        // mirroring SequenceExpr. No-op when no skipper is configured.
        if (!first) {
            context.run_skipper();
        }
        first = false;
        auto result = parse_at(context, i);
        if (!result.success) {
            context.state(state);
            return {false, nullptr};
        }
        if (result.tree)
            node->children.push_back(result.tree);
    }
    node->end_offset = context.mark();
    return {true, node};
}

// ---------------------------------------------------------------------------
// choice_parse_impl: the single source of truth for ordered-choice semantics
// across a runtime-known number of children.
//
// Used by the type-erased DynAlternationExpr. As with sequence_parse_impl,
// the static AlternationExpr keeps its compile-time recursive `parseAlt<Index>`
// template for devirtualization; this impl serves the dynamic path only.
//
// Cut contract: a fresh cut scope is pushed for the choice. The first
// successful alternative wins. If an alternative fails after a cut was
// committed inside it, the failure escalates to peg::ParseError (hard
// error) — the programmer has explicitly committed this branch.
// ---------------------------------------------------------------------------
template<typename Context, typename ChildOp>
    requires std::invocable<ChildOp&, Context&, std::size_t>
typename Context::ParseResult
choice_parse_impl(Context& context, ChildOp parse_at, std::size_t count)
{
    context.init_cut();
    ScopeGuard _{[&context]() { context.remove_cut(); }};
    for (std::size_t i = 0; i < count; ++i) {
        auto result = parse_at(context, i);
        if (result.success) {
            return result;
        }
        if (context.cut()) {
            throw ParseError{context.furthest_failure_pos(), context.expected()};
        }
    }
    return {false, nullptr};
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
