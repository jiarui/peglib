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

    // Construct from a contiguous range (std::string, std::vector, ...).
    // The Context stores a std::span into `t` — it does NOT copy. The caller
    // must keep the input alive for the Context's lifetime. For a self-
    // contained copy, use Grammar::parse_string (which makes its own string).
    // Passing a temporary here dangles silently.
    template<typename InputType>
    Context(const InputType& t)
        : m_input{std::span(t)}, m_position{0}, m_last_cut{0},
          m_input_size{static_cast<std::size_t>(m_input.end() - m_input.begin())}
    {}

    // A Context owns a memo table, cut stack, and (for FileSource) paged
    // buffers — all expensive or unsafe to duplicate. It is meant for a
    // single parse; copying one mid-parse would duplicate memo entries
    // keyed by raw NonTerminal* and silently corrupt furthest-error state.
    // Move is allowed (e.g. from from_file); copy is not.
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) noexcept = default;
    Context& operator=(Context&&) noexcept = default;

    using iterator = typename InputSource::iterator;
    using value_type = typename InputSource::value_type;
    using NonTerminalType = peg::parsers::NonTerminal<Context<InputSource, NodeType>>;

    // Position within the input is tracked as a byte/item offset (std::size_t)
    // rather than an iterator. This decouples the memo key and all position
    // state from the InputSource's iterator type, which is the prerequisite
    // for the source-erasure refactor (Phase 2). For span-backed sources the
    // offset is pointer-difference-equivalent; for FileSource the iterator's
    // ordering (FileSource.h) is already defined in terms of the same size_t
    // m_pos, so the migration is semantically exact.

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
        explicit RuleState(std::size_t pos) : m_last_pos{pos} {}
        RuleState(const RuleState&) = default;
        RuleState& operator=(const RuleState&) = default;
        std::size_t m_last_pos;
        // Cached ParseResult from the first successful parse at this
        // (position, rule) pair. On a memo hit, the caller receives this
        // cached result — including the tree and action value — without
        // re-executing the action. This is what makes packrat memoization
        // safe with semantic actions.
        ParseResult m_cached_result;
    };

    struct State
    {
        explicit State(std::size_t pos) : m_pos(pos) {}
        std::size_t m_pos;
        // Comparable so combinators can detect zero-width progress
        // (e.g. a repetition body that matched without advancing) without
        // reaching into m_pos directly.
        friend bool operator==(const State& lhs, const State& rhs)
        {
            return lhs.m_pos == rhs.m_pos;
        }
    };

    // The input length, in items. Computed once at construction; used by
    // ended()/next()/reset() so they no longer need to compare iterators.
    std::size_t input_size() const noexcept { return m_input_size; }

    State state() { return State{m_position}; }

    void state(const State& state) { m_position = state.m_pos; }

    bool ended() const noexcept { return m_position >= m_input_size; }

    // Current position as a byte/item offset. Replaces the iterator-returning
    // mark(); all position state is now offset-based.
    std::size_t mark() const noexcept { return m_position; }

    // Value at the current position. TerminalExpr / TerminalSeqExpr use this
    // instead of dereferencing an iterator. Dispatches to the contiguous
    // buffer (span) or the paged accessor (FileSource) at compile time.
    value_type current() const
    {
        assert(m_position < m_input_size && "current() past end of input");
        if constexpr (is_context_releasable_v<InputSource>) {
            return m_input.at(m_position);
        } else {
            return m_input[m_position];
        }
    }

    void next() noexcept
    {
        if (m_position < m_input_size) {
            ++m_position;
        }
    }

    void reset(std::size_t pos) noexcept
    {
        // Upper bound is always enforced. The lower bound (m_last_cut) is
        // NOT enforced: after a cut, memo data for earlier positions has
        // been intentionally released, but it is still valid to rewind
        // there and re-parse from scratch.
        assert(pos <= m_input_size && "reset past end of input");
        m_position = pos;
    }

    const InputSource& get_input() const { return m_input; }

    std::tuple<bool, RuleState> rule_state(const NonTerminalType* rule, std::size_t pos)
    {
        auto [iter_records, ins] =
            m_mem.emplace(pos, std::map<const NonTerminalType*, RuleState>{});
        auto [iter, ok] = iter_records->second.emplace(rule, RuleState{pos});
        return std::tuple<bool, RuleState>{ok, iter->second};
    }

    bool update_rule_state(const NonTerminalType* rule,
                           std::size_t start_pos,
                           const RuleState& rule_state)
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
        std::size_t pos;
        bool cut = false;
        CutRecord(std::size_t i, bool c) : pos{i}, cut{c} {}
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

    template<typename value_type>
    Context(FileSource<value_type>&& s)
        : m_input{std::move(s)}, m_position{0}, m_last_cut{0}, m_input_size{m_input.size()}
    {}

protected:
    InputSource m_input;
    std::size_t m_position = 0;
    std::size_t m_last_cut = 0;
    std::size_t m_input_size = 0;
    std::map<std::size_t, std::map<const NonTerminalType*, RuleState>> m_mem;
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
