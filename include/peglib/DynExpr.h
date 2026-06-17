#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "peglib/Combinators.h"
#include "peglib/ParserFwd.h"
#include "peglib/Terminals.h"

namespace peg
{
namespace parsers
{

// ---------------------------------------------------------------------------
// Dyn* — dynamically-typed parsing expressions.
//
// The compile-time DSL (SequenceExpr<C, A, B>, ...) encodes child types in
// the class template parameters. For grammars assembled at runtime (e.g.
// from textual PEG input), child types are not known until runtime, so we
// type-erasure each child behind shared_ptr<ParsingExprInterface<Context>>
// and store them in a vector.
//
// Each Dyn* type mirrors its static counterpart's semantics and returns
// ParseResult (success + optional tree) exactly like the static DSL.
// ---------------------------------------------------------------------------

template<typename Context>
struct DynSequenceExpr : ParsingExpr<Context, DynSequenceExpr<Context>>
{
    using InterfacePtr = std::shared_ptr<ParsingExprInterface<Context>>;
    using ParseResult = typename Context::ParseResult;

    explicit DynSequenceExpr(std::vector<InterfacePtr> children)
        : m_children(std::move(children)) {}

    ParseResult parse(Context& context) const override
    {
        auto state = context.state();
        auto node = std::make_shared<typename Context::ParseTreeNode>();
        node->start_offset = context.offset_of(context.mark());
        for (const auto& child : m_children) {
            auto result = child->parse(context);
            if (!result.success) {
                context.state(state);
                return {false, nullptr};
            }
            if (result.tree) node->children.push_back(result.tree);
        }
        node->end_offset = context.offset_of(context.mark());
        return {true, node};
    }

    [[nodiscard]] const std::vector<InterfacePtr>& children() const noexcept
    {
        return m_children;
    }

protected:
    std::vector<InterfacePtr> m_children;
};

template<typename Context>
struct DynAlternationExpr : ParsingExpr<Context, DynAlternationExpr<Context>>
{
    using InterfacePtr = std::shared_ptr<ParsingExprInterface<Context>>;
    using ParseResult = typename Context::ParseResult;

    explicit DynAlternationExpr(std::vector<InterfacePtr> children)
        : m_children(std::move(children)) {}

    ParseResult parse(Context& context) const override
    {
        context.init_cut();
        ScopeGuard guard{[&context]() { context.remove_cut(); }};
        for (const auto& child : m_children) {
            auto result = child->parse(context);
            if (result.success) {
                return result;
            }
            if (context.cut()) {
                throw ParseError{context.furthest_failure_pos(), context.expected()};
            }
        }
        return {false, nullptr};
    }

    [[nodiscard]] const std::vector<InterfacePtr>& children() const noexcept
    {
        return m_children;
    }

protected:
    std::vector<InterfacePtr> m_children;
};

template<typename Context>
struct DynRepeatExpr : ParsingExpr<Context, DynRepeatExpr<Context>>
{
    using InterfacePtr = std::shared_ptr<ParsingExprInterface<Context>>;
    using ParseResult = typename Context::ParseResult;

    DynRepeatExpr(InterfacePtr child, std::size_t min_r, std::int64_t max_r)
        : m_child(std::move(child)), min_rep(min_r), max_rep(max_r) {}

    ParseResult parse(Context& context) const override
    {
        context.init_cut();
        ScopeGuard guard{[&context]() { context.remove_cut(); }};
        auto init_state = context.state();
        auto node = std::make_shared<typename Context::ParseTreeNode>();
        node->start_offset = context.offset_of(context.mark());

        std::size_t loop_count = 0;
        bool exited_via_failure = false;
        typename Context::State last_success_state = init_state;

        while (true) {
            auto start_state = context.state();
            context.cut(false);
            auto result = m_child->parse(context);
            if (result.success) {
                loop_count++;
                last_success_state = context.state();
                if (result.tree) node->children.push_back(result.tree);
            } else {
                exited_via_failure = true;
                break;
            }
            if (max_rep > 0 && loop_count >= static_cast<std::size_t>(max_rep)) {
                break;
            }
            if (context.state().m_pos == start_state.m_pos) {
                break;
            }
        }

        if (loop_count < min_rep) {
            context.state(init_state);
            return {false, nullptr};
        }
        if (exited_via_failure) {
            context.state(last_success_state);
            node->children.resize(loop_count);
        }
        if (max_rep < 0 && exited_via_failure && context.cut()) {
            throw ParseError{context.furthest_failure_pos(), context.expected()};
        }
        node->end_offset = context.offset_of(context.mark());
        return {true, node};
    }

    [[nodiscard]] const InterfacePtr& child() const noexcept { return m_child; }
    [[nodiscard]] std::tuple<std::size_t, std::int64_t> reps() const noexcept
    {
        return {min_rep, max_rep};
    }

protected:
    InterfacePtr m_child;
    std::size_t min_rep;
    std::int64_t max_rep;
};

template<typename Context>
struct DynAndExpr : ParsingExpr<Context, DynAndExpr<Context>>
{
    using InterfacePtr = std::shared_ptr<ParsingExprInterface<Context>>;
    explicit DynAndExpr(InterfacePtr child) : m_child(std::move(child)) {}
    typename Context::ParseResult parse(Context& context) const override
    {
        auto state = context.state();
        auto result = m_child->parse(context);
        context.state(state);
        return {result.success, nullptr};
    }
    [[nodiscard]] const InterfacePtr& child() const noexcept { return m_child; }

protected:
    InterfacePtr m_child;
};

template<typename Context>
struct DynNotExpr : ParsingExpr<Context, DynNotExpr<Context>>
{
    using InterfacePtr = std::shared_ptr<ParsingExprInterface<Context>>;
    explicit DynNotExpr(InterfacePtr child) : m_child(std::move(child)) {}
    typename Context::ParseResult parse(Context& context) const override
    {
        auto state = context.state();
        auto result = m_child->parse(context);
        context.state(state);
        return {!result.success, nullptr};
    }
    [[nodiscard]] const InterfacePtr& child() const noexcept { return m_child; }

protected:
    InterfacePtr m_child;
};

// ---------------------------------------------------------------------------
// DynExpr: user-visible handle wrapping any ParsingExprInterface.
// ---------------------------------------------------------------------------
template<typename Context>
struct DynExpr : ParsingExpr<Context, DynExpr<Context>>
{
    using InterfacePtr = std::shared_ptr<ParsingExprInterface<Context>>;

    DynExpr() = default;
    explicit DynExpr(InterfacePtr impl) : m_impl(std::move(impl)) {}

    typename Context::ParseResult parse(Context& context) const override
    {
        return m_impl->parse(context);
    }

    [[nodiscard]] const InterfacePtr& impl() const noexcept { return m_impl; }

protected:
    InterfacePtr m_impl;
};

} // namespace parsers
} // namespace peg
