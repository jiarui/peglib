#pragma once
#include "Context.h"
#include <tuple>
#include <memory>
#include <iostream>
#include <concepts>
#include <cassert>
#include <set>
#include <array>
namespace peg
{
    namespace parsers
    {
        
        template <typename elem>
        struct ParsingExprInterface {
            using ElementType = elem;
            virtual ~ParsingExprInterface() = default;
            virtual bool operator()(Context<elem>& context) const = 0;
        };

        template<typename elem, typename ExprType>
        struct ParsingExpr : ParsingExprInterface<elem>{
            using ParseExprType = ExprType;
        };

        template <typename T, typename elem>
        concept ParsingExprType = std::is_base_of<ParsingExprInterface<elem>, T>::value;


        template<typename elem>
        bool symbolConsumable(typename Context<elem>::IterType pos, const elem& value) {
            return *pos == value;
        }

        template<typename elem>
        bool symbolConsumable(typename Context<elem>::IterType pos, const std::set<elem>& values) {
            return values.find(*pos) != values.end();
        }

        template<typename elem>
        bool symbolConsumable(typename Context<elem>::IterType pos, const std::array<elem, 2>& values) {
            return (*pos >= values[0]) && (*pos <= values[1]); 
        }
        
        template <typename elem, typename ValuesType = elem>
        struct TerminalExpr : ParsingExpr<elem, TerminalExpr<elem, ValuesType>> {
            TerminalExpr(const ValuesType& value) : m_terminalValue{value} {}
            bool operator()(Context<elem>& context) const override {
                return parse(context);
            }
        private:
            bool parse(Context<elem>& context) const {
                if(!context.ended() && symbolConsumable(context.mark(), m_terminalValue)) {
                    context.next();
                    return true;
                }
                return false;
            }
            ValuesType m_terminalValue;
        };

        template<typename elem>
        struct NonTerminal : ParsingExpr<elem, NonTerminal<elem>> {
        public:

            NonTerminal(const NonTerminal& rhs) : m_rule{rhs.m_rule} {}

            NonTerminal(NonTerminal&& rhs) : m_rule(rhs.m_rule) {}

            template<typename ExprType>
            NonTerminal(const ParsingExpr<elem, ExprType>& rhs) 
                : m_rule(std::make_shared<ExprType>(static_cast<const ExprType&>(rhs))) {}
            
            template<typename ExprType>
            NonTerminal(ParsingExpr<elem, ExprType>&& rhs)
                : m_rule(std::make_shared<ExprType>(static_cast<const ExprType&>(rhs))) {}

            template<typename ExprType>
            NonTerminal& operator=(const ParsingExpr<elem, ExprType>& rhs){
                m_rule = std::make_shared<ExprType>(rhs);
                return *this;
            }

            bool operator()(Context<elem>& context) const override {
                return parse(context);
            }
        private:
            bool parse(Context<elem>& context) const {
                auto current_pos = context.mark();
                auto ruleState = context.ruleState(this, current_pos);
                std::cout<<__func__<<':'<<ruleState.m_leftRecursion<<':'<<m_rule<<std::endl;
                if (ruleState.m_leftRecursion) {
                    return false;
                }
                typename Context<elem>::RuleState prev{ruleState};
                bool result = m_rule->operator()(context);
                if(!result){
                    return false;
                }
                if(ruleState.m_leftRecursion) {
                    while(!context.ended()) {
                        const auto start = context.mark();
                        ruleState.m_pos = start;
                        bool result = m_rule->operator()(context);
                        const auto end = context.mark();
                        if(!result || end < start) break;
                    }
                }
                return true;

            }
            std::shared_ptr<ParsingExprInterface<elem>> m_rule;
        };

        template<typename elem>
        struct NonTerminalRef : ParsingExpr<elem, NonTerminalRef<elem>> {
            NonTerminalRef(const NonTerminal<elem>& rhs) : m_nonterm{rhs} {
                std::cout<<"NonTerminalRef ctor"<<std::endl;
            }
            bool operator()(Context<elem>& context) const override{
                return m_nonterm(context);
            }
        private:
            const NonTerminal<elem>& m_nonterm;

        };

        template<typename elem>
        struct EmptyExpr : ParsingExpr<elem, EmptyExpr<elem>> {
            EmptyExpr() {}
            bool operator()(Context<elem>& context) const override {
                return true;
            }
        };

