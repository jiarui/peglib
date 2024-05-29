#pragma once
#include <span>
#include <vector>
#include <map>
#include <tuple>
#include <iostream>
#include <cassert>
#include <functional>
#include "FileReader.h"
namespace peg
{
    namespace parsers{
        template<typename Context> struct ParsingExprInterface;
        template<typename Context> struct NonTerminal;
    }

    //std::string std::vector file

    template <typename InputRef>
    struct Context {

        template <typename InputType>
        Context(const InputType& t) : m_input{std::span(t)}, m_position{m_input.begin()} {
        }

        Context(const std::string& path, size_t bufsize) : m_input{path, bufsize}, m_position{m_input.begin()} {}

        using IterType = typename InputRef::iterator;
        using ValueType = typename InputRef::value_type;
        using Rule = peg::parsers::NonTerminal<Context<InputRef>>;
        using Action = std::function<void(Context<InputRef>&)>;
        using MatchRange = typename std::span<const ValueType>;

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

        void reset(IterType pos) {
            assert(pos >= m_input.begin() && pos <= m_input.end());
            m_position = pos;
        }

        InputRef get_input() {
            return m_input;
        }

        std::tuple<bool, RuleState&> ruleState(const Rule *rule, IterType pos) {
            auto [iter, ok] = m_mem.emplace(std::make_tuple(rule, pos), RuleState{pos});
            return std::tuple<bool, RuleState&>{ok, iter->second};
        }

    public:
        InputRef m_input;
        IterType m_position;
        std::map<std::tuple<const Rule*, IterType>, RuleState> m_mem;
    };

    template<typename InputRef>
    Context(InputRef) -> Context<std::span<const typename InputRef::value_type>>;

    Context(const std::string& path, size_t ) -> Context<FileReader>;

    

} // namespace peg
