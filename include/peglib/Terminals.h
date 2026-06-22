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
    TerminalExpr(const TerminalValueType& value, SemanticAction action = nullptr)
        : ParsingExpr<Context, TerminalExpr<Context, TerminalValueType>>(action),
          m_terminalValue{value}
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
            char lo = static_cast<char>(std::get<0>(m_terminalValue));
            char hi = static_cast<char>(std::get<1>(m_terminalValue));
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
                text += escape_char_for_expected(static_cast<char>(v));
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
    TerminalSeqExpr(const SeqType& value) : m_terminalValues{value} {}
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
        // Build a printable form of the sequence. We use std::string to
        // accumulate since SeqType is typically std::basic_string<char>.
        std::string text;
        for (const auto& v : m_terminalValues) {
            text += static_cast<char>(v);
        }
        context.record_failure(
            pos,
            ExpectedItem{.kind = ExpectedKind::Literal, .text = escape_string_for_expected(text)});
    }
};

// ---------------------------------------------------------------------------
// EmptyExpr: always succeeds, consumes nothing.
// ---------------------------------------------------------------------------
template<typename Context>
struct EmptyExpr : ParsingExpr<Context, EmptyExpr<Context>>
{
    EmptyExpr() {}
    typename Context::ParseResult parse(Context& /*context*/) const override
    {
        return {true, nullptr};
    }
};

} // namespace parsers
} // namespace peg
