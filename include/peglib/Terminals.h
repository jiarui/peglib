#pragma once
#include "peglib/ParserFwd.h"

#include <cstddef>
#include <ranges>

namespace peg
{
namespace parsers
{

// ---------------------------------------------------------------------------
// TerminalExpr: matches a single value, set, range, or predicate.
// Records an ExpectedItem on failure for error reporting.
// ---------------------------------------------------------------------------
template<typename Context, typename TerminalValueType>
struct TerminalExpr : ParsingExpr<Context, TerminalExpr<Context, TerminalValueType>>
{
    using SemanticAction =
        typename ParsingExpr<Context, TerminalExpr<Context, TerminalValueType>>::SemanticAction;
    TerminalExpr(TerminalValueType value, SemanticAction action = nullptr)
        : ParsingExpr<Context, TerminalExpr<Context, TerminalValueType>>(std::move(action)),
          m_terminalValue{std::move(value)}
    {}
    typename Context::ParseResult parse(Context& context) const override
    {
        if (!context.ended() && symbolConsumable(context.current(), m_terminalValue)) {
            context.next();
            return {true, nullptr};
        }
        record_terminal_expected(context, m_terminalValue, "<terminal>");
        return {false, nullptr};
    }

protected:
    TerminalValueType m_terminalValue;
};

// ---------------------------------------------------------------------------
// TerminalSeqExpr: matches a sequence of values (random-access range).
// ---------------------------------------------------------------------------
template<typename Context, typename SeqType>
    requires std::ranges::random_access_range<SeqType>
struct TerminalSeqExpr : ParsingExpr<Context, TerminalSeqExpr<Context, SeqType>>
{
    TerminalSeqExpr(SeqType value) : m_terminalValues{std::move(value)} {}
    typename Context::ParseResult parse(Context& context) const override
    {
        auto initState = context.state();
        for (const auto& i : m_terminalValues) {
            if (!context.ended() && symbolConsumable(context.current(), i)) {
                context.next();
            } else {
                context.state(initState);
                record_expected(context);
                return {false, nullptr};
            }
        }
        return {true, nullptr};
    }

protected:
    SeqType m_terminalValues;

private:
    // Record the expected sequence as a single Literal item.
    void record_expected(Context& context) const
    {
        std::size_t pos = context.mark();
        // Build a printable form of the sequence. The to_display CPO renders
        // each element to UTF-8 (char passthrough / char32_t UTF-8 encoding /
        // user-supplied hook for non-integral element types) so sequences
        // display correctly instead of being truncated by static_cast<char>.
        std::string text;
        for (const auto& v : m_terminalValues) {
            text += to_display_cpo(v);
        }
        context.record_failure(
            pos,
            ExpectedItem{.kind = ExpectedKind::Literal, .text = escape_string_for_expected(text)});
    }
};

// ---------------------------------------------------------------------------
// TokenExpr: like TerminalExpr, but **keeps** the matched element as a typed
// result (value_type). Match behaviour is identical to TerminalExpr (same
// symbolConsumable logic, same failure-recording); the difference is that
// TokenExpr builds a ParseTreeNode so its slot survives into the fold, where
// the matched element is recovered from ctx.at(span.start). No value is
// stashed on the node — the fold reads it from the span.
//
// Contrast with TerminalExpr (void terminal): TerminalExpr produces no node
// and is filtered out of sequence results, so structural tokens (parentheses,
// keywords, separators) never appear as action parameters. TokenExpr is for
// tokens whose identity the action needs.
// ---------------------------------------------------------------------------
template<typename Context, typename TerminalValueType>
struct TokenExpr : ParsingExpr<Context, TokenExpr<Context, TerminalValueType>>
{
    TokenExpr(TerminalValueType value) : m_terminalValue{std::move(value)} {}

    typename Context::ParseResult parse(Context& context) const override
    {
        if (!context.ended() && symbolConsumable(context.current(), m_terminalValue)) {
            context.next();
            // Build a node bracketing exactly the one matched element. The fold
            // recovers the element via ctx.at(start_offset) — no value stash.
            auto node = std::make_shared<typename Context::ParseTreeNode>();
            node->start_offset = context.mark() - 1;
            node->end_offset = context.mark();
            return {true, node};
        }
        record_terminal_expected(context, m_terminalValue, "<token>");
        return {false, nullptr};
    }

protected:
    TerminalValueType m_terminalValue;
};

// ---------------------------------------------------------------------------
// EmptyExpr: always succeeds, consumes nothing.
// ---------------------------------------------------------------------------
template<typename Context>
struct EmptyExpr : ParsingExpr<Context, EmptyExpr<Context>>
{
    EmptyExpr() = default;
    typename Context::ParseResult parse(Context& /*context*/) const override
    {
        return {true, nullptr};
    }
};

} // namespace parsers
} // namespace peg
