#pragma once
#include <span>
#include <vector>
#include <map>
#include <iostream>
namespace peg
{
    namespace parsers{
        template<typename elem> struct NonTerminal;
    }

    template <typename elem, typename MatchType>
    struct Match {
        Match(std::span<elem> range) {}
    private:
        std::span<elem> m_match_range;
        MatchType m_match_type;
        std::vector<Match> m_children;
    };

    template <typename elem>
    struct Context {
    public:
        using IterType = typename std::span<const elem>::iterator;

        struct RuleState {
            RuleState(IterType pos, bool lr = false) : m_pos{pos}, m_leftRecursion{lr} {}
            RuleState(const RuleState&) = default;
            RuleState& operator=(const RuleState&) = default;
            IterType m_pos;
            bool m_leftRecursion;
        };

        struct State {
            State(IterType pos, size_t count) : m_pos(pos), m_matchCount(count) {}
            IterType m_pos;
            size_t m_matchCount;
        };

        State state() {
            return {m_position, m_matches.size()};
        }

        void state(const State& state) {
            m_position = state.m_pos;
            m_matches.resize(state.m_matchCount);
        }

        Context(std::span<elem> input) :m_input(input), m_position(m_input.begin()) {}

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
            std::cout<<"Current"<<*m_position<<std::endl;
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

        RuleState& ruleState(const peg::parsers::NonTerminal<elem> *rule, IterType pos) {
            const auto [iter, ok] = m_mem.emplace(rule, RuleState{pos, false});
            if(!ok){
                iter->second.m_leftRecursion = true;
            }
            return iter->second;
        }
    private:
        std::span<const elem> m_input;
        IterType m_position;
        std::vector<std::span<const elem>> m_matches;
        std::map<const parsers::NonTerminal<elem>*, RuleState> m_mem;
    };

    template<typename InputType>
    Context(const InputType&) -> Context<typename InputType::value_type>;

} // namespace peg
