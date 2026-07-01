// Context: the parse state carried through every expression's parse() call.
//
// Three orthogonal axes are cleanly separated:
//   - CharT   : element type (char, char32_t, ...).
//   - NodeType: semantic-action product type stored on ParseTreeNode. Default
//               std::monostate; value type for lightweight products; move-only
//               type for recursive ASTs; shared_ptr<T> for polymorphic ASTs.
//   - Source  : input storage strategy. Erased behind InputSourceBase;
//               SpanSource (contiguous, zero-virtual-call hot path) or
//               FileSourceSource (paged, cut-evictable). Selected at
//               construction, invisible to the template signature.
#pragma once
#include <cassert>
#include <concepts>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stack>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "InputSource.h"
#include "ParseError.h"
namespace peg
{

// Span: offset pair describing a match. start/end mirror
// ParseTreeNode::start_offset/end_offset; semantics set by Context::value_type
// (byte offsets for char-level, token indices for token-level).
struct Span
{
    std::size_t start{};
    std::size_t end{};
};

namespace parsers
{
template<typename Context>
struct ParsingExprInterface;
template<typename Context>
struct NonTerminal;
template<typename Context>
struct Rule;
} // namespace parsers

template<typename CharT, typename NodeType = std::monostate>
struct Context
{
    using node_type = NodeType;
    using value_type = CharT;
    using char_type = CharT;

    // -----------------------------------------------------------------------
    // Lifetime.
    //
    // (1) From a contiguous range: stores a non-owning SpanSource pointing
    //     into `t`. Caller must keep the input alive for the Context's
    //     lifetime. **Passing a temporary here dangles silently.** For a
    //     self-contained copy, use Grammar::parse_string.
    //
    // (2) From a FileSource rvalue: takes ownership (moved into a
    //     FileSourceSource adapter). No lifetime obligation on the caller.
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

    // Move is allowed (e.g. from from_file); copy is not — copying mid-parse
    // would duplicate memo entries keyed by raw NonTerminal* and silently
    // corrupt furthest-error state.
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) noexcept = default;
    Context& operator=(Context&&) noexcept = default;

    using NonTerminalType = peg::parsers::NonTerminal<Context<CharT, NodeType>>;

    // Immutable record of a successful match. Pure structure: name, offsets,
    // children, dispatch metadata. No value slot — the typed fold (parse_ast)
    // owns computed values as locals and moves them up, which is what makes a
    // move-only NodeType safe.
    //
    // Lifetime: ParseTreeNode is owned by the Context's arena (m_node_arena);
    // every other reference — children, the packrat memo (RuleState), and the
    // tree returned by parse_tree() — is a NON-OWNING observer valid for the
    // Context's lifetime. This replaces the old shared_ptr model: the sharing
    // (memo ↔ tree, parent ↔ child) was lifetime-only, with no mutation after
    // build, so a single owner + observers expresses it correctly and removes
    // the per-node refcount machinery. The returned tree must not outlive its
    // Context (parse_ast folds it away into a context-independent value, so
    // this only matters for direct parse_tree() callers, who use it in-scope).
    struct ParseTreeNode
    {
        // Interned: a non-owning view into the producing NonTerminal's m_name
        // (which lives in the Grammar and outlives this node's Context). Saves
        // the per-node std::string copy/move that the old `std::string name`
        // paid on every committed node — rule names are a small fixed set in
        // the Grammar, so each node just observes its producer's name rather
        // than owning a copy. Empty for anonymous combinator nodes.
        std::string_view name;
        std::size_t start_offset = 0;
        std::size_t end_offset = 0;
        std::vector<ParseTreeNode*> children;
        // Producer rule (typed-fold dispatch). Stamped by NonTerminal::parse
        // so the post-parse typed fold can find each node's registered fold
        // via pointer identity. Null for anonymous combinator nodes and for
        // transparent rules with no typed fold.
        const NonTerminalType* producer = nullptr;
        // Winning-branch index for an AlternationExpr's node (the node IS the
        // winner's node, passed through). Stamped by parseAlt so the typed
        // fold can dispatch on the actual winning branch's static type at
        // runtime. SIZE_MAX = not an alternation winner.
        std::size_t alt_winner = static_cast<std::size_t>(-1);
    };
    using ParseTreeNodePtr = ParseTreeNode*;

