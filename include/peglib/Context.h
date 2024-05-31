#pragma once
#include <span>
#include <vector>
#include <map>
#include <tuple>
#include <iostream>
#include <cassert>
#include <functional>
#include <stack>
#include "FileReader.h"
namespace peg
{
    namespace parsers{
        template<typename Context> struct ParsingExprInterface;
        template<typename Context> struct NonTerminal;
    }

    //std::string std::vector file

    template <typename T>
    concept InputSourceType = requires ( T t) {
        {t.begin()};
        {t.end()};
        typename T::value_type;
        typename T::iterator;
    };

    template <InputSourceType InputSource>
    struct Context {

        template <typename InputType>
        Context(const InputType& t) : m_input{std::span(t)}, m_position{m_input.begin()}, m_last_cut{m_position} 
        {}

        Context(const std::string& path, size_t bufsize) : m_input{bufsize, path}, m_position{m_input.begin()}, m_last_cut{m_position} {}

        using IterType = typename InputSource::iterator;
        using ValueType = typename InputSource::value_type;
        using Rule = peg::parsers::NonTerminal<Context<InputSource>>;
        using Action = std::function<void(Context<InputSource>&)>;
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
            assert(pos >= m_last_cut && pos <= m_input.end());
            m_position = pos;
        }

        InputSource get_input() {
            return m_input;
        }

        std::tuple<bool, RuleState> ruleState(const Rule *rule, IterType pos) {
            auto [iter_records, ins] = m_mem.emplace(pos, std::map<const Rule*, RuleState>{});
            auto [iter, ok] = iter_records->second.emplace(rule, RuleState{pos});
            return std::tuple<bool, RuleState>{ok, iter->second};
        }

        bool updateRuleState(const Rule* rule, IterType start_pos, IterType return_pos, bool return_value){
            auto memos = m_mem.find(start_pos);
            if(memos == m_mem.end()) {
                return false;
            }
            auto memo = memos->second.find(rule);
            if(memo == memos->second.end()){
                return false;
            }
            memo->second.m_last_pos = return_pos;
            memo->second.m_last_return = return_value;
            return true;
        }

        bool updateRuleState(const Rule* rule, IterType start_pos, const RuleState& ruleState){
            auto memos = m_mem.find(start_pos);
            if(memos == m_mem.end()) {
                return false;
            }
            auto memo = memos->second.find(rule);
            if(memo == memos->second.end()){
                return false;
            }
            memo->second.m_last_pos = ruleState.m_last_pos;
            memo->second.m_last_return = ruleState.m_last_return;
            return true;
        }

        struct CutRecord {
            IterType pos;
            bool cut = false;
            CutRecord(IterType i, bool c) : pos{i}, cut{c} {}
        };

        void cut(bool c) {
            m_cut.top().cut = c;
            m_cut.top().pos = mark();
        }

        bool cut() {
            return m_cut.top().cut;
        }

        void init_cut() {
            m_cut.emplace(mark(), false);
        }

        void remove_cut() {
            if(cut()){
                m_last_cut = m_cut.top().pos;
                std::erase_if(m_mem, [this](const auto& item) {
                    const auto& [pos, record] = item;
                    return pos < m_last_cut;
                });
                //TODO notify m_input to release elements before m_last_cut
            }
            m_cut.pop();
        }

    public:
        InputSource m_input;
        IterType m_position;
        IterType m_last_cut;
        std::map<IterType, std::map<const Rule*, RuleState>> m_mem;
        std::stack<CutRecord> m_cut;
    };

    template<typename InputSource>
    Context(InputSource) -> Context<std::span<const typename InputSource::value_type>>;

    Context(const std::string& path, size_t ) -> Context<FileSource>;

    

} // namespace peg
