#pragma once
#include <cassert>
#include <cstddef>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stack>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "InputSource.h"
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

// ===========================================================================
// Context: the parse state carried through every expression's parse() call.
//
// Three orthogonal axes are now cleanly separated:
//   - CharT   : the character type (char, char32_t, ...). Determines matching
//               semantics and the terminal literal types.
//   - NodeType: the semantic-action product type stored on ParseTreeNode.
//               std::monostate (default) = pure recognizer; a value type =
//               lightweight product; std::shared_ptr<T> = polymorphic shared
//               AST. The library does not force a storage policy on NodeType.
//   - Source  : the input storage strategy. Erased behind InputSourceBase;
//               SpanSource (contiguous, zero-virtual-call hot path) or
//               FileSourceSource (paged, cut-evictable). Selected at
//               construction, invisible to the template signature.
//
// The source is type-erased so that a single Context<CharT, NodeType> can
// drive either storage strategy. SpanSource fills a raw-pointer cache
// (m_fast_data) that the per-character hot path indexes directly — zero
// virtual dispatch for the common in-memory case.
// ===========================================================================
template<typename CharT, typename NodeType = std::monostate>
struct Context
{
    using node_type = NodeType;
    using value_type = CharT;
    using char_type = CharT;

    // -----------------------------------------------------------------------
    // Construction.
    //
    // (1) From a contiguous range (std::string, std::vector, ...): the
    //     Context stores a non-owning SpanSource pointing into `t`. The
    //     caller must keep the input alive for the Context's lifetime.
    //     Passing a temporary here dangles silently. For a self-contained
    //     copy, use Grammar::parse_string (which makes its own string).
    //
    // (2) From a FileSource rvalue: the Context takes ownership (moved into
    //     a FileSourceSource adapter). No lifetime obligation on the caller.
    // -----------------------------------------------------------------------
    template<typename Range>
    Context(const Range& t)
        : m_input{std::make_unique<SpanSource<CharT>>(std::span<const CharT>(t).data(),
                                                      std::span<const CharT>(t).size())},
          m_fast_data{m_input->contiguous_data()}, m_input_size{m_input->size()}
    {}

    template<typename C, std::size_t PageSize>
    Context(FileSource<C, PageSize>&& fs)
        : m_input{std::make_unique<FileSourceSource<C, PageSize>>(std::move(fs))},
          m_fast_data{nullptr}, m_input_size{m_input->size()}
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

    using NonTerminalType = peg::parsers::NonTerminal<Context<CharT, NodeType>>;

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
        NodeType value{}; // filled by the untyped semantic action (escape hatch)
        // Producer rule (typed-fold dispatch). Stamped by NonTerminal::parse so
        // the post-parse typed fold can find each node's registered action via
        // pointer identity (no name/string lookup). Null for anonymous
        // combinator nodes and for transparent rules with no typed action.
        const NonTerminalType* producer = nullptr;
        // Winning-branch index for an AlternationExpr's node (the node IS the
        // winner's node, passed through). Stamped by parseAlt so the typed fold
        // can dispatch on the actual winning branch's static type at runtime
        // (the fold knows the branch TYPES but not which won). SIZE_MAX = not
        // an alternation winner / not stamped.
        std::size_t alt_winner = static_cast<std::size_t>(-1);
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

    // The input length, in items. Computed once at construction.
    std::size_t input_size() const noexcept { return m_input_size; }

    State state() { return State{m_position}; }

    void state(const State& state) { m_position = state.m_pos; }

    bool ended() const noexcept { return m_position >= m_input_size; }

    // Current position as a byte/item offset.
    std::size_t mark() const noexcept { return m_position; }

    // Value at the current position. Uses the contiguous cache when available
    // (span-backed — zero virtual dispatch); falls back to the virtual at()
    // for paged sources (FileSource).
    value_type current() const
    {
        assert(m_position < m_input_size && "current() past end of input");
        if (m_fast_data) {
            return m_fast_data[m_position];
        }
        return m_input->at(m_position);
    }

    // Value at an arbitrary offset. Same fast-path logic as current().
    value_type at(std::size_t offset) const
    {
        assert(offset < m_input_size && "at() past end of input");
        if (m_fast_data) {
            return m_fast_data[offset];
        }
        return m_input->at(offset);
    }

    // Access the underlying input source. Semantic actions that need to
    // extract matched text by offset use ctx.input().slice(off, count) —
    // slicing is an InputSource concern, not a parse-state concern, and
    // keeping it on InputSource avoids forcing every Context instantiation
    // (including non-character token types) to name std::basic_string<CharT>.
    [[nodiscard]] InputSourceBase<CharT>& input() const noexcept { return *m_input; }

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

    // Set the current scope's cut flag. No-op when the cut stack is empty
    // (a cut appearing outside any Alternation/Repetition scope has no
    // scope to commit — the cut flag is simply dropped). This keeps
    // standalone `~` / cut() safe at the grammar top level.
    void cut(bool c)
    {
        if (m_cut.empty())
            return;
        m_cut.top().cut = c;
        m_cut.top().pos = mark();
    }

    // Outside any Alternation/Repetition scope the cut stack is empty and
    // nothing has been committed. Reading cut() there returns false (no
    // active commitment) rather than crashing on the empty stack.
    bool cut() { return m_cut.empty() ? false : m_cut.top().cut; }

    void init_cut() { m_cut.emplace(mark(), false); }

