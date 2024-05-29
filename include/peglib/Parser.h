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
#include <functional>
namespace peg
{
    namespace parsers
    {
        
        template <typename Context>
        struct ParsingExprInterface {
            friend NonTerminal<Context>;
            using ElementType = typename Context::ValueType;
            virtual ~ParsingExprInterface() = default;
            virtual bool parse(Context& context) const = 0;
        };

        template<typename Context, typename ExprType, typename NodeType_ = void>
        struct ParsingExpr : ParsingExprInterface<Context>{
            using ParseExprType = ExprType;
            using NodeType = NodeType_;
            using SematicAction = std::function<NodeType(Context&, typename Context::MatchRange match_range)>;
            void setAction(SematicAction action) {
                m_action = action;
            }
            ParsingExpr() = default;
            ParsingExpr(SematicAction action) : m_action(action) {}
            ParsingExpr(const ParsingExpr&) = default;
            ParsingExpr(ParsingExpr&&) = delete;
        protected:
            SematicAction m_action;
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
        
        template <typename Context, typename TerminalValueType>
        struct TerminalExpr : ParsingExpr<Context, TerminalExpr<Context, TerminalValueType>> {
            using SematicAction = typename ParsingExpr<Context, TerminalExpr<Context, TerminalValueType>>::SematicAction;
            TerminalExpr(const TerminalValueType& value, SematicAction action=nullptr) : m_terminalValue{value}, ParsingExpr<Context, TerminalExpr<Context, TerminalValueType>>(action) {}
            bool parse(Context& context) const {
                if(!context.ended() && symbolConsumable(*context.mark(), m_terminalValue)) {
                    context.next();
                    return true;
                }
                return false;
            }
        protected:
            TerminalValueType m_terminalValue;
        };