    struct ParseResult
    {
        bool success = false;
        ParseTreeNodePtr tree = nullptr;
        explicit operator bool() const { return success; }
    };

    struct RuleState
    {
        RuleState() = default;
        RuleState(const RuleState&) = default;
        RuleState& operator=(const RuleState&) = default;
        // End position of the cached result (genuinely part of the cached
        // ANSWER: a recovered/null-tree result ends at an arbitrary resync
        // position, not derivable from the tree).
        std::size_t m_last_pos = 0;
        ParseResult m_cached_result;
    };

    // Transient left-recursion control state, one per NonTerminal currently on
    // the call stack at a position. Stores NO parse answers (those live in the
    // memo); `last_pos` is the grow-loop progress marker, kept here (transient)
    // rather than in RuleState so the memo is not churned every growth
    // iteration. Lifetime is the C++ call — pushed at first-time entry, popped
    // on return; zero heap allocation.
    struct LRFrame
    {
        const NonTerminalType* rule;
        std::size_t pos;
        std::size_t last_pos;
        bool is_head;
        LRFrame* next;
    };

    struct State
    {
        explicit State(std::size_t pos) : m_pos(pos) {}
        std::size_t m_pos;
        friend bool operator==(const State& lhs, const State& rhs)
        {
            return lhs.m_pos == rhs.m_pos;
        }
    };

    std::size_t input_size() const noexcept { return m_input_size; }
    State state() { return State{m_position}; }
    void state(const State& state) { m_position = state.m_pos; }
    bool ended() const noexcept { return m_position >= m_input_size; }
    std::size_t mark() const noexcept { return m_position; }

    // Per-character access. Uses the contiguous cache when available (zero
    // virtual dispatch); falls back to the virtual at() for paged sources.
    value_type current() const
    {
        assert(m_position < m_input_size && "current() past end of input");
        if (m_fast_data) {
            return m_fast_data[m_position];
        }
        return m_input->at(m_position);
    }
    value_type at(std::size_t offset) const
    {
        assert(offset < m_input_size && "at() past end of input");
        if (m_fast_data) {
            return m_fast_data[offset];
        }
        return m_input->at(offset);
    }

    [[nodiscard]] InputSourceBase<CharT>& input() const noexcept { return *m_input; }

    // Allocate a fresh ParseTreeNode owned by this Context's arena. The node
    // is value-initialized; the caller fills in its fields and the node lives
    // until the Context is destroyed (no per-node free — a monotonic pool).
    // On a failed branch the node simply becomes unreachable garbage in the
    // arena, which is correctness-neutral (the standard arena high-water-mark
    // tradeoff) and avoids the alloc/free churn that the make_shared model
    // paid on every speculative combinator node.
    ParseTreeNode* make_node() { return &m_node_arena.emplace_back(); }

    void next() noexcept
    {
        if (m_position < m_input_size) {
            ++m_position;
        }
    }

    void reset(std::size_t pos) noexcept
    {
        // Upper bound enforced. The lower bound (m_last_cut) is NOT enforced:
        // after a cut, memo data for earlier positions has been intentionally
        // released, but it is still valid to rewind there and re-parse.
        assert(pos <= m_input_size && "reset past end of input");
        m_position = pos;
    }

    std::tuple<bool, RuleState> rule_state(const NonTerminalType* rule, std::size_t pos)
    {
        // try_emplace value-initializes an empty inner map on a miss without
        // naming its (now unordered_map) type, so the body stays agnostic to
        // the inner container choice.
        auto [iter_records, ins] = m_mem.try_emplace(pos);
        auto [iter, ok] = iter_records->second.emplace(rule, RuleState{});
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
        memo->second = rule_state;
        return true;
    }

    // Read the CURRENT cached RuleState for (rule, pos) live from the memo
    // map (not a stale snapshot). Used by left-recursive re-entry to return
    // the freshly-grown seed. Returns a default state if no entry exists.
    RuleState memo_get(const NonTerminalType* rule, std::size_t pos) const
    {
        auto memos = m_mem.find(pos);
        if (memos == m_mem.end()) {
            return RuleState{};
        }
        auto it = memos->second.find(rule);
        if (it == memos->second.end()) {
            return RuleState{};
        }
        return it->second;
    }

