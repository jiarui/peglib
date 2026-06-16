#pragma once
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <span>
#include <stack>
#include <tuple>
#include <vector>

#include "FileSource.h"
namespace peg
{
namespace parsers
{
template<typename Context>
struct ParsingExprInterface;
template<typename Context>
struct NonTerminal;
} // namespace parsers

// std::string std::vector file

template<typename T>
concept InputSourceType = requires(T t) {
    { t.begin() };
    { t.end() };
    typename T::value_type;
    typename T::iterator;
};

template<InputSourceType InputType>
struct ContextInputSource
{
    using type = std::span<const typename InputType::value_type>;
};

template<typename vt>
struct ContextInputSource<FileSource<vt>>
{
    using type = FileSource<vt>;
};

// Trait that selects the `match_range` type for a given InputSource.
//
// For contiguous-range sources (std::span-backed) we use a matching
// `std::span<const value_type>` so semantic actions get a flat view.
//
// For FileSource the underlying storage is paged and not contiguous,
// so `std::span<const char>` cannot be constructed from a pair of
// `FileSource::iterator`. Fall back to a pair-of-iterators range.
// (Long-term: replace with a lazy view / subrange once FileSource
// fully models std::input_iterator.)
template<InputSourceType InputType>
struct ContextMatchRange
{
    using type = std::span<const typename InputType::value_type>;
};

template<typename vt>
struct ContextMatchRange<FileSource<vt>>
{
    using type = std::pair<typename FileSource<vt>::iterator, typename FileSource<vt>::iterator>;
};

template<InputSourceType InputSource>
struct Context
{
    template<typename InputType>
    Context(const InputType& t)
        : m_input{std::span(t)}, m_position{m_input.begin()}, m_last_cut{m_position}
    {}

    using iterator = typename InputSource::iterator;
    using value_type = typename InputSource::value_type;
    using Rule = peg::parsers::NonTerminal<Context<InputSource>>;
    using match_range = typename ContextMatchRange<InputSource>::type;

    struct RuleState
    {
        RuleState(iterator pos, bool lr = false) : m_last_pos{pos}, m_last_return{lr} {}
        RuleState(const RuleState&) = default;
        RuleState& operator=(const RuleState&) = default;
        iterator m_last_pos;
        bool m_last_return;
    };

    struct State
    {
        explicit State(iterator pos) : m_pos(pos) {}
        iterator m_pos;
    };

    State state() { return State{m_position}; }

    void state(const State& state) { m_position = state.m_pos; }

    bool ended() const { return m_position == m_input.end(); }

    iterator mark() const { return m_position; }

    void next()
    {
        if (m_position < m_input.end()) {
            ++m_position;
        }
    }

    void reset(iterator pos)
    {
        // Upper bound is always enforced. The lower bound (m_last_cut) is
        // NOT enforced: after a cut, memo data for earlier positions has
        // been intentionally released, but it is still valid to rewind
        // there and re-parse from scratch.
        assert(pos <= m_input.end() && "reset past end of input");
        m_position = pos;
    }

    const InputSource& get_input() const { return m_input; }

    std::tuple<bool, RuleState> ruleState(const Rule* rule, iterator pos)
    {
        auto [iter_records, ins] = m_mem.emplace(pos, std::map<const Rule*, RuleState>{});
        auto [iter, ok] = iter_records->second.emplace(rule, RuleState{pos});
        return std::tuple<bool, RuleState>{ok, iter->second};
    }

    bool
    updateRuleState(const Rule* rule, iterator start_pos, iterator return_pos, bool return_value)
    {
        auto memos = m_mem.find(start_pos);
        if (memos == m_mem.end()) {
            return false;
        }
        auto memo = memos->second.find(rule);
        if (memo == memos->second.end()) {
            return false;
        }
        memo->second.m_last_pos = return_pos;
        memo->second.m_last_return = return_value;
        return true;
    }

    bool updateRuleState(const Rule* rule, iterator start_pos, const RuleState& ruleState)
    {
        auto memos = m_mem.find(start_pos);
        if (memos == m_mem.end()) {
            return false;
        }
        auto memo = memos->second.find(rule);
        if (memo == memos->second.end()) {
            return false;
        }
        memo->second.m_last_pos = ruleState.m_last_pos;
        memo->second.m_last_return = ruleState.m_last_return;
        return true;
    }

    struct CutRecord
    {
        iterator pos;
        bool cut = false;
        CutRecord(iterator i, bool c) : pos{i}, cut{c} {}
    };

    void cut(bool c)
    {
        m_cut.top().cut = c;
        m_cut.top().pos = mark();
    }

    bool cut() { return m_cut.top().cut; }

    void init_cut() { m_cut.emplace(mark(), false); }

    void remove_cut()
    {
        if (cut()) {
            m_last_cut = m_cut.top().pos;
            std::erase_if(m_mem, [this](const auto& item) {
                const auto& [pos, record] = item;
                return pos < m_last_cut;
            });
            // TODO notify m_input to release elements before m_last_cut
        }
        m_cut.pop();
    }

    template<typename value_type>
    Context(FileSource<value_type>&& s)
        : m_input{std::move(s)}, m_position{m_input.begin()}, m_last_cut{m_position}
    {}

protected:
    InputSource m_input;
    iterator m_position;
    iterator m_last_cut;
    std::map<iterator, std::map<const Rule*, RuleState>> m_mem;
    std::stack<CutRecord> m_cut;
};

template<typename value_type>
auto from_file(const std::string& path, size_t bufsize)
{
    return Context<FileSource<value_type>>(FileSource<value_type>(bufsize, path));
}

template<InputSourceType InputType>
Context(const InputType&) -> Context<typename ContextInputSource<InputType>::type>;

} // namespace peg
