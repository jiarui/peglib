#pragma once
#include "Parser.h"
#include <concepts>
namespace peg
{
    using namespace parsers;

    template<typename C=Context<std::span<const std::string::value_type>>>
    using Rule = typename C::Rule;

    template <typename elem>
    auto terminal(const std::predicate<elem> auto& f) {
        return TerminalExpr<Context<std::span<const elem>>, decltype(f)>(f);
    }

    template <typename elem>
    auto terminal(elem value) {
        return TerminalExpr<Context<std::span<const elem>>, elem>(value);
    }



    template <typename elem>
    auto terminal(const std::set<elem>& values) {
        return TerminalExpr<Context<std::span<const elem>>, std::set<elem>>(values);
    }

    template <typename elem>
    auto terminal(const std::array<elem, 2>& values) {
        return TerminalExpr<Context<std::span<const elem>>, std::array<elem, 2>>(values);
    }

    template <typename elem>
    auto terminal(const elem& value_min, const elem& value_max) {
        std::array<elem, 2> values = {value_min, value_max};
        return terminal(values);
    }

    template<typename SeqType>
    auto terminalSeq(const SeqType& valueSeq){
        return TerminalSeqExpr<Context<std::span<const typename SeqType::value_type>>, SeqType>(valueSeq);
    }

    template<typename CharType>
    auto terminalSeq(const CharType* str){
        return TerminalSeqExpr<Context<std::span<const CharType>>, std::basic_string<CharType>>(std::basic_string<CharType>{str});
    }

    template<typename Context>
    auto emtpy() {
        return EmptyExpr<Context>();
    }

    template<typename Context>
    auto cut() {
        return CutExpr<Context>();
    }

    template<typename Context, typename ParsingExprType>
    auto
    self(const ParsingExpr<Context, ParsingExprType>& expr) {
        if constexpr (std::is_same_v<ParsingExprType, NonTerminal<Context>>) {
            return NonTerminalRef<Context>(static_cast<const ParsingExprType&>(expr));
        }
        else {
            return static_cast<const ParsingExprType&>(expr);
        }
    }

    template<typename Context, typename ParsingExprType>
    auto
    expr_children(const ParsingExpr<Context, ParsingExprType>& expr){
        return static_cast<const ParsingExprType&>(expr).children();
    }

    template <typename Context, typename ParsingExprType1, typename ParsingExprType2>
    auto
    operator>>(const ParsingExpr<Context, ParsingExprType1>& expr1,  const ParsingExpr<Context, ParsingExprType2>& expr2){
        auto lhs = self(expr1);
        auto rhs = self(expr2);
        return SequenceExpr<Context,
                            decltype(lhs), 
                            decltype(rhs)>(
                                std::make_tuple(
                                    lhs, 
                                    rhs)
                            );
    }

    template<typename Context, typename ParsingExprType, typename ...Children1>
    auto
    operator>>(const SequenceExpr<Context, Children1...>& expr1, const ParsingExpr<Context, ParsingExprType>& expr2){
        auto rhs = self(expr2);
        return SequenceExpr<Context, Children1..., decltype(rhs)>(
                                std::tuple_cat(
                                    static_cast<const SequenceExpr<Context, Children1...>>(expr1).children(),
                                    std::make_tuple(rhs)
                                )
                            );
    }

    template<typename Context, typename ParsingExprType, typename ...Children2>
    auto
    operator>>(const ParsingExpr<Context, ParsingExprType>& expr1, const SequenceExpr<Context, Children2...>& expr2){
        auto lhs = self(expr1);
        return SequenceExpr<Context, decltype(lhs), Children2...>(
                                std::tuple_cat(
                                    std::make_tuple(lhs),
                                    static_cast<const SequenceExpr<Context, Children2...>>(expr2).children()
                                )
                            );
    }

    template<typename Context, typename ...Children1, typename ...Children2>
    auto
    operator>>(const SequenceExpr<Context, Children1...>& expr1, const SequenceExpr<Context, Children2...>& expr2){
        return SequenceExpr<Context, Children1..., Children2...>(
                                std::tuple_cat(
                                    static_cast<const SequenceExpr<Context, Children1...>>(expr1).children(),
                                    static_cast<const SequenceExpr<Context, Children2...>>(expr2).children()
                                )
                            );
    }

    template<typename Context, typename ParsingExprType>
    auto operator>>(const ParsingExpr<Context, ParsingExprType>& expr, const typename Context::value_type& value) {
        auto lhs = self(expr);
        return lhs >> terminal(value);
    }

    template<typename Context, typename ParsingExprType>
    auto operator>>(const typename Context::value_type& value, const ParsingExpr<Context, ParsingExprType>& expr) {
        auto rhs = self(expr);
        return terminal(value) >> rhs;
    }

