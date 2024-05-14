#pragma once
#include "Context.h"
#include "Parser.h"
#include <memory>
namespace peg
{
    using namespace parsers;
    template<typename elem>
    using Rule = NonTerminal<elem>;

    template <typename elem>
    TerminalExpr<elem> terminal(elem value) {
        std::cout<<"termvalue "<<value<<std::endl;
        return TerminalExpr<elem>(value);
    }

    template<typename elem>
    EmptyExpr<elem> emtpy() {
        return EmptyExpr<elem>();
    }

    template <typename elem, typename ParsingExprType1, typename ParsingExprType2>
    auto
    operator>>(const ParsingExpr<elem, ParsingExprType1>& expr1,  const ParsingExpr<elem, ParsingExprType2>& expr2){
        return SequenceExpr<elem,
                            typename ParsingExprType1::ParseExprType, 
                            typename ParsingExprType2::ParseExprType>(
                                std::make_tuple(
                                    static_cast<const typename ParsingExprType1::ParseExprType&>(expr1), 
                                    static_cast<const typename ParsingExprType2::ParseExprType&>(expr2)));
    }

    template<typename elem, typename ParsingExprType, typename ...Children1>
    auto
    operator>>(const SequenceExpr<elem, Children1...>& expr1, const ParsingExpr<elem, ParsingExprType>& expr2){
        return SequenceExpr<elem, Children1..., typename ParsingExprType::ParseExprType>(
                                std::tuple_cat(
                                    static_cast<const SequenceExpr<elem, Children1...>>(expr1).children(),
                                    std::make_tuple(static_cast<const typename ParsingExprType::ParseExprType&>(expr2))
                                )
                            );
    }

    template<typename elem, typename ParsingExprType, typename ...Children2>
    auto
    operator>>(const ParsingExpr<elem, ParsingExprType>& expr1, const SequenceExpr<elem, Children2...>& expr2){
        return SequenceExpr<elem, typename ParsingExprType::ParseExprType, Children2...>(
                                std::tuple_cat(
                                    std::make_tuple(static_cast<const typename ParsingExprType::ParseExprType&>(expr1)),
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

    template<typename elem>
    auto operator>>(const NonTerminal<elem>& lhs, const NonTerminal<elem>& rhs) {
        return NonTerminalRef<elem>(lhs) >> NonTerminalRef(rhs);
    }

    template<typename elem, typename ParsingExprType>
    auto operator>>(const NonTerminal<elem>& lhs, const ParsingExpr<elem, ParsingExprType>& rhs) {
        return NonTerminalRef<elem>(lhs) >> rhs;
    }

    template<typename elem, typename ParsingExprType>
    auto operator>>(const ParsingExpr<elem, ParsingExprType>& lhs, const NonTerminal<elem>& rhs) {
        return lhs >> NonTerminalRef<elem>(rhs);
    }

    template<typename elem>
    auto operator>>(const NonTerminal<elem>& lhs, const elem& value) {
        return NonTerminalRef<elem>(lhs) >> terminal(value);
    }

    template<typename elem>
    auto operator>>(const elem& value, const NonTerminal<elem>& rhs) {
        return terminal(value) >> NonTerminalRef<elem>(rhs);
    }

    template <typename elem, typename ParsingExprType1, typename ParsingExprType2>
    auto
    operator|(const ParsingExpr<elem, ParsingExprType1>& expr1,  const ParsingExpr<elem, ParsingExprType2>& expr2){
        return AlternationExpr<elem,
                            typename ParsingExprType1::ParseExprType, 
                            typename ParsingExprType2::ParseExprType>(
                                std::make_tuple(
                                    static_cast<const typename ParsingExprType1::ParseExprType&>(expr1), 
                                    static_cast<const typename ParsingExprType2::ParseExprType&>(expr2)));
    }

    template<typename elem, typename ParsingExprType, typename ...Children1>
    auto
    operator|(const AlternationExpr<elem, Children1...>& expr1, const ParsingExpr<elem, ParsingExprType>& expr2){
        return AlternationExpr<elem, Children1..., typename ParsingExprType::ParseExprType>(
                                std::tuple_cat(
                                    static_cast<const AlternationExpr<elem, Children1...>>(expr1).children(),
                                    std::make_tuple(static_cast<const typename ParsingExprType::ParseExprType&>(expr2))
                                )
                            );
    }

    template<typename elem, typename ParsingExprType, typename ...Children2>
    auto
    operator|(const ParsingExpr<elem, ParsingExprType>& expr1, const AlternationExpr<elem, Children2...>& expr2){
        return AlternationExpr<elem, typename ParsingExprType::ParseExprType, Children2...>(
                                std::tuple_cat(
                                    std::make_tuple(static_cast<const typename ParsingExprType::ParseExprType&>(expr1)),
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
        return static_cast<const ParsingExprType&>(expr1) | terminal(value);
    }

    template<typename elem, typename ParsingExprType>
    auto
    operator|(const elem& value, const ParsingExpr<elem, ParsingExprType>& expr2) {
        return terminal(value) | static_cast<const ParsingExprType&>(expr2);
    }

    template<typename elem, typename ParsingExprType>
    auto operator*(const ParsingExpr<elem, ParsingExprType>& n) {
        return Repetition<elem, ParsingExprType, 0>(static_cast<const ParsingExprType&>(n));
    }

    template<typename elem, typename ParsingExprType>
    auto operator*(const Repetition<elem, ParsingExprType, 0, -1>& n) {
        return n;
    }

    template<typename elem, typename ParsingExprType>
    auto operator+(const ParsingExpr<elem, ParsingExprType>& n) {
        return Repetition<elem, ParsingExprType, 1>(static_cast<const ParsingExprType&>(n));
    }

    template<typename elem, typename ParsingExprType>
    auto operator+(const Repetition<elem, ParsingExprType, 1, -1>& n) {
        return n;
    }

    template<typename elem, typename ParsingExprType>
    auto operator-(const ParsingExpr<elem, ParsingExprType>& n){
        return Repetition<elem, ParsingExprType, 0, 1>(static_cast<const ParsingExprType&>(n));
    }

    template<typename elem, typename ParsingExprType>
    auto operator-(const Repetition<elem, ParsingExprType, 0, 1>& n){
        return n;
    }

    template<typename elem, typename ParsingExprType>
    auto operator!(const ParsingExpr<elem, ParsingExprType>& n) {
        return NotExpr<elem, ParsingExprType>(static_cast<const ParsingExprType&>(n));
    }

    template<typename elem, typename ParsingExprType>
    auto operator!(const NotExpr<elem, ParsingExprType>& n) {
        return n.child();
    }

    template<typename elem, typename ParsingExprType>
    auto operator&(const ParsingExpr<elem, ParsingExprType>& n) {
        return AndExpr<elem, ParsingExprType>(static_cast<const ParsingExprType&>(n));
    }

    template<typename elem, typename ParsingExprType>
    auto operator&(const AndExpr<elem, ParsingExprType>& n) {
        return n.child();
    }



} // namespace peg
