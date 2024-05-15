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
        return TerminalExpr<elem>(value);
    }

    template <typename elem>
    TerminalExpr<elem, std::set<elem>> terminalSet(const std::set<elem>& values) {
        return TerminalExpr<elem, std::set<elem>>(values);
    }

    template <typename elem>
    TerminalExpr<elem, std::array<elem, 2>> terminalRange(const std::array<elem, 2>& values) {
        return TerminalExpr<elem, std::array<elem, 2>>(values);
    }

    template <typename elem>
    TerminalExpr<elem, std::array<elem,2>> terminalRange(const elem& value_min, const elem& value_max) {
        std::array<elem, 2> values = {value_min, value_max};
        return terminalRange(values);
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

    template<typename elem, typename ParsingExprType>
    auto operator>>(const ParsingExpr<elem, ParsingExprType>& lhs, const elem& value) {
        return static_cast<const ParsingExprType&>(lhs) >> terminal(value);
    }

    template<typename elem, typename ParsingExprType>
    auto operator>>(const elem& value, const ParsingExpr<elem, ParsingExprType>& rhs) {
        return terminal(value) >> static_cast<const ParsingExprType&>(rhs);
    }

    template<typename elem, typename ParsingExprType>
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
    auto
    operator|(const ParsingExpr<elem, ParsingExprType>& lhs, const NonTerminal<elem>& rhs) {
        return static_cast<const ParsingExprType&>(lhs) | NonTerminalRef<elem>(rhs);
    }

    template<typename elem, typename ParsingExprType>
    auto
    operator|(const NonTerminal<elem>& lhs, const ParsingExpr<elem, ParsingExprType>& rhs) {
        return NonTerminalRef<elem>(lhs) | static_cast<const ParsingExprType&>(rhs);
    }

    template<typename elem, typename ParsingExprType>
    auto operator*(const ParsingExpr<elem, ParsingExprType>& n) {
        return ZeroOrMoreExpr<elem, ParsingExprType>(static_cast<const ParsingExprType&>(n));
    }

    template<typename elem, typename ParsingExprType>
    auto operator*(const ZeroOrMoreExpr<elem, ParsingExprType>& n) {
        return n;
    }

    template<typename elem, typename ParsingExprType>
    auto operator*(size_t n_rep, const ParsingExpr<elem, ParsingExprType>& expr) {
        return NTimesExpr<elem, ParsingExprType>(static_cast<const ParsingExprType&>(expr), n_rep);
    }

    template<typename elem>
    auto operator*(const NonTerminal<elem> &expr) {
        return NonTerminalRef<elem>(expr);
    }

    template<typename elem>
    auto operator*(size_t n_rep, const NonTerminal<elem> &expr) {
        return n_rep * NonTerminalRef<elem>(expr);
    }



    template<typename elem, typename ParsingExprType>
    auto operator+(const ParsingExpr<elem, ParsingExprType>& n) {
        return OneOrMoreExpr<elem, ParsingExprType>(static_cast<const ParsingExprType&>(n));
    }

    template<typename elem, typename ParsingExprType>
    auto operator+(const OneOrMoreExpr<elem, ParsingExprType>& n) {
        return n;
    }

    template<typename elem, typename ParsingExprType>
    auto operator+(const NonTerminal<elem>& n) {
        return +NonTerminalRef(n);
    }

    template<typename elem, typename ParsingExprType>
    auto operator-(const ParsingExpr<elem, ParsingExprType>& n){
        return OptionalExpr<elem, ParsingExprType>(static_cast<const ParsingExprType&>(n));
    }

    template<typename elem, typename ParsingExprType>
    auto operator-(const OptionalExpr<elem, ParsingExprType>& n){
        return n;
    }

    template<typename elem, typename ParsingExprType>
    auto operator-(const NonTerminal<elem>& n) {
        return -NonTerminalRef(n);
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
    auto operator!(const NonTerminal<elem>& n) {
        return !NonTerminalRef(n);
    }

    template<typename elem, typename ParsingExprType>
    auto operator&(const ParsingExpr<elem, ParsingExprType>& n) {
        return AndExpr<elem, ParsingExprType>(static_cast<const ParsingExprType&>(n));
    }

    template<typename elem, typename ParsingExprType>
    auto operator&(const AndExpr<elem, ParsingExprType>& n) {
        return n.child();
    }

    template<typename elem, typename ParsingExprType>
    auto operator&(const NonTerminal<elem>& n) {
        return &NonTerminalRef(n);
    }



} // namespace peg
