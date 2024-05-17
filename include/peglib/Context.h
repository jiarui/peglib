#pragma once
#include <span>
#include <vector>
#include <map>
#include <tuple>
#include <iostream>
#include <cassert>
namespace peg
{
    namespace parsers{
        template<typename elem> struct NonTerminal;
    }

    template <typename elem, typename MatchType>
    struct Match {
        using IterType = typename std::span<const elem>::iterator;
        Match() = default;
        Match(const MatchType& match_id, const std::span<const elem>& match_pos) :
            m_match_id{match_id}, m_match_pos(match_pos) {}
        
        Match(const MatchType& match_id, std::span<const elem>&& match_pos) :
            m_match_id{match_id}, m_match_pos(match_pos) {}
        
        Match(const MatchType& match_id, const IterType start, const IterType end):
            Match(match_id, std::span<const elem>(start, end)) {}
        const MatchType& id() const {
            return m_match_id;
        }
    protected:
        MatchType m_match_id;
        std::span<const elem> m_match_pos;
    };

    template <typename elem, typename MatchType_ = int>
    struct Context {
    public:
        using IterType = typename std::span<const elem>::iterator;
        using MatchType = MatchType_;

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
            return {m_position, m_matches.size()};
        }

        void state(const State& state) {
            m_position = state.m_pos;
            m_matches.resize(state.m_matchCount);
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
            std::cout<<"Current "<<*m_position<<std::to_address(m_position)<<std::endl;
            if(m_position < m_input.end()) {
                ++m_position;
                std::cout<<"Now at "<<*m_position<<std::to_address(m_position)<<std::endl;
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

        std::tuple<bool, RuleState&> ruleState(const peg::parsers::NonTerminal<elem> *rule, IterType pos) {
            auto [iter, ok] = m_mem.emplace(std::make_tuple(rule, pos), RuleState{pos});
            return std::tuple<bool, RuleState&>{ok, iter->second};
        }

        void addMatch(MatchType match_id, IterType start, IterType end) {
            m_matches.emplace_back(match_id, start, end);
        }

        const std::vector<Match<elem, MatchType>>& matches(){
            return m_matches;
        }

    private:
        std::span<const elem> m_input;
        IterType m_position;
        std::vector<Match<elem, MatchType>> m_matches;
        std::map<std::tuple<const parsers::NonTerminal<elem>*, IterType>, RuleState> m_mem;
    };

    template<typename InputType>
    Context(const InputType&) -> Context<typename InputType::value_type>;

} // namespace peg
