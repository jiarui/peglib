#pragma once
#include <cassert>
#include <cstddef>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stack>
#include <tuple>
#include <variant>
#include <vector>

#include "FileSource.h"
#include "ParseError.h"
namespace peg
{
namespace parsers
{
template<typename Context>
struct ParsingExprInterface;
template<typename Context>
struct NonTerminal;
template<typename Context>
struct Rule;
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

// Does the given InputSource support `release_before` (i.e. cut-driven
// buffer eviction)? Contiguous span-backed sources do not; only FileSource
// does, because it owns the only paged/evictable storage.
template<typename>
inline constexpr bool is_context_releasable_v = false;

template<typename vt>
inline constexpr bool is_context_releasable_v<FileSource<vt>> = true;

template<InputSourceType InputSource, typename NodeType = std::monostate>
struct Context
{
    using node_type = NodeType;

    template<typename InputType>
    Context(const InputType& t)
        : m_input{std::span(t)}, m_position{m_input.begin()}, m_last_cut{m_position}
    {}

    using iterator = typename InputSource::iterator;
    using value_type = typename InputSource::value_type;
    using NonTerminalType = peg::parsers::NonTerminal<Context<InputSource, NodeType>>;
    using match_range = typename ContextMatchRange<InputSource>::type;

    // -------------------------------------------------------------------
    // ParseTreeNode: immutable record of a successful match.
    //
    // Produced by parse() and carried in ParseResult. The tree structure
    // mirrors the grammar: each NonTerminal that succeeds creates a node
    // named after the rule; combinator nodes (Sequence, Choice, etc.) are
    // anonymous grouping nodes. Semantic actions read children->value to
    // build parent values — no value stack involved.
    // -------------------------------------------------------------------
    struct ParseTreeNode
    {
        std::string name;             // rule name (empty for anonymous)
        std::size_t start_offset = 0; // byte offset of match start
        std::size_t end_offset = 0;   // byte offset past match end
        std::vector<std::shared_ptr<ParseTreeNode>> children;
        NodeType value{}; // filled by semantic action
    };
    using ParseTreeNodePtr = std::shared_ptr<ParseTreeNode>;

    // Result of every parse() call: success flag + optional tree node.
    // On failure, tree is nullptr. On success, tree may still be nullptr
    // for transparent rules (action returned a null value) or for leaf
    // expressions that don't create named nodes.
    struct ParseResult
    {
        bool success = false;
        ParseTreeNodePtr tree;
        explicit operator bool() const { return success; }
    };

    struct RuleState
    {
        RuleState(iterator pos, bool lr = false) : m_last_pos{pos}, m_last_return{lr} {}
        RuleState(const RuleState&) = default;
        RuleState& operator=(const RuleState&) = default;
        iterator m_last_pos;
        bool m_last_return;
        // Cached ParseResult from the first successful parse at this
        // (position, rule) pair. On a memo hit, the caller receives this
        // cached result — including the tree and action value — without
        // re-executing the action. This is what makes packrat memoization
        // safe with semantic actions.
        ParseResult m_cached_result;
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

    std::tuple<bool, RuleState> rule_state(const NonTerminalType* rule, iterator pos)
    {
        auto [iter_records, ins] =
            m_mem.emplace(pos, std::map<const NonTerminalType*, RuleState>{});
        auto [iter, ok] = iter_records->second.emplace(rule, RuleState{pos});
        return std::tuple<bool, RuleState>{ok, iter->second};
    }

    bool update_rule_state(const NonTerminalType* rule,
                           iterator start_pos,
                           iterator return_pos,
                           bool return_value)
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

    bool
    update_rule_state(const NonTerminalType* rule, iterator start_pos, const RuleState& rule_state)
    {
        auto memos = m_mem.find(start_pos);
        if (memos == m_mem.end()) {
            return false;
        }
        auto memo = memos->second.find(rule);
        if (memo == memos->second.end()) {
            return false;
        }
        // Copy all fields, including m_cached_result. This is how
        // NonTerminal::parse writes the final ParseResult back into the
        // memo map after a successful first-time parse.
        memo->second = rule_state;
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
            if constexpr (is_context_releasable_v<InputSource>) {
                m_input.release_before(m_last_cut);
            }
        }
        m_cut.pop();
    }

    // -----------------------------------------------------------------------
    // Error tracking: furthest-failure position + expected set
    // -----------------------------------------------------------------------

    using expected_set = std::set<ExpectedItem>;

    // Called by leaf expressions and NonTerminals when they fail.
    // Updates m_furthest_failure_pos / m_expected according to the rule:
    //   - If pos > m_furthest_failure_pos: clear, update, record.
    //   - If pos == m_furthest_failure_pos: append (set deduplicates).
    //   - If pos <  m_furthest_failure_pos: ignore.
    void record_failure(std::size_t pos, ExpectedItem item)
    {
        if (!m_has_error || pos > m_furthest_failure_pos) {
            m_furthest_failure_pos = pos;
            m_expected.clear();
            m_expected.insert(std::move(item));
            m_has_error = true;
        } else if (pos == m_furthest_failure_pos) {
            m_expected.insert(std::move(item));
        }
        // else: pos < furthest — ignore
    }

    // Convenience overload: takes iterator, converts to offset automatically.
    void record_failure(iterator pos_it, ExpectedItem item)
    {
        record_failure(offset_of(pos_it), std::move(item));
    }

    [[nodiscard]] std::size_t furthest_failure_pos() const noexcept
    {
        return m_furthest_failure_pos;
    }

    [[nodiscard]] const expected_set& expected() const noexcept { return m_expected; }

    [[nodiscard]] bool has_error() const noexcept { return m_has_error; }

    // Move the error out as a Diagnostic value-object. After this call,
    // has_error() returns false (the Context is reset to "no error" state).
    [[nodiscard]] std::optional<Diagnostic> take_error()
    {
        if (!m_has_error) {
            return std::nullopt;
        }
        Diagnostic diag{m_furthest_failure_pos, std::move(m_expected)};
        m_has_error = false;
        m_expected.clear();
        m_furthest_failure_pos = 0;
        return diag;
    }

    // Convert an iterator to a byte offset. Works for both span-backed
    // (random-access iterators) and FileSource-backed (custom iterators).
    [[nodiscard]] std::size_t offset_of(iterator it) const noexcept
    {
        if constexpr (is_context_releasable_v<InputSource>) {
            return it.position();
        } else {
            return static_cast<std::size_t>(it - m_input.begin());
        }
    }

    template<typename value_type>
    Context(FileSource<value_type>&& s)
        : m_input{std::move(s)}, m_position{m_input.begin()}, m_last_cut{m_position}
    {}

protected:
    InputSource m_input;
    iterator m_position;
    iterator m_last_cut;
    std::map<iterator, std::map<const NonTerminalType*, RuleState>> m_mem;
    std::stack<CutRecord> m_cut;

    // Error tracking state
    std::size_t m_furthest_failure_pos = 0;
    expected_set m_expected;
    bool m_has_error = false;
};

template<typename value_type>
auto from_file(const std::string& path, size_t bufsize)
{
    return Context<FileSource<value_type>>(FileSource<value_type>(bufsize, path));
}

template<InputSourceType InputType>
Context(const InputType&) -> Context<typename ContextInputSource<InputType>::type>;

} // namespace peg
