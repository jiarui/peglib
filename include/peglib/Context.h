#pragma once
#include <span>
#include <vector>
#include <map>
#include <tuple>
#include <iostream>
#include <cassert>
#include <functional>
namespace peg
{
    namespace parsers{
        template<typename Context> struct ParsingExprInterface;
        template<typename Context> struct NonTerminal;
    }

    template <typename elem>
    struct Context {
    public:
        using IterType = typename std::span<const elem>::iterator;
        using ValueType = elem;
        using Rule = peg::parsers::NonTerminal<Context<elem>>;
        using Action = std::function<void(Context<elem>&)>;
        using Expr = peg::parsers::ParsingExprInterface<elem>;
        using MatchRange = typename std::span<const elem>;

        struct RuleState {
            RuleState(IterType pos, bool lr = false) : m_last_pos{pos}, m_last_return{lr} {}
            RuleState(const RuleState&) = default;
            RuleState& operator=(const RuleState&) = default;
            IterType m_last_pos;
            bool m_last_return;
        };

        struct State {
            State(IterType pos, size_t count) : m_pos(pos), m_matchCount(count) {}
            IterType m_pos;
            size_t m_matchCount;
        };

        State state() {
            return {m_position, 0};
        }

        void state(const State& state) {
            m_position = state.m_pos;
        }

        Context(std::span<const elem> input) :m_input(input), m_position(m_input.begin()) {}

        template <typename InputType>
        Context(const InputType& input) {
            m_input = std::span(input);
            m_position = m_input.begin();
        }

        bool ended() {
            return m_position == m_input.end();
        }

        IterType mark() {
            return m_position;
        }

        void next() {
            if(m_position < m_input.end()) {
                ++m_position;
            }
        }

        void next(size_t forward) {
            auto final_pos = m_position + forward;
            m_position = (final_pos <= m_input.end()) ? final_pos : m_input.end();
        }

        void reset(IterType pos) {
            assert(pos >= m_input.begin() && pos <= m_input.end());
            m_position = pos;
        }

        std::span<const elem> get_input() {
            return m_input;
        }

        std::tuple<bool, RuleState&> ruleState(const Rule *rule, IterType pos) {
            auto [iter, ok] = m_mem.emplace(std::make_tuple(rule, pos), RuleState{pos});
            return std::tuple<bool, RuleState&>{ok, iter->second};
        }

    private:
        std::span<const elem> m_input;
        IterType m_position;
        std::map<std::tuple<const Rule*, IterType>, RuleState> m_mem;
    };

    template<typename InputType>
    Context(const InputType&) -> Context<typename InputType::value_type>;

} // namespace peg
