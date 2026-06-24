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
        record_expected(context);
        return {false, nullptr};
    }

protected:
    TerminalValueType m_terminalValue;

private:
    // Record the expected item at the current position for error reporting.
    void record_expected(Context& context) const
    {
        std::size_t pos = context.mark();
        // Visit the terminal value to produce an ExpectedItem.text.
        // We handle shapes in order of specificity:
        //   1. Single value (char): print as 'x'
        //   2. Array of size 2 (range): print as 'a'..'z'
        //   3. Other container (set): print each element, joined
        //   4. Predicate/functor: generic placeholder
        if constexpr (std::is_same_v<TerminalValueType, typename Context::value_type>) {
            context.record_failure(pos,
                                   ExpectedItem{.kind = ExpectedKind::Literal,
                                                .text = escape_char_for_expected(m_terminalValue)});
        } else if constexpr (requires {
                                 std::get<0>(m_terminalValue);
                                 std::get<1>(m_terminalValue);
                             }) {
            // Array-of-2 / pair: range terminal. Produces "'lo'..'hi'".
            // Keep the element types intact so escape_char_for_expected can
            // render them correctly for any value_type (char, char32_t, ...).
            auto lo = std::get<0>(m_terminalValue);
            auto hi = std::get<1>(m_terminalValue);
            std::string text = escape_char_for_expected(lo) + ".." + escape_char_for_expected(hi);
            context.record_failure(
                pos, ExpectedItem{.kind = ExpectedKind::Range, .text = std::move(text)});
        } else if constexpr (requires {
                                 m_terminalValue.begin();
                                 m_terminalValue.end();
                             }) {
            // Set-like container: print as set of literals.
            std::string text;
            bool first = true;
            for (const auto& v : m_terminalValue) {
                if (!first)
                    text += ", ";
                first = false;
                text += escape_char_for_expected(v);
            }
            context.record_failure(
                pos, ExpectedItem{.kind = ExpectedKind::Range, .text = std::move(text)});
        } else {
            context.record_failure(
                pos, ExpectedItem{.kind = ExpectedKind::Literal, .text = "<terminal>"});
        }
    }
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
// result (Model A's value-bearing terminal).
//
// Match behaviour is identical to TerminalExpr (same symbolConsumable logic,
// same failure-recording), with one difference: on success TokenExpr builds a
// ParseTreeNode carrying the matched element in `node->token_value` (a
// parallel field of type `value_type`, distinct from `node->value` which is
// NodeType). The extractor reads `token_value` for TokenExpr slots, so the
// operator/element identity is visible to left-fold actions.
//
// Contrast with TerminalExpr (Model A's "void" terminal): TerminalExpr
// produces no node and is filtered out of sequence results, so structural
// tokens (parentheses, keywords, separators) never appear as action
// parameters. TokenExpr is for tokens whose identity the action needs.
// ---------------------------------------------------------------------------
template<typename Context, typename TerminalValueType>
struct TokenExpr : ParsingExpr<Context, TokenExpr<Context, TerminalValueType>>
{
    TokenExpr(TerminalValueType value) : m_terminalValue{std::move(value)} {}

    typename Context::ParseResult parse(Context& context) const override
    {
        if (!context.ended() && symbolConsumable(context.current(), m_terminalValue)) {
            auto matched = context.current();
            context.next();
            // Build a node carrying the matched element in token_value. We do
            // NOT touch node->value (that field is NodeType, owned by semantic
            // actions on NonTerminals). Keeping the two channels separate
            // preserves the value/transparent conventions unchanged.
            auto node = std::make_shared<typename Context::ParseTreeNode>();
            node->start_offset = context.mark() - 1;
            node->end_offset = context.mark();
            node->token_value = std::move(matched);
            return {true, node};
        }
        record_expected(context);
        return {false, nullptr};
    }

protected:
    TerminalValueType m_terminalValue;

private:
    // Record the expected item at the current position for error reporting.
    // Mirrors TerminalExpr::record_expected so error diagnostics are
    // indistinguishable between the two terminal forms.
    void record_expected(Context& context) const
    {
        std::size_t pos = context.mark();
        if constexpr (std::is_same_v<TerminalValueType, typename Context::value_type>) {
            context.record_failure(pos,
                                   ExpectedItem{.kind = ExpectedKind::Literal,
                                                .text = escape_char_for_expected(m_terminalValue)});
        } else if constexpr (requires {
                                 std::get<0>(m_terminalValue);
                                 std::get<1>(m_terminalValue);
                             }) {
            auto lo = std::get<0>(m_terminalValue);
            auto hi = std::get<1>(m_terminalValue);
            std::string text = escape_char_for_expected(lo) + ".." + escape_char_for_expected(hi);
            context.record_failure(
                pos, ExpectedItem{.kind = ExpectedKind::Range, .text = std::move(text)});
        } else if constexpr (requires {
                                 m_terminalValue.begin();
                                 m_terminalValue.end();
                             }) {
            std::string text;
            bool first = true;
            for (const auto& v : m_terminalValue) {
                if (!first)
                    text += ", ";
                first = false;
                text += escape_char_for_expected(v);
            }
            context.record_failure(
                pos, ExpectedItem{.kind = ExpectedKind::Range, .text = std::move(text)});
        } else {
            context.record_failure(
                pos, ExpectedItem{.kind = ExpectedKind::Literal, .text = "<token>"});
        }
    }
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
