// Operator DSL for building expression trees: >>, |, *, +, -, !, &, n*e.
// All parsing-expression types (Terminals, Combinators, Rule) derive from
// ParsingExpr<Context, Derived>, so the operators handle every operand
// uniformly via static_cast to the CRTP derived type.
//
// Rule (Grammar::operator[]) is a non-owning handle and is itself a
// ParsingExpr, so it participates naturally. Recursive grammars never form
// shared_ptr cycles because Rule stores a bare NonTerminal*.
#pragma once
#include <concepts>
#include <cstddef>

#include "Concepts.h"
#include "Parser.h"
namespace peg
{
using parsers::AlternationExpr;
using parsers::AndExpr;
using parsers::CutExpr;
using parsers::EmptyExpr;
using parsers::LexemeExpr;
using parsers::NotExpr;
using parsers::NTimesExpr;
using parsers::OneOrMoreExpr;
using parsers::OptionalExpr;
using parsers::ParsingExpr;
using parsers::SequenceExpr;
using parsers::TerminalExpr;
using parsers::TerminalSeqExpr;
using parsers::TokenExpr;
using parsers::ZeroOrMoreExpr;

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
    return expr >> TerminalExpr<Context, typename Context::value_type>(value);
}

template<typename Context, typename ParsingExprType>
auto operator>>(const typename Context::value_type& value,
                const ParsingExpr<Context, ParsingExprType>& expr)
{
    return TerminalExpr<Context, typename Context::value_type>(value) >> expr;
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
    return expr1 | TerminalExpr<Context, typename Context::value_type>(value);
}

template<typename Context, typename ParsingExprType>
auto operator|(const typename Context::value_type& value,
               const ParsingExpr<Context, ParsingExprType>& expr2)
{
    return TerminalExpr<Context, typename Context::value_type>(value) | expr2;
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

// NB: peglib overloads unary operator& as the and-predicate combinator. Use
// std::addressof to obtain a Rule*.
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
