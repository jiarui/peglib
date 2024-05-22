#pragma once
#include "Context.h"
#include <tuple>
#include <memory>
#include <iostream>
#include <cassert>
#include <set>
#include <array>
#include <ranges>
#include <concepts>
namespace peg
{
    namespace parsers
    {
        
        template <typename elem>
        struct ParsingExprInterface {
            friend NonTerminal<elem>;
            using ElementType = elem;
            virtual ~ParsingExprInterface() = default;
            virtual bool parse(Context<elem>& context) const = 0;
        };

        template<typename elem, typename ExprType>
        struct ParsingExpr : ParsingExprInterface<elem>{
            using ParseExprType = ExprType;
        };

        template<typename elem>
        bool symbolConsumable(const elem& v, const elem& value) {
            return v == value;
        }

        template<typename elem>
        bool symbolConsumable(const elem& v, const std::set<elem>& values) {
            return values.find(v) != values.end();
        }

        template<typename elem>
        bool symbolConsumable(const elem& v, const std::array<elem, 2>& values) {
            return (v >= values[0]) && (v <= values[1]); 
        }

        template<typename elem, typename Functor>
        requires std::predicate<Functor, elem>
        bool symbolConsumable(const elem& v, const Functor& f) {
            return f(v);
        }
        
        template <typename elem, typename ValuesType = elem>
        struct TerminalExpr : ParsingExpr<elem, TerminalExpr<elem, ValuesType>> {
            TerminalExpr(const ValuesType& value) : m_terminalValue{value} {}
            bool parse(Context<elem>& context) const {
                if(!context.ended() && symbolConsumable(*context.mark(), m_terminalValue)) {
                    context.next();
                    return true;
                }
                return false;
            }
        protected:
            ValuesType m_terminalValue;
        };

        template <typename elem, typename SeqType>
        requires std::ranges::random_access_range<SeqType>
        struct TerminalSeqExpr : ParsingExpr<elem, TerminalSeqExpr<elem, SeqType>> {
            TerminalSeqExpr(const SeqType& value) : m_terminalValues{value} {}
            bool parse(Context<elem>& context) const override {
                auto initState = context.state();
                for(const auto& i: m_terminalValues){
                    if(!context.ended() && symbolConsumable(*context.mark(), i)) {
                        context.next();
                    }
                    else {
                        context.state(initState);
                        return false;
                    }
                }                
                return true;
            }
        protected:
            SeqType m_terminalValues;
        };

        template<typename elem>
        struct NonTerminalRef;
        
        template<typename elem>
        struct NonTerminal : ParsingExpr<elem, NonTerminal<elem>> {
        public:
            using Action = std::function<void(Context<elem>&, std::span<const elem> match_range)>;

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

            bool operator()(Context<elem>& context) const {
                auto start_pos = context.mark();
                const bool result = parse(context);
                if (result && m_action) {
                    auto end_pos = context.mark();
                    m_action(context, std::span<const elem>(start_pos, end_pos));
                }
                return result;
            }

            void setAction(const Action& action) {
                m_action = action;
            }

            bool parse(Context<elem>& context) const override {
                auto current_pos = context.mark();
                std::tuple<bool, typename Context<elem>::RuleState&> rs = context.ruleState(this, current_pos);
                typename Context<elem>::RuleState& ruleState = std::get<1>(rs);
                bool result = false;
                if(!std::get<0>(rs)) {
                    context.reset(ruleState.m_last_pos);
                    result = ruleState.m_last_return;
                }
                else {
                    auto last_pos = current_pos;
                    bool last_return = false;
                    ruleState.m_last_pos = last_pos;
                    ruleState.m_last_return = last_return;
                    while(true) {
                        context.reset(current_pos);
                        bool res = m_rule->parse(context);
                        auto end_pos = context.mark();
                        if (res){
                            if(end_pos > last_pos){
                                ruleState.m_last_pos = (last_pos = end_pos);
                                ruleState.m_last_return = (last_return = res);
                            }
                            else {
                                ruleState.m_last_return = (last_return = res);
                                break;
                            }
                        }
                        else{
                            break;
                        }
                    }
                    result = last_return;
                    context.reset(last_pos);
                }
                return result;

            }
        protected:
            std::shared_ptr<ParsingExprInterface<elem>> m_rule;
            Action m_action;
        };

        template<typename elem>
        struct NonTerminalRef : ParsingExpr<elem, NonTerminalRef<elem>> {
            NonTerminalRef(const NonTerminal<elem>& rhs) : m_nonterm{rhs} {
            }
            bool parse(Context<elem>& context) const override {
                return m_nonterm.parse(context);
            }
        protected:
            const NonTerminal<elem>& m_nonterm;

        };

        template<typename elem>
        struct EmptyExpr : ParsingExpr<elem, EmptyExpr<elem>> {
            EmptyExpr() {}
            bool parse(Context<elem>& context) const override{
                return true;
            }
        };

        template<typename elem, typename ...Children>
        struct SequenceExpr : ParsingExpr<elem, SequenceExpr<elem, Children...>>  {
            SequenceExpr(const std::tuple<Children...>& children) : m_children{children} {
            }
            const std::tuple<Children...>& children() const {
                return m_children;
            }
            bool parse(Context<elem>& context) const override {
                auto state = context.state();
                bool result = parseSeq<0>(context);
                if (!result){
                    context.state(state);
                }
                return result;
            }
        protected:
            template<size_t Index>
            bool parseSeq(Context<elem>& context) const {
                if constexpr (Index < sizeof...(Children)) {
                    bool result = std::get<Index>(m_children).parse(context);
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
            bool parse(Context<elem>& context) const override {
                return parse<0>(context);
            }
        protected:
            template <size_t Index>
            bool parse(Context<elem>& context) const{
                if constexpr ( Index < sizeof...(Children)) {
                    bool result = std::get<Index>(m_children).parse(context);
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
            
            const Child& child() {
                return m_child;
            }
            std::tuple<size_t, ssize_t> reps() const {
                return {min_rep, max_rep};
            }

            bool parse(Context<elem> &context) const override {
                auto initState = context.state();
                bool result = true;
                size_t loopCount = 0;
                while(true){
                    auto startState = context.state();
                    result = m_child.parse(context);
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
        protected:
            Child m_child;
            size_t min_rep;
            ssize_t max_rep;
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
            const Child& child() {
                return m_child;
            }
            bool parse(Context<elem>& context) const override{
                auto initState = context.state();
                bool result = !m_child.parse(context);
                context.state(initState);
                return result;
            }
        protected:
            Child m_child;
        };   

        template<typename elem, typename Child>
        struct AndExpr : ParsingExpr<elem, AndExpr<elem, Child>>{
            AndExpr(const Child& child) :m_child(child){}
            const Child& child() {
                return m_child;
            }
            bool parse(Context<elem>& context) const override {
                auto initState = context.state();
                bool result = m_child.parse(context);
                context.state(initState);
                return result; 
            }
        protected:
            Child m_child;
            

        };
        
    } // namespace parsers


} // namespace peg