    void remove_cut()
    {
        if (cut()) {
            m_last_cut = m_cut.top().pos;
            std::erase_if(m_mem, [this](const auto& item) {
                const auto& [pos, record] = item;
                return pos < m_last_cut;
            });
            // Virtual dispatch: no-op for SpanSource, evicts pages for
            // FileSourceSource. Replaces the old if-constexpr trait branch.
            m_input->release_before(m_last_cut);
        }
        m_cut.pop();
    }

    // -------------------------------------------------------------------
    // Auto-skip. The skipper is a transparent rule invoked between
    // adjacent sequence elements and between repetition iterations. It is
    // owned by Grammar and stamped onto the Context at Grammar::parse
    // entry. These accessors are public only because the expression types
    // that call run_skipper() live in peg::parsers; end users should drive
    // this via Grammar::set_skipper / lexeme().
    // -------------------------------------------------------------------

    // Invoke the skipper if one is set and auto-skip is enabled. Called
    // by SequenceExpr / DynSequenceExpr / repeat_parse_impl between
    // adjacent children. No-op (and zero virtual dispatch) when no
    // skipper is set or when skip_enabled() is false (inside lexeme).
    //
    // Reentrancy guard: while the skipper itself runs, auto-skip is
    // temporarily disabled so the skipper's own internal Repetition /
    // Sequence children do not recursively invoke run_skipper() (which
    // would double-consume input or, for a skipper written with
    // adjacency, loop). This mirrors how lexeme() suppresses skip for
    // its subtree. A skipper is therefore always written as a single
    // self-contained rule (typically *e); it cannot rely on auto-skip
    // itself — and that is the correct design.
    void run_skipper()
    {
        if (m_skip_enabled && m_skipper) {
            bool prev = m_skip_enabled;
            m_skip_enabled = false;
            // Transparent: discard the result. A skipper is always
            // written as *e (zero-or-more), so it always succeeds.
            m_skipper->parse(*this);
            m_skip_enabled = prev;
        }
    }

    // For Grammar::parse use only — stamps the Grammar-owned skipper
    // pointer onto this Context for the duration of one parse.
    void internal_set_skipper(const NonTerminalType* s) noexcept { m_skipper = s; }

    [[nodiscard]] bool has_skipper() const noexcept { return m_skipper != nullptr; }

    // Toggled by lexeme() to disable auto-skip for a subtree. Use
    // skip_enabled(prev) to restore the prior value (the lexeme wrapper
    // does this via ScopeGuard).
    void skip_enabled(bool e) noexcept { m_skip_enabled = e; }
    [[nodiscard]] bool skip_enabled() const noexcept { return m_skip_enabled; }

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

    // -----------------------------------------------------------------------
    // Multi-diagnostic accumulator.
    //
    // The furthest-failure path above keeps a single "best" diagnostic
    // (furthest-wins) — the right default for single-error reporting.
    // Production parsers (IDEs, linters) need to report many errors per
    // file: after a recoverable failure, the parser resyncs to a sync
    // token and continues, accumulating each recovered failure as its own
    // diagnostic.
    //
    // Append-only and unordered. Each recovered rule calls
    // record_diagnostic() once; the caller drains via take_diagnostics()
    // after the top-level parse returns. Independent of the furthest-wins
    // logic above.
    // -----------------------------------------------------------------------
    void record_diagnostic(Diagnostic diag) { m_diagnostics.push_back(std::move(diag)); }

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept
    {
        return m_diagnostics;
    }

    // Move all accumulated diagnostics out and clear the vector.
    [[nodiscard]] std::vector<Diagnostic> take_diagnostics() { return std::move(m_diagnostics); }

protected:
    std::unique_ptr<InputSourceBase<CharT>> m_input;
    // Non-null for contiguous (span) sources, null for paged (FileSource).
    // Cached at construction so current()/at() skip the virtual dispatch.
    const CharT* m_fast_data;
    std::size_t m_position = 0;
    std::size_t m_last_cut = 0;
    std::size_t m_input_size = 0;
    std::map<std::size_t, std::map<const NonTerminalType*, RuleState>> m_mem;
    std::stack<CutRecord> m_cut;

    // Error tracking state
    std::size_t m_furthest_failure_pos = 0;
    expected_set m_expected;
    bool m_has_error = false;

    // Multi-diagnostic accumulator. Parallel to the furthest-failure state
    // above; populated by record_diagnostic() at recovery points.
    std::vector<Diagnostic> m_diagnostics;

    // Skipper: a transparent rule invoked between adjacent sequence
    // elements and between repetition iterations. nullptr = no auto-skip.
    // Owned by Grammar; Context holds a non-owning pointer stamped at
    // Grammar::parse entry (see Grammar::set_skipper). Stays nullptr for
    // Contexts not driven through Grammar::parse, so run_skipper() is a
    // no-op in that case.
    const NonTerminalType* m_skipper = nullptr;
    // Toggled by lexeme() for the duration of its subtree so auto-skip
    // can be locally disabled. Save/restore via skip_enabled(bool) keeps
    // nesting safe (lexeme inside lexeme).
    bool m_skip_enabled = true;
};

template<typename CharT, std::size_t PageSize = 4096>
auto from_file(const std::string& path)
{
    return Context<CharT>(FileSource<CharT, PageSize>(path));
}

// CTAD: Context(someString) deduces to Context<char>, etc.
template<typename Range>
Context(const Range&) -> Context<typename Range::value_type>;

} // namespace peg
