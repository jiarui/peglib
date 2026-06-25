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
// MatcherExpr: a match-time primitive (a weakened lpeg.Cmt).
//
// Wraps a user function `fn(Context&, Span) -> std::optional<Span>` that reads
// the context read-only and returns the span it consumed, or std::nullopt to
// reject. MatcherExpr::parse then advances the context to span.end and builds a
// node bracketing [start, span.end). The node survives into the tree so its
// offsets are observable — via on_match (the side-effect hook) or parse_tree.
//
// CONTRACT:
//   - fn reads the input via context.current()/at()/mark()/ended()/
//     input_size(). It must NOT mutate position (no next()/reset()) —
//     MatcherExpr owns the single advance to span.end.
//   - The incoming Span is {start, start} (match begins at the current pos);
//     the returned Span.end is where consumption stopped (>= start).
//   - result_of<MatcherExpr> is void: it is a recognizer (filtered from
//     sequence results, like terminal), NOT a value source. Observation of
//     what it matched flows through on_match reading the node's span.
//
// Use it for matches that depend on runtime information not expressible as a
// static combinator tree — Lua long brackets/comments (count '=' and verify
// the close level matches), balanced delimiters, indentation-sensitive blocks.
// ---------------------------------------------------------------------------
template<typename Context, typename Fn>
struct MatcherExpr : ParsingExpr<Context, MatcherExpr<Context, Fn>>
{
    explicit MatcherExpr(Fn fn) : m_fn(std::move(fn)) {}

    typename Context::ParseResult parse(Context& context) const override
    {
        std::size_t start = context.mark();
        std::optional<Span> consumed = m_fn(context, Span{start, start});
        if (consumed) {
            // MatcherExpr owns the position advance — fn never touches it.
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