    template <typename Context, typename ParsingExprType1, typename ParsingExprType2>
    auto
    operator|(const ParsingExpr<Context, ParsingExprType1>& expr1,  const ParsingExpr<Context, ParsingExprType2>& expr2){
        auto lhs = self(expr1);
        auto rhs = self(expr2);
        return AlternationExpr<Context,
                            decltype(lhs), 
                            decltype(rhs)>(
                                std::make_tuple(
                                    lhs, 
                                    rhs));
    }

    template<typename Context, typename ParsingExprType, typename ...Children1>
    auto
    operator|(const AlternationExpr<Context, Children1...>& expr1, const ParsingExpr<Context, ParsingExprType>& expr2){
        auto rhs = self(expr2);
        return AlternationExpr<Context, Children1..., decltype(rhs)>(
                                std::tuple_cat(
                                    static_cast<const AlternationExpr<Context, Children1...>>(expr1).children(),
                                    std::make_tuple(rhs)
                                )
                            );
    }

    template<typename Context, typename ParsingExprType, typename ...Children2>
    auto
    operator|(const ParsingExpr<Context, ParsingExprType>& expr1, const AlternationExpr<Context, Children2...>& expr2){
        auto lhs = self(expr1);
        return AlternationExpr<Context, typename ParsingExprType::ParseExprType, Children2...>(
                                std::tuple_cat(
                                    std::make_tuple(lhs),
                                    static_cast<const AlternationExpr<Context, Children2...>>(expr2).children()
                                )
                            );
    }

    template<typename Context, typename ...Children1, typename ...Children2>
    auto
    operator|(const AlternationExpr<Context, Children1...>& expr1, const AlternationExpr<Context, Children2...>& expr2){
        return AlternationExpr<Context, Children1..., Children2...>(
                                std::tuple_cat(
                                    static_cast<const AlternationExpr<Context, Children1...>>(expr1).children(),
                                    static_cast<const AlternationExpr<Context, Children2...>>(expr2).children()
                                )
                            );
    }

    template<typename Context, typename ParsingExprType>
    auto
    operator|(const ParsingExpr<Context, ParsingExprType>& expr1, const typename Context::value_type& value) {
        auto lhs = self(expr1);
        return lhs | terminal(value);
    }

    template<typename Context, typename ParsingExprType>
    auto
    operator|(const typename Context::value_type& value, const ParsingExpr<Context, ParsingExprType>& expr2) {
        auto rhs = self(expr2);
        return terminal(value) | rhs;
    }

    template<typename Context, typename ParsingExprType>
    auto operator*(const ParsingExpr<Context, ParsingExprType>& expr) {
        auto n = self(expr);
        return ZeroOrMoreExpr<Context, decltype(n)>(n);
    }

    template<typename Context, typename ParsingExprType>
    auto operator*(const ZeroOrMoreExpr<Context, ParsingExprType>& n) {
        return n;
    }

    template<typename Context, typename ParsingExprType>
    auto operator*(size_t n_rep, const ParsingExpr<Context, ParsingExprType>& expr) {
        auto n = self(expr);
        return NTimesExpr<Context, decltype(n)>(n, n_rep);
    }

    template<typename Context, typename ParsingExprType>
    auto operator+(const ParsingExpr<Context, ParsingExprType>& expr) {
        auto n = self(expr);
        return OneOrMoreExpr<Context, decltype(n)>(n);
    }

    template<typename Context, typename ParsingExprType>
    auto operator+(const OneOrMoreExpr<Context, ParsingExprType>& n) {
        return n;
    }

    template<typename Context, typename ParsingExprType>
    auto operator-(const ParsingExpr<Context, ParsingExprType>& expr){
        auto n = self(expr);
        return OptionalExpr<Context, decltype(n)>(n);
    }

    template<typename Context, typename ParsingExprType>
    auto operator-(const OptionalExpr<Context, ParsingExprType>& n){
        return n;
    }

    template<typename Context, typename ParsingExprType>
    auto operator!(const ParsingExpr<Context, ParsingExprType>& expr) {
        auto n = self(expr);
        return NotExpr<Context, decltype(n)>(n);
    }

    template<typename Context, typename ParsingExprType>
    auto operator!(const NotExpr<Context, ParsingExprType>& n) {
        return n.child();
    }

    template<typename Context, typename ParsingExprType>
    auto operator&(const ParsingExpr<Context, ParsingExprType>& expr) {
        auto n = self(expr);
        return AndExpr<Context, decltype(n)>(n);
    }

    template<typename Context, typename ParsingExprType>
    auto operator&(const AndExpr<Context, ParsingExprType>& n) {
        return n.child();
    }

} // namespace peg