    // -----------------------------------------------------------------------
    // Left-recursion invocation stack + active-head index (transient).
    // -----------------------------------------------------------------------

    LRFrame* lr_push(LRFrame* frame) noexcept
    {
        frame->next = m_lr_stack;
        m_lr_stack = frame;
        return frame;
    }

    [[nodiscard]] LRFrame* lr_top() noexcept { return m_lr_stack; }
    [[nodiscard]] const LRFrame* lr_top() const noexcept { return m_lr_stack; }

    void lr_pop(LRFrame* frame) noexcept
    {
        assert(m_lr_stack == frame && "lr_pop out of order");
        m_lr_stack = frame->next;
    }

    // Is `rule` already being evaluated at `pos`? Scans the LR stack. True ⇒
    // this application is (directly or indirectly) left-recursive.
    bool lr_in_progress(const NonTerminalType* rule, std::size_t pos) const noexcept
    {
        for (const LRFrame* f = m_lr_stack; f != nullptr; f = f->next) {
            if (f->pos == pos && f->rule == rule) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const NonTerminalType* growing_head(std::size_t pos) const noexcept
    {
        auto it = m_growing_head.find(pos);
        return it == m_growing_head.end() ? nullptr : it->second;
    }

    void set_growing_head(std::size_t pos, const NonTerminalType* rule)
    {
        m_growing_head[pos] = rule;
    }

    void clear_growing_head(std::size_t pos) noexcept { m_growing_head.erase(pos); }

    // Clear every memo entry at `pos` except `keep` AND except any rule
    // currently on the LR stack at `pos`. Called by a head's growth loop at
    // the start of each growth iteration so sibling (involved) rules are
    // re-evaluated against the head's freshly-grown seed instead of returning
    // a frozen result. The stack-resident exclusion is essential: a rule
    // mid-evaluation up the call chain must NOT have its memo dropped.
    void clear_siblings_at(std::size_t pos, const NonTerminalType* keep)
    {
        auto memos = m_mem.find(pos);
        if (memos == m_mem.end()) {
            return;
        }
        for (auto it = memos->second.begin(); it != memos->second.end();) {
            const NonTerminalType* rule = it->first;
            if (rule == keep || lr_in_progress(rule, pos)) {
                ++it;
            } else {
                it = memos->second.erase(it);
            }
        }
    }

    struct CutRecord
    {
        std::size_t pos;
        bool cut = false;
        CutRecord(std::size_t i, bool c) : pos{i}, cut{c} {}
    };

    // Set the current scope's cut flag. No-op when the cut stack is empty (a
    // cut appearing outside any Alternation/Repetition scope has no scope to
    // commit — the flag is dropped). Keeps standalone cut() / `~` safe.
    void cut(bool c)
    {
        if (m_cut.empty())
            return;
        m_cut.top().cut = c;
        m_cut.top().pos = mark();
    }

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
            m_input->release_before(m_last_cut);
        }
        m_cut.pop();
    }

    // -----------------------------------------------------------------------
    // Auto-skip. The skipper fires between adjacent sequence children and
    // between repetition iterations; nowhere else (not before first, not
    // after last, not inside Alternatives / predicates / terminal-seq
    // literals). End users drive skip via Grammar::set_skipper / lexeme().
    // -----------------------------------------------------------------------

    // Reentrancy guard: while the skipper runs, skip_enabled is temporarily
    // cleared so the skipper's own internal Repetition / Sequence children do
    // not recursively invoke run_skipper() (which would double-consume). A
    // skipper is therefore a single self-contained rule (typically *e) and
    // cannot rely on auto-skip itself.
    void run_skipper()
    {
        if (m_skip_enabled && m_skipper) {
            bool prev = m_skip_enabled;
            m_skip_enabled = false;
            m_skipper->parse(*this);
            m_skip_enabled = prev;
        }
    }

    void internal_set_skipper(const NonTerminalType* s) noexcept { m_skipper = s; }
    [[nodiscard]] bool has_skipper() const noexcept { return m_skipper != nullptr; }

    void skip_enabled(bool e) noexcept { m_skip_enabled = e; }
    [[nodiscard]] bool skip_enabled() const noexcept { return m_skip_enabled; }

