#pragma once
#include "Context.h"
#include "Parser.h"
namespace peg
{
    using namespace parsers;
    template<typename elem>
    using Rule = NonTerminal<elem>;

    template <typename elem>
    TerminalExpr<elem> terminal(elem value) {
        return TerminalExpr<elem>(value);
    }

    template <typename elem>
    TerminalExpr<elem, std::set<elem>> terminal(const std::set<elem>& values) {
        return TerminalExpr<elem, std::set<elem>>(values);
    }

    template <typename elem>
    TerminalExpr<elem, std::array<elem, 2>> terminal(const std::array<elem, 2>& values) {
        return TerminalExpr<elem, std::array<elem, 2>>(values);
    }

    template <typename elem>
    TerminalExpr<elem, std::array<elem,2>> terminal(const elem& value_min, const elem& value_max) {
        std::array<elem, 2> values = {value_min, value_max};
        return terminal(values);
    }

    template<typename SeqType>
    TerminalSeqExpr<typename SeqType::value_type, SeqType> terminalSeq(const SeqType& valueSeq){
        return TerminalSeqExpr<typename SeqType::value_type, SeqType>(valueSeq);
    }

    template<typename CharType>
    TerminalSeqExpr<CharType, std::string> terminalSeq(const CharType* str){
        return TerminalSeqExpr<CharType, std::basic_string<CharType>>(std::basic_string<CharType>{str});
    }

    template<typename elem>
    EmptyExpr<elem> emtpy() {
        return EmptyExpr<elem>();
    }

    template<typename elem, typename ParsingExprType>
    auto
    self(const ParsingExpr<elem, ParsingExprType>& expr) {
        if constexpr (std::is_same_v<ParsingExprType, NonTerminal<elem>>) {
            return NonTerminalRef<elem>(static_cast<const ParsingExprType&>(expr));
        }
        else {
            return static_cast<const ParsingExprType&>(expr);
        }
    }

    template<typename elem, typename ParsingExprType>
    auto
    expr_children(const ParsingExpr<elem, ParsingExprType>& expr){
        return static_cast<const ParsingExprType&>(expr).children();
    }

    template <typename elem, typename ParsingExprType1, typename ParsingExprType2>
    auto
    operator>>(const ParsingExpr<elem, ParsingExprType1>& expr1,  const ParsingExpr<elem, ParsingExprType2>& expr2){
        auto lhs = self(expr1);
        auto rhs = self(expr2);
        return SequenceExpr<elem,
                            decltype(lhs), 
                            decltype(rhs)>(
                                std::make_tuple(
                                    lhs, 
                                    rhs)
                            );
    }

    template<typename elem, typename ParsingExprType, typename ...Children1>
    auto
    operator>>(const SequenceExpr<elem, Children1...>& expr1, const ParsingExpr<elem, ParsingExprType>& expr2){
        auto rhs = self(expr2);
        return SequenceExpr<elem, Children1..., decltype(rhs)>(
                                std::tuple_cat(
                                    static_cast<const SequenceExpr<elem, Children1...>>(expr1).children(),
                                    std::make_tuple(rhs)
                                )
                            );
    }

    template<typename elem, typename ParsingExprType, typename ...Children2>
    auto
    operator>>(const ParsingExpr<elem, ParsingExprType>& expr1, const SequenceExpr<elem, Children2...>& expr2){
        auto lhs = self(expr1);
        return SequenceExpr<elem, decltype(lhs), Children2...>(
                                std::tuple_cat(
                                    std::make_tuple(lhs),
                                    static_cast<const SequenceExpr<elem, Children2...>>(expr2).children()
                                )
                            );
    }

    template<typename elem, typename ...Children1, typename ...Children2>
    auto
    operator>>(const SequenceExpr<elem, Children1...>& expr1, const SequenceExpr<elem, Children2...>& expr2){
        return SequenceExpr<elem, Children1..., Children2...>(
                                std::tuple_cat(
                                    static_cast<const SequenceExpr<elem, Children1...>>(expr1).children(),
                                    static_cast<const SequenceExpr<elem, Children2...>>(expr2).children()
                                )
                            );
    }

    template<typename elem, typename ParsingExprType>
    auto operator>>(const ParsingExpr<elem, ParsingExprType>& expr, const elem& value) {
        auto lhs = self(expr);
        return lhs >> terminal(value);
    }

    template<typename elem, typename ParsingExprType>
    auto operator>>(const elem& value, const ParsingExpr<elem, ParsingExprType>& expr) {
        auto rhs = self(expr);
        return terminal(value) >> rhs;
    }

    template <typename elem, typename ParsingExprType1, typename ParsingExprType2>
    auto
    operator|(const ParsingExpr<elem, ParsingExprType1>& expr1,  const ParsingExpr<elem, ParsingExprType2>& expr2){
        auto lhs = self(expr1);
        auto rhs = self(expr2);
        return AlternationExpr<elem,
                            decltype(lhs), 
                            decltype(rhs)>(
                                std::make_tuple(
                                    lhs, 
                                    rhs));
    }

