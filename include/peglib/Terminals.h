// Leaf matching expressions: TerminalExpr (void result, filtered), TokenExpr
// (value_type result, kept), TerminalSeqExpr (multi-char literal), MatcherExpr
// (match-time predicate), EmptyExpr.
#pragma once
#include "peglib/ParserFwd.h"

#include <cstddef>
#include <optional>
#include <ranges>

namespace peg
{
namespace parsers
{

// Matches a single value, set, range, or predicate. Result is void —
// filtered out of sequence results, so structural tokens (parens, keywords)
// never appear as typed-action parameters.
template<typename Context, typename TerminalValueType>
struct TerminalExpr : ParsingExpr<Context, TerminalExpr<Context, TerminalValueType>>
{
    explicit TerminalExpr(TerminalValueType value) : m_terminalValue{std::move(value)} {}
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

// Matches a contiguous run of elements (random-access range).
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
    void record_expected(Context& context) const
    {
        std::size_t pos = context.mark();
        // Lazy: build the escaped literal only if this position is retained
        // (furthest-or-tied). See record_terminal_expected for rationale.
        context.record_failure_lazy(pos, [&]() {
            std::string text;
            for (const auto& v : m_terminalValues) {
                text += to_display_cpo(v);
            }
            return ExpectedItem{.kind = ExpectedKind::Literal,
                                .text = escape_string_for_expected(text)};
        });
    }
};

// Like TerminalExpr but **keeps** the matched element as a typed result
// (value_type). Builds a node bracketing exactly the one matched element; the
// fold recovers the element via ctx.at(span.start). Use this for tokens whose
// identity the action needs; use TerminalExpr for structural tokens.
template<typename Context, typename TerminalValueType>
struct TokenExpr : ParsingExpr<Context, TokenExpr<Context, TerminalValueType>>
{
    TokenExpr(TerminalValueType value) : m_terminalValue{std::move(value)} {}

    typename Context::ParseResult parse(Context& context) const override
    {
        if (!context.ended() && symbolConsumable(context.current(), m_terminalValue)) {
            context.next();
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

// Match-time primitive (a weakened lpeg.Cmt). Wraps a user function
// `fn(Context&, Span) -> std::optional<Span>` that reads the context READ-ONLY
// (must not call next()/reset()) and returns the span it consumed, or nullopt
// to reject. MatcherExpr advances the position to span.end and builds a node.
// Result is void (a recognizer); observe what it matched via on_match reading
// the node's span. Use it for Lua long brackets, balanced delimiters, etc.
template<typename Context, typename Fn>
struct MatcherExpr : ParsingExpr<Context, MatcherExpr<Context, Fn>>
{
    explicit MatcherExpr(Fn fn) : m_fn(std::move(fn)) {}

    typename Context::ParseResult parse(Context& context) const override
    {
        std::size_t start = context.mark();
        std::optional<Span> consumed = m_fn(context, Span{start, start});
        if (consumed) {
            context.reset(consumed->end);
            auto node = std::make_shared<typename Context::ParseTreeNode>();
            node->start_offset = start;
            node->end_offset = consumed->end;
            return {true, node};
        }
        return {false, nullptr};
    }

protected:
    Fn m_fn;
};

// Always succeeds, consumes nothing.
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