        template<typename elem, typename ...Children>
        struct SequenceExpr : ParsingExpr<elem, SequenceExpr<elem, Children...>>  {
            SequenceExpr(const std::tuple<Children...>& children) : m_children{children} {
                std::cout<<"Seq"<<std::endl;
            }
            bool operator()(Context<elem>& context) const override {
                auto state = context.state();
                bool result = parseSeq<0>(context);
                if (!result){
                    context.state(state);
                }
                return result;
            }
            const std::tuple<Children...>& children() const {
                return m_children;
            }
        protected:
            template<size_t Index>
            bool parseSeq(Context<elem>& context) const {
                if constexpr (Index < sizeof...(Children)) {
                    bool result = std::get<Index>(m_children)(context);
                    if (result) {
                        return parseSeq<Index+1>(context);
                    }
                    else {
                        return false;
                    }
                }
                else {
                    return true;
                }
            }
            std::tuple<Children...> m_children;
        };

        template<typename elem, typename ...Children>
        struct AlternationExpr : ParsingExpr<elem, AlternationExpr<elem, Children...>>{
            AlternationExpr(const std::tuple<Children...>& children) : m_children(children) {}
            const std::tuple<Children...>& children() const {
                return m_children;
            }
            bool operator()(Context<elem>& context) const override {
                return parse<0>(context);

            }
        protected:
            template <size_t Index>
            bool parse(Context<elem>& context) const{
                if constexpr ( Index < sizeof...(Children)) {
                    bool result = std::get<Index>(m_children)(context);
                    if (result){
                        return true;
                    }
                    else {
                        return parse<Index+1>(context);
                    }
                }
                return false;
            }
            std::tuple<Children...> m_children;
        };

        template<typename elem, typename Child>
        struct Repetition : ParsingExpr<elem, Repetition<elem, Child>> {
            Repetition(const Child& child, size_t min_r, ssize_t max_r = -1)
                : m_child(child), min_rep(min_r), max_rep(max_r){
                    if (!((max_rep < 0) || ((max_rep > 0) && (min_rep <= max_rep)))) {
                        throw std::invalid_argument("rep not correct");
                    }
                }

            bool operator()(Context<elem> &context) const override {
                return parse(context);
            }
            const Child& child() {
                return m_child;
            }
            std::tuple<size_t, ssize_t> reps() const {
                return {min_rep, max_rep};
            }
        protected:
            Child m_child;
            size_t min_rep;
            ssize_t max_rep;

            bool parse(Context<elem> &context) const {
                auto initState = context.state();
                bool result = true;
                size_t loopCount = 0;
                while(true){
                    auto startState = context.state();
                    result = m_child(context);
                    if(result)
                        loopCount++;
                    else
                        break;
                    if((max_rep > 0) && (loopCount >= max_rep)){
                        break;
                    }
                    //Not advancing, stop
                    if(context.state().m_pos == startState.m_pos) {
                        break;
                    }
                }
                if(loopCount < min_rep){
                    context.state(initState);
                    return false;
                }
                else if (max_rep < 0 ){
                    return true;
                }
                else if(loopCount < min_rep){
                    context.state(initState);
                    return false;
                }
                return true;
            }
        };

        template<typename elem, typename Child>
        struct ZeroOrMoreExpr : Repetition<elem, Child> {
            ZeroOrMoreExpr(const Child& child) : Repetition<elem, Child>(child, 0, -1) {}
        };

        template<typename elem, typename Child>
        struct OneOrMoreExpr : Repetition<elem, Child> {
            OneOrMoreExpr(const Child& child) : Repetition<elem, Child>(child, 1, -1) {}
        };

        template<typename elem, typename Child>
        struct NTimesExpr : Repetition<elem, Child> {
            NTimesExpr(const Child& child, size_t n_reps) : Repetition<elem, Child>(child, n_reps, n_reps) {}
        };

        template<typename elem, typename Child>
        struct OptionalExpr : Repetition<elem, Child> {
            OptionalExpr(const Child& child) : Repetition<elem, Child>(child, 0, 1) {}
        };

        template<typename elem, typename Child>
        struct NotExpr : ParsingExpr<elem, NotExpr<elem, Child>>{
            NotExpr(const Child& child) :m_child(child) {}
            bool operator()(Context<elem>& context) const override {
                return parse(context);
            }
            const Child& child() {
                return m_child;
            }
        protected:
            Child m_child;
            bool parse(Context<elem>& context) const {
                auto initState = context.state();
                bool result = !m_child(context);
                context.state(initState);
                return result;
            }

        };

        template<typename elem, typename Child>
        struct AndExpr : ParsingExpr<elem, AndExpr<elem, Child>>{
            AndExpr(const Child& child) :m_child(child){}
            bool operator()(Context<elem>& context) const override {
                return parse(context);
            }
            const Child& child() {
                return m_child;
            }
        protected:
            Child m_child;
            bool parse(Context<elem>& context) const {
                auto initState = context.state();
                bool result = m_child(context);
                context.state(initState);
                return result; 
            }

        };




        
    } // namespace parsers


} // namespace peg