    // -----------------------------------------------------------------------
    // Error tracking: furthest-failure position + expected set
    // -----------------------------------------------------------------------

    using expected_set = ExpectedSet;

    // Furthest-wins / same-position-accumulates / earlier-ignored.
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
    }

    // Lazy variant: the ExpectedItem is produced by `producer` ONLY if `pos` is
    // furthest-or-tied (i.e. it would actually be retained). This is the common
    // case under ordered-choice backtracking: terminal/token failures happen at
    // every non-matching branch, but only the ones at the furthest position are
    // kept — so building the escaped display string eagerly (the old path
    // through record_terminal_expected) wasted a std::string allocation +
    // snprintf/escape work on every discarded failure. Callers that already
    // have a cheap ExpectedItem should keep using the eager overload above.
    template<typename Producer>
        requires std::invocable<Producer&> &&
                 std::convertible_to<std::invoke_result_t<Producer&>, ExpectedItem>
    void record_failure_lazy(std::size_t pos, Producer producer)
    {
        if (!m_has_error || pos > m_furthest_failure_pos) {
            m_furthest_failure_pos = pos;
            m_expected.clear();
            m_expected.insert(producer());
            m_has_error = true;
        } else if (pos == m_furthest_failure_pos) {
            m_expected.insert(producer());
        }
        // pos < furthest: producer never invoked — string never built.
    }

    [[nodiscard]] std::size_t furthest_failure_pos() const noexcept
    {
        return m_furthest_failure_pos;
    }
    [[nodiscard]] const expected_set& expected() const noexcept { return m_expected; }
    [[nodiscard]] bool has_error() const noexcept { return m_has_error; }

    // Move the error out as a Diagnostic. After this call, has_error() is false.
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
    // Multi-diagnostic accumulator (parallel to the furthest-failure path
    // above). Production parsers report many errors per file: after a
    // recoverable failure, the parser resyncs to a sync token and continues,
    // accumulating each recovered failure as its own diagnostic.
    // -----------------------------------------------------------------------

    void record_diagnostic(Diagnostic diag) { m_diagnostics.push_back(std::move(diag)); }

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept
    {
        return m_diagnostics;
    }
    [[nodiscard]] std::vector<Diagnostic> take_diagnostics() { return std::move(m_diagnostics); }

protected:
    std::unique_ptr<InputSourceBase<CharT>> m_input;
    const CharT* m_fast_data;
    std::size_t m_position = 0;
    std::size_t m_last_cut = 0;
    std::size_t m_input_size = 0;
    // Node arena: owns every ParseTreeNode for this parse's lifetime. A deque
    // gives stable element addresses across growth and frees all nodes on
    // Context destruction with no per-node deallocation. See make_node().
    std::deque<ParseTreeNode> m_node_arena;
    // Packrat memo. Two-level, keyed by (position → rule*). Both layers are
    // hash maps rather than red-black trees: the callgrind baseline showed
    // std::map node allocation + descent as the single largest hotspot
    // (~30% of instruction refs) — two RB-tree node allocs per first-time
    // (rule, pos) entry and two descents per lookup. unordered_map trades
    // that for one hash probe + amortized bucket storage per layer, with no
    // per-entry heap node on a hit. The two-level shape (not a single flat
    // (pos, rule*) map) is deliberate: clear_siblings_at must iterate every
    // rule AT a given position each left-recursion growth iteration, and a
    // flat map would make that a full-table scan (O(total entries) per
    // growth step → quadratic on left-recursive grammars).
    std::unordered_map<std::size_t, std::unordered_map<const NonTerminalType*, RuleState>> m_mem;
    std::stack<CutRecord> m_cut;

    LRFrame* m_lr_stack = nullptr;
    std::unordered_map<std::size_t, const NonTerminalType*> m_growing_head;

    std::size_t m_furthest_failure_pos = 0;
    expected_set m_expected;
    bool m_has_error = false;

    std::vector<Diagnostic> m_diagnostics;

    const NonTerminalType* m_skipper = nullptr;
    bool m_skip_enabled = true;
};

template<typename CharT, std::size_t PageSize = 4096>
auto from_file(const std::string& path)
{
    return Context<CharT>(FileSource<CharT, PageSize>(path));
}

template<typename Range>
Context(const Range&) -> Context<typename Range::value_type>;

} // namespace peg