        template <typename Context, typename SeqType>
        requires std::ranges::random_access_range<SeqType>
        struct TerminalSeqExpr : ParsingExpr<Context, TerminalSeqExpr<Context, SeqType>> {
            TerminalSeqExpr(const SeqType& value) : m_terminalValues{value} {}
            bool parse(Context& context) const override {
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

        template<typename Context>
        struct NonTerminalRef;
        
        template<typename Context>
        struct NonTerminal : ParsingExpr<Context, NonTerminal<Context>> {
        public:

            template<typename ExprType>
            NonTerminal(const ParsingExpr<Context, ExprType>& rhs) 
                : m_rule(std::make_shared<ExprType>(static_cast<const ExprType&>(rhs))) {}
            
            template<typename ExprType>
            NonTerminal(ParsingExpr<Context, ExprType>&& rhs)
                : m_rule(std::make_shared<ExprType>(static_cast<const ExprType&>(rhs))) {}

            template<typename ExprType>
            NonTerminal& operator=(const ParsingExpr<Context, ExprType>& rhs){
                m_rule = std::make_shared<ExprType>(rhs);
                return *this;
            }

            bool operator()(Context& context) const {
                return parse(context);
            }

            bool parse(Context& context) const override {
                auto start_pos = context.mark();
                std::tuple<bool, typename Context::RuleState&> rs = context.ruleState(this, start_pos);
                typename Context::RuleState& ruleState = std::get<1>(rs);
                bool result = false;
                if(!std::get<0>(rs)){
                    context.reset(ruleState.m_last_pos);
                    result = ruleState.m_last_return;
                    return result;
                }
                else {
                    result = parseImpl(context, ruleState);
                    if (result && ParsingExpr<Context, NonTerminal<Context>>::m_action) {
                        auto end_pos = context.mark();
                        ParsingExpr<Context, NonTerminal<Context>>::m_action(context, typename Context::MatchRange(start_pos, end_pos));
                    }
                    return result;
                }
            }

        protected:

            bool parseImpl(Context& context, typename Context::RuleState& ruleState) const {
                auto current_pos = context.mark();
                auto last_pos = context.mark();
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
                bool result = last_return;
                context.reset(last_pos);
                return result;
            }
        protected:
            std::shared_ptr<ParsingExprInterface<Context>> m_rule;
        };

        template<typename Context>
        struct NonTerminalRef : ParsingExpr<Context, NonTerminalRef<Context>> {
            NonTerminalRef(const NonTerminal<Context>& rhs) : m_nonterm{rhs} {
            }
            bool parse(Context& context) const override {
                return m_nonterm.parse(context);
            }
        protected:
            const NonTerminal<Context>& m_nonterm;

        };

        template<typename Context>
        struct EmptyExpr : ParsingExpr<Context, EmptyExpr<Context>> {
            EmptyExpr() {}
            bool parse(Context& context) const override{
                return true;
            }
        };

        template<typename Context, typename ...Children>
        struct SequenceExpr : ParsingExpr<Context, SequenceExpr<Context, Children...>>  {
            SequenceExpr(const std::tuple<Children...>& children) : m_children{children} {}
            
            const std::tuple<Children...>& children() const {
                return m_children;
            }
            bool parse(Context& context) const override {
                auto state = context.state();
                bool result = parseSeq<0>(context);
                if (!result){
                    context.state(state);
                }
                return result;
            }
        protected:
            template<size_t Index>
            bool parseSeq(Context& context) const {
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

        template<typename Context, typename ...Children>
        struct AlternationExpr : ParsingExpr<Context, AlternationExpr<Context, Children...>>{
            AlternationExpr(const std::tuple<Children...>& children) : m_children(children) {}
            const std::tuple<Children...>& children() const {
                return m_children;
            }
            bool parse(Context& context) const override {
                return parse<0>(context);
            }
        protected:
            template <size_t Index>
            bool parse(Context& context) const{
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

        template<typename Context, typename Child>
        struct Repetition {
            Repetition(const Child& child, size_t min_r, ssize_t max_r = -1)
                : m_child(child), min_rep(min_r), max_rep(max_r){
                    if (!((max_rep < 0) || ((max_rep >= 0) && (min_rep <= max_rep)))) {
                        throw std::invalid_argument("rep not correct");
                    }
                }
            
            const Child& child() {
                return m_child;
            }
            std::tuple<size_t, ssize_t> reps() const {
                return {min_rep, max_rep};
            }

            bool parse(Context &context) const {
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

        template<typename Context, typename Child>
        struct ZeroOrMoreExpr : ParsingExpr<Context, ZeroOrMoreExpr<Context, Child>>, Repetition<Context, Child> {
            ZeroOrMoreExpr(const Child& child) : Repetition<Context, Child>(child, 0, -1) {}
            using Repetition<Context, Child>::child;
            bool parse(Context &context) const override {
                return Repetition<Context, Child>::parse(context);
            }
        };

        template<typename Context, typename Child>
        struct OneOrMoreExpr : ParsingExpr<Context, OneOrMoreExpr<Context, Child>>, Repetition<Context, Child> {
            OneOrMoreExpr(const Child& child) : Repetition<Context, Child>(child, 1, -1) {}
            using Repetition<Context, Child>::child;
            bool parse(Context &context) const override {
                return Repetition<Context, Child>::parse(context);
            }
        };

        template<typename Context, typename Child>
        struct NTimesExpr : ParsingExpr<Context, NTimesExpr<Context, Child>>, Repetition<Context, Child> {
            NTimesExpr(const Child& child, size_t n_reps) : Repetition<Context, Child>(child, n_reps, n_reps) {}
            using Repetition<Context, Child>::child;
            bool parse(Context &context) const override {
                return Repetition<Context, Child>::parse(context);
            }
        };

        template<typename Context, typename Child>
        struct OptionalExpr : ParsingExpr<Context, OptionalExpr<Context, Child>>, Repetition<Context, Child> {
            OptionalExpr(const Child& child) : Repetition<Context, Child>(child, 0, 1) {}
            using Repetition<Context, Child>::child;
            bool parse(Context &context) const override {
                return Repetition<Context, Child>::parse(context);
            }
        };

        template<typename Context, typename Child>
        struct NotExpr : ParsingExpr<Context, NotExpr<Context, Child>>{
            NotExpr(const Child& child) :m_child(child) {}
            const Child& child() {
                return m_child;
            }
            bool parse(Context& context) const override{
                auto initState = context.state();
                bool result = !m_child.parse(context);
                context.state(initState);
                return result;
            }
        protected:
            Child m_child;
        };   

        template<typename Context, typename Child>
        struct AndExpr : ParsingExpr<Context, AndExpr<Context, Child>>{
            AndExpr(const Child& child) :m_child(child){}
            const Child& child() {
                return m_child;
            }
            bool parse(Context& context) const override {
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