    template<typename elem, typename ParsingExprType, typename ...Children1>
    auto
    operator|(const AlternationExpr<elem, Children1...>& expr1, const ParsingExpr<elem, ParsingExprType>& expr2){
        auto rhs = self(expr2);
        return AlternationExpr<elem, Children1..., decltype(rhs)>(
                                std::tuple_cat(
                                    static_cast<const AlternationExpr<elem, Children1...>>(expr1).children(),
                                    std::make_tuple(rhs)
                                )
                            );
    }

    template<typename elem, typename ParsingExprType, typename ...Children2>
    auto
    operator|(const ParsingExpr<elem, ParsingExprType>& expr1, const AlternationExpr<elem, Children2...>& expr2){
        auto lhs = self(expr1);
        return AlternationExpr<elem, typename ParsingExprType::ParseExprType, Children2...>(
                                std::tuple_cat(
                                    std::make_tuple(lhs),
                                    static_cast<const AlternationExpr<elem, Children2...>>(expr2).children()
                                )
                            );
    }

    template<typename elem, typename ...Children1, typename ...Children2>
    auto
    operator|(const AlternationExpr<elem, Children1...>& expr1, const AlternationExpr<elem, Children2...>& expr2){
        return AlternationExpr<elem, Children1..., Children2...>(
                                std::tuple_cat(
                                    static_cast<const AlternationExpr<elem, Children1...>>(expr1).children(),
                                    static_cast<const AlternationExpr<elem, Children2...>>(expr2).children()
                                )
                            );
    }

    template<typename elem, typename ParsingExprType>
    auto
    operator|(const ParsingExpr<elem, ParsingExprType>& expr1, const elem& value) {
        auto lhs = self(expr1);
        return lhs | terminal(value);
    }

    template<typename elem, typename ParsingExprType>
    auto
    operator|(const elem& value, const ParsingExpr<elem, ParsingExprType>& expr2) {
        auto rhs = self(expr2);
        return terminal(value) | rhs;
    }

    template<typename elem, typename ParsingExprType>
    auto operator*(const ParsingExpr<elem, ParsingExprType>& expr) {
        auto n = self(expr);
        return ZeroOrMoreExpr<elem, decltype(n)>(n);
    }

    template<typename elem, typename ParsingExprType>
    auto operator*(const ZeroOrMoreExpr<elem, ParsingExprType>& n) {
        return n;
    }

    template<typename elem, typename ParsingExprType>
    auto operator*(size_t n_rep, const ParsingExpr<elem, ParsingExprType>& expr) {
        auto n = self(expr);
        return NTimesExpr<elem, decltype(n)>(n, n_rep);
    }

    template<typename elem, typename ParsingExprType>
    auto operator+(const ParsingExpr<elem, ParsingExprType>& expr) {
        auto n = self(expr);
        return OneOrMoreExpr<elem, decltype(n)>(n);
    }

    template<typename elem, typename ParsingExprType>
    auto operator+(const OneOrMoreExpr<elem, ParsingExprType>& n) {
        return n;
    }

    template<typename elem, typename ParsingExprType>
    auto operator-(const ParsingExpr<elem, ParsingExprType>& expr){
        auto n = self(expr);
        return OptionalExpr<elem, decltype(n)>(n);
    }

    template<typename elem, typename ParsingExprType>
    auto operator-(const OptionalExpr<elem, ParsingExprType>& n){
        return n;
    }

    template<typename elem, typename ParsingExprType>
    auto operator!(const ParsingExpr<elem, ParsingExprType>& expr) {
        auto n = self(expr);
        return NotExpr<elem, decltype(n)>(n);
    }

    template<typename elem, typename ParsingExprType>
    auto operator!(const NotExpr<elem, ParsingExprType>& n) {
        return n.child();
    }

    template<typename elem, typename ParsingExprType>
    auto operator&(const ParsingExpr<elem, ParsingExprType>& expr) {
        auto n = self(expr);
        return AndExpr<elem, decltype(n)>(n);
    }

    template<typename elem, typename ParsingExprType>
    auto operator&(const AndExpr<elem, ParsingExprType>& n) {
        return n.child();
    }

    template <typename elem, typename ParsingExprType, typename MatchType>
    auto operator==(const ParsingExpr<elem, ParsingExprType>& expr, const MatchType& match_id) {
        return MatchExpr<elem, ParsingExprType, MatchType>(static_cast<const ParsingExprType&>(expr), match_id);
    }

    template <typename elem, typename ParsingExprType, typename CharType>
    auto operator==(const ParsingExpr<elem, ParsingExprType>& expr, const CharType* match_id) {
        return static_cast<const ParsingExprType&>(expr) == std::basic_string<CharType>{match_id};
    }



} // namespace peg
