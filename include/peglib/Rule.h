#pragma once
#include <concepts>

#include "Parser.h"
namespace peg
{
using parsers::AlternationExpr;
using parsers::AndExpr;
using parsers::CutExpr;
using parsers::EmptyExpr;
using parsers::NotExpr;
using parsers::NTimesExpr;
using parsers::OneOrMoreExpr;
using parsers::OptionalExpr;
using parsers::ParsingExpr;
using parsers::SequenceExpr;
using parsers::TerminalExpr;
using parsers::TerminalSeqExpr;
using parsers::ZeroOrMoreExpr;

template<typename elem>
auto terminal(const std::predicate<elem> auto& f)
{
    return TerminalExpr<Context<std::span<const elem>>, decltype(f)>(f);
}

template<typename elem>
auto terminal(elem value)
{
    return TerminalExpr<Context<std::span<const elem>>, elem>(value);
}

template<typename elem>
auto terminal(const std::set<elem>& values)
{
    return TerminalExpr<Context<std::span<const elem>>, std::set<elem>>(values);
}

template<typename elem>
auto terminal(const std::array<elem, 2>& values)
{
    return TerminalExpr<Context<std::span<const elem>>, std::array<elem, 2>>(values);
}

template<typename elem>
auto terminal(const elem& value_min, const elem& value_max)
{
    std::array<elem, 2> values = {value_min, value_max};
    return terminal(values);
}

template<typename SeqType>
auto terminalSeq(const SeqType& valueSeq)
{
    return TerminalSeqExpr<Context<std::span<const typename SeqType::value_type>>, SeqType>(
        valueSeq);
}

template<typename CharType>
auto terminalSeq(const CharType* str)
{
    return TerminalSeqExpr<Context<std::span<const CharType>>, std::basic_string<CharType>>(
        std::basic_string<CharType>{str});
}

template<typename C = Context<std::span<const std::string::value_type>>>
auto empty()
{
    return EmptyExpr<C>();
}

template<typename C = Context<std::span<const std::string::value_type>>>
auto cut()
{
    return CutExpr<C>();
}

// ---------------------------------------------------------------------------
// Operator overloads for building expression trees.
//
// All parsing expression types (Terminals, Combinators, and Rule) derive
// from ParsingExpr<Context, Derived>, so the operators below handle every
// operand uniformly via static_cast to the CRTP derived type.
//
// Rule (returned by Grammar::operator[]) is a non-owning handle and is
// itself a ParsingExpr, so it participates in operators naturally.
// Expression trees store Rule copies by value (~40 bytes each: a bare
// NonTerminal* + a copied std::string name). Since Rule is non-owning,
// recursive grammars never form shared_ptr cycles.
// ---------------------------------------------------------------------------

template<typename Context, typename ParsingExprType1, typename ParsingExprType2>
auto operator>>(const ParsingExpr<Context, ParsingExprType1>& expr1,
                const ParsingExpr<Context, ParsingExprType2>& expr2)
{
    return SequenceExpr<Context, ParsingExprType1, ParsingExprType2>(std::make_tuple(
        static_cast<const ParsingExprType1&>(expr1), static_cast<const ParsingExprType2&>(expr2)));
}

template<typename Context, typename ParsingExprType, typename... Children1>
auto operator>>(const SequenceExpr<Context, Children1...>& expr1,
                const ParsingExpr<Context, ParsingExprType>& expr2)
{
    return SequenceExpr<Context, Children1..., ParsingExprType>(
        std::tuple_cat(static_cast<const SequenceExpr<Context, Children1...>>(expr1).children(),
                       std::make_tuple(static_cast<const ParsingExprType&>(expr2))));
}

template<typename Context, typename ParsingExprType, typename... Children2>
auto operator>>(const ParsingExpr<Context, ParsingExprType>& expr1,
                const SequenceExpr<Context, Children2...>& expr2)
{
    return SequenceExpr<Context, ParsingExprType, Children2...>(
        std::tuple_cat(std::make_tuple(static_cast<const ParsingExprType&>(expr1)),
                       static_cast<const SequenceExpr<Context, Children2...>>(expr2).children()));
}

template<typename Context, typename... Children1, typename... Children2>
auto operator>>(const SequenceExpr<Context, Children1...>& expr1,
                const SequenceExpr<Context, Children2...>& expr2)
{
    return SequenceExpr<Context, Children1..., Children2...>(
        std::tuple_cat(static_cast<const SequenceExpr<Context, Children1...>>(expr1).children(),
                       static_cast<const SequenceExpr<Context, Children2...>>(expr2).children()));
}

template<typename Context, typename ParsingExprType>
auto operator>>(const ParsingExpr<Context, ParsingExprType>& expr,
                const typename Context::value_type& value)
{
    return expr >> terminal(value);
}

template<typename Context, typename ParsingExprType>
auto operator>>(const typename Context::value_type& value,
                const ParsingExpr<Context, ParsingExprType>& expr)
{
    return terminal(value) >> expr;
}

template<typename Context, typename ParsingExprType1, typename ParsingExprType2>
auto operator|(const ParsingExpr<Context, ParsingExprType1>& expr1,
               const ParsingExpr<Context, ParsingExprType2>& expr2)
{
    return AlternationExpr<Context, ParsingExprType1, ParsingExprType2>(std::make_tuple(
        static_cast<const ParsingExprType1&>(expr1), static_cast<const ParsingExprType2&>(expr2)));
}

template<typename Context, typename ParsingExprType, typename... Children1>
auto operator|(const AlternationExpr<Context, Children1...>& expr1,
               const ParsingExpr<Context, ParsingExprType>& expr2)
{
    return AlternationExpr<Context, Children1..., ParsingExprType>(
        std::tuple_cat(static_cast<const AlternationExpr<Context, Children1...>>(expr1).children(),
                       std::make_tuple(static_cast<const ParsingExprType&>(expr2))));
}

template<typename Context, typename ParsingExprType, typename... Children2>
auto operator|(const ParsingExpr<Context, ParsingExprType>& expr1,
               const AlternationExpr<Context, Children2...>& expr2)
{
    return AlternationExpr<Context, ParsingExprType, Children2...>(std::tuple_cat(
        std::make_tuple(static_cast<const ParsingExprType&>(expr1)),
        static_cast<const AlternationExpr<Context, Children2...>>(expr2).children()));
}

template<typename Context, typename... Children1, typename... Children2>
auto operator|(const AlternationExpr<Context, Children1...>& expr1,
               const AlternationExpr<Context, Children2...>& expr2)
{
    return AlternationExpr<Context, Children1..., Children2...>(std::tuple_cat(
        static_cast<const AlternationExpr<Context, Children1...>>(expr1).children(),
        static_cast<const AlternationExpr<Context, Children2...>>(expr2).children()));
}

template<typename Context, typename ParsingExprType>
auto operator|(const ParsingExpr<Context, ParsingExprType>& expr1,
               const typename Context::value_type& value)
{
    return expr1 | terminal(value);
}

template<typename Context, typename ParsingExprType>
auto operator|(const typename Context::value_type& value,
               const ParsingExpr<Context, ParsingExprType>& expr2)
{
    return terminal(value) | expr2;
}

template<typename Context, typename ParsingExprType>
auto operator*(const ParsingExpr<Context, ParsingExprType>& expr)
{
    return ZeroOrMoreExpr<Context, ParsingExprType>(static_cast<const ParsingExprType&>(expr));
}

template<typename Context, typename ParsingExprType>
auto operator*(const ZeroOrMoreExpr<Context, ParsingExprType>& n)
{
    return n;
}

template<typename Context, typename ParsingExprType>
auto operator*(size_t n_rep, const ParsingExpr<Context, ParsingExprType>& expr)
{
    return NTimesExpr<Context, ParsingExprType>(static_cast<const ParsingExprType&>(expr), n_rep);
}

template<typename Context, typename ParsingExprType>
auto operator+(const ParsingExpr<Context, ParsingExprType>& expr)
{
    return OneOrMoreExpr<Context, ParsingExprType>(static_cast<const ParsingExprType&>(expr));
}

template<typename Context, typename ParsingExprType>
auto operator+(const OneOrMoreExpr<Context, ParsingExprType>& n)
{
    return n;
}

template<typename Context, typename ParsingExprType>
auto operator-(const ParsingExpr<Context, ParsingExprType>& expr)
{
    return OptionalExpr<Context, ParsingExprType>(static_cast<const ParsingExprType&>(expr));
}

template<typename Context, typename ParsingExprType>
auto operator-(const OptionalExpr<Context, ParsingExprType>& n)
{
    return n;
}

template<typename Context, typename ParsingExprType>
auto operator!(const ParsingExpr<Context, ParsingExprType>& expr)
{
    return NotExpr<Context, ParsingExprType>(static_cast<const ParsingExprType&>(expr));
}

template<typename Context, typename ParsingExprType>
auto operator!(const NotExpr<Context, ParsingExprType>& n)
{
    return n.child();
}

template<typename Context, typename ParsingExprType>
auto operator&(const ParsingExpr<Context, ParsingExprType>& expr)
{
    return AndExpr<Context, ParsingExprType>(static_cast<const ParsingExprType&>(expr));
}

template<typename Context, typename ParsingExprType>
auto operator&(const AndExpr<Context, ParsingExprType>& n)
{
    return n.child();
}

} // namespace peg
