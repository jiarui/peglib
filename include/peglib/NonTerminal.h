// NonTerminal: internal grammar-tree node with stable identity. Supports
// packrat memoization and left-recursion (seed-grow). NonTerminal is
// non-copyable — identity (the `this` pointer) is the memo key and seed-grow
// anchor. Users interact via Rule (a bare-pointer, non-owning handle), never
// directly with NonTerminal.
//
// Value/side-effect model (both run post-parse, in the fold, via parse_ast):
//   - parse() returns ParseResult { success, tree }: pure structure. Cached
//     in RuleState::m_cached_result; memo hits replay without re-parsing.
//   - m_typed_fold: value computation, set via RuleHandle::set_action.
//   - m_on_match: side-effect hook, set via Rule::on_match. Fires once per
//     committed-tree node.
//   Neither runs during parse, so backtracked matches never trigger them.
#pragma once
#include "peglib/ParserFwd.h"
#include "peglib/Recover.h"
#include "peglib/ResultType.h"

#include <cassert>
#include <memory>
#include <string>

namespace peg
{
namespace parsers
{

template<typename Context>
struct NonTerminal : ParsingExpr<Context, NonTerminal<Context>>
{
public:
    using ParseResult = typename ParsingExpr<Context, NonTerminal<Context>>::ParseResult;
    using ParseTreeNodePtr = typename ParsingExpr<Context, NonTerminal<Context>>::ParseTreeNodePtr;
    using NodeType = typename Context::node_type;

    using TypedFold = std::function<NodeType(Context&, const ParseTreeNodePtr&)>;
    void set_typed_fold(TypedFold f) { m_typed_fold = std::move(f); }
    [[nodiscard]] const TypedFold& typed_fold() const noexcept { return m_typed_fold; }

    using OnMatch = std::function<void(Context&, const ParseTreeNodePtr&)>;
    void set_on_match(OnMatch f) { m_on_match = std::move(f); }
    [[nodiscard]] const OnMatch& on_match() const noexcept { return m_on_match; }

    NonTerminal() = default;

    template<typename ExprType>
    NonTerminal(const ParsingExpr<Context, ExprType>& rhs)
        : m_rule(std::make_shared<ExprType>(static_cast<const ExprType&>(rhs)))
    {}

    NonTerminal(const NonTerminal&) = delete;
    NonTerminal& operator=(const NonTerminal&) = delete;

    template<typename ExprType>
    NonTerminal& operator=(const ParsingExpr<Context, ExprType>& rhs)
    {
        m_rule = std::make_shared<ExprType>(static_cast<const ExprType&>(rhs));
        return *this;
    }

    void set_name(std::string name) { m_name = std::move(name); }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    void set_label(std::string label) { m_label = std::move(label); }
    [[nodiscard]] const std::string& label() const noexcept { return m_label; }

    // Configure recovery. On body failure (with no cut committed at the
    // failure site), the rule scans forward to the next sync token, records a
    // diagnostic at the original failure position, consumes the sync token,
    // reports success with a transparent null tree, and continues from there.
    void set_recovery(RecoverSpec<typename Context::value_type> spec)
    {
        m_recover = std::move(spec);
    }

    [[nodiscard]] bool has_recovery() const noexcept { return m_recover.configured(); }
    [[nodiscard]] bool is_defined() const noexcept { return m_rule != nullptr; }

    // Debug-only lifetime aid: ~Grammar() poisons each NonTerminal's body
    // before releasing its owning shared_ptr. A dangling Rule handle (one
    // that outlived its Grammar) trips the assert in parseImpl instead of
    // silently using freed memory.
    void clear_body_for_debug() noexcept { m_rule.reset(); }

    ParseResult parse(Context& context) const override
    {
        auto start_pos = context.mark();
        auto [ok, rule_state] = context.rule_state(this, start_pos);

        if (!ok) {
            // (a) Left-recursive re-entry: `this` is on the LR stack at
            // start_pos. Mark this frame as the cycle's head, return the
            // CURRENT seed (live, not the stale snapshot).
            if (context.lr_in_progress(this, start_pos)) {
                for (auto* f = context.lr_top(); f != nullptr; f = f->next) {
                    if (f->pos == start_pos && f->rule == this) {
                        if (!f->is_head) {
                            f->is_head = true;
                            context.set_growing_head(start_pos, this);
                        }
                        break;
                    }
                }
                auto seed = context.memo_get(this, start_pos);
                context.reset(seed.m_last_pos);
                return seed.m_cached_result;
            }

            // (b) Involved in an active head's growth: don't grow
            // independently — return the current seed (live).
            const auto* head = context.growing_head(start_pos);
            if (head != nullptr && head != this) {
                auto seed = context.memo_get(this, start_pos);
                context.reset(seed.m_last_pos);
                return seed.m_cached_result;
            }

            // (c) Ordinary memo hit.
            context.reset(rule_state.m_last_pos);
            return rule_state.m_cached_result;
        }

        // First-time parse: track this rule on the LR invocation stack while
        // its body evaluates, so a left-recursive self-call can be detected.
        typename Context::LRFrame frame{this, start_pos, start_pos, false, context.lr_top()};
        context.lr_push(&frame);

        auto inner = parseImpl(context, start_pos, rule_state, frame);

        context.lr_pop(&frame);

        if (frame.is_head) {
            context.clear_growing_head(start_pos);
        }

        if (!inner.success) {
            if (!m_label.empty()) {
                context.record_failure(
                    start_pos, ExpectedItem{.kind = ExpectedKind::RuleLabel, .text = m_label});
            } else if (!m_name.empty()) {
                context.record_failure(
                    start_pos, ExpectedItem{.kind = ExpectedKind::RuleName, .text = m_name});
            }
            // Recovery: cut-committed failures are NOT recovered.
            if (m_recover.configured() && !context.cut()) {
                std::size_t scan = start_pos;
                while (scan < context.input_size() && !m_recover.is_sync_token(context.at(scan))) {
                    ++scan;
                }
                std::size_t resume_at =
                    (scan < context.input_size()) ? scan + 1 : context.input_size();
                context.reset(resume_at);
                context.record_diagnostic(
                    Diagnostic{start_pos,
                               {ExpectedItem{ExpectedKind::RuleLabel,
                                             m_recover.label.empty() ? m_name : m_recover.label}}});
                ParseResult recovered{true, nullptr};
                rule_state.m_cached_result = recovered;
                rule_state.m_last_pos = resume_at;
                context.update_rule_state(this, start_pos, rule_state);
                return recovered;
            }
            ParseResult fail{false, nullptr};
            rule_state.m_cached_result = fail;
            context.update_rule_state(this, start_pos, rule_state);
            return fail;
        }

        // Build this rule's tree node. If this rule has its own typed action,
        // wrap the body node as this rule's single child so the fold can
        // dispatch on THIS rule (not collapse onto the innermost producer).
        // Otherwise adopt the body node at zero cost (transparent passthrough
        // alias); producer is stamped only-if-none so it sticks at the
        // innermost action-bearing rule.
        ParseTreeNodePtr node;
        if (m_typed_fold) {
            node = context.make_node();
            node->name = m_name;
            node->producer = this;
            node->start_offset = start_pos;
            node->end_offset = context.mark();
            if (inner.tree)
                node->children.push_back(inner.tree);
        } else {
            node = inner.tree;
            if (!node)
                node = context.make_node();
            node->name = m_name;
            if (!node->producer)
                node->producer = this;
            node->start_offset = start_pos;
            node->end_offset = context.mark();
        }

        ParseResult result{true, node};
        rule_state.m_cached_result = result;
        context.update_rule_state(this, start_pos, rule_state);
        return result;
    }

    void collect_rule_refs(std::set<std::string>& refs) const override
    {
        if (m_rule)
            m_rule->collect_rule_refs(refs);
    }

protected:
    // Seed-grow loop (Warth §3.2). For an ordinary rule the first iteration
    // matches and the second makes no progress (and breaks). For a
    // left-recursive head (frame.is_head), each growth iteration also clears
    // the sibling memo entries at start_pos so involved partner rules in an
    // indirect/mutual cycle are re-driven against the grown seed.
    ParseResult parseImpl(Context& context,
                          std::size_t start_pos,
                          typename Context::RuleState& rule_state,
                          typename Context::LRFrame& frame) const
    {
        assert(m_rule && "NonTerminal::parse called on an unassigned rule");
        // Plant the failure seed: a left-recursive self-call during the body
        // parse must fail (not recurse forever).
        frame.last_pos = start_pos;
        rule_state.m_cached_result = ParseResult{};
        rule_state.m_last_pos = start_pos;
        context.update_rule_state(this, start_pos, rule_state);

        ParseResult best{false, nullptr};

        while (true) {
            context.reset(start_pos);
            if (frame.is_head && best.success) {
                context.clear_siblings_at(start_pos, this);
            }
            auto result = m_rule->parse(context);
            auto end_pos = context.mark();
            if (result.success) {
                if (end_pos > frame.last_pos) {
                    frame.last_pos = end_pos;
                    best = result;
                    rule_state.m_cached_result = result;
                    rule_state.m_last_pos = end_pos;
                    if (!context.update_rule_state(this, start_pos, rule_state)) {
                        break;
                    }
                } else {
                    // Fixed point. Adopt `result` only when `best` is still
                    // empty (first iteration matched zero-width — empty,
                    // lookahead, or any body that consumes nothing here).
                    // Otherwise keep `best`: a no-progress iteration cannot
                    // improve on the longest match, and may be a REGRESSED
                    // parse (shorter than `best`) — overwriting `best` with
                    // it silently drops the grown suffix.
                    if (!best.success) {
                        best = std::move(result);
                    }
                    rule_state.m_cached_result = best;
                    rule_state.m_last_pos = frame.last_pos;
                    context.update_rule_state(this, start_pos, rule_state);
                    break;
                }
            } else {
                break;
            }
        }
        context.reset(frame.last_pos);
        return best;
    }

protected:
    std::shared_ptr<ParsingExprInterface<Context>> m_rule;
    std::string m_name;
    std::string m_label;
    RecoverSpec<typename Context::value_type> m_recover;
    TypedFold m_typed_fold;
    OnMatch m_on_match;
};

template<typename Context, typename ExprType>
struct RuleHandle;

// Rule: non-owning handle to a NonTerminal, returned by Grammar::operator[].
// Stores a bare NonTerminal* (Grammar is the sole owner) plus a copied rule
// name. ~16 bytes on libstdc++: 8-byte pointer + SSO string.
//
// **Lifetime constraint**: a Rule cannot outlive its Grammar. After
// ~Grammar(), any outstanding Rule handle dangles (in debug builds the body
// is poisoned, so the next parse() hits the assert in parseImpl; under ASan
// the same misuse is caught as use-after-free). After Grammar move
// (Grammar g2 = std::move(g1)), Rule handles from g1 stay valid: shared_ptr
// move leaves the source map empty but the owned NonTerminal objects stay
// alive at the same addresses.
//
// Two assignment semantics:
//   operator=(ParsingExpr<...>) : assign a body to the underlying NonTerminal
//                                 (auto-names from m_name).
//   operator=(Rule rhs)         : alias — A's body delegates to rhs's
//                                 NonTerminal. Lazy: if rhs is reassigned
//                                 later, parsing A sees the update.
template<typename Context>
struct Rule : ParsingExpr<Context, Rule<Context>>
{
    using Impl = NonTerminal<Context>;
    using ParseResult = typename ParsingExpr<Context, Rule<Context>>::ParseResult;
    using OnMatch = typename Impl::OnMatch;

    Rule(Impl* impl, std::string name) : m_impl(impl), m_name(std::move(name)) {}

    Rule(const Rule&) = default;
    Rule(Rule&&) = default;
    // Copy/move *assignment* have alias semantics (see below), not the usual
    // shallow rebind. Expression trees only copy-construct Rule (via
    // std::make_tuple), never copy-assign.

    // Assign a body expression. Returns a typed RuleHandle whose set_action
    // does compile-time type checking against the body's result type.
    template<typename ExprType>
        requires(!std::same_as<std::remove_cvref_t<ExprType>, Rule<Context>>)
    auto operator=(const ParsingExpr<Context, ExprType>& rhs) -> RuleHandle<Context, ExprType>;

    // Alias assignment: make this rule delegate to rhs's NonTerminal.
    // NOTE: std::addressof is required because peglib overloads unary
    // operator& as the and-predicate combinator.
    auto operator=(const Rule& rhs) -> RuleHandle<Context, Rule<Context>>;
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    auto operator=(Rule&& rhs) -> RuleHandle<Context, Rule<Context>>;

    // Side-effect hook: void callback fired once per committed-tree node
    // during the post-parse fold (parse_ast only). For actions that observe
    // a match without producing a value (tokenization, tracing). Clear by
    // passing nullptr.
    Rule& on_match(OnMatch hook)
    {
        m_impl->set_on_match(std::move(hook));
        return *this;
    }

    Rule& set_label(std::string label)
    {
        m_impl->set_label(std::move(label));
        return *this;
    }

    Rule& set_recovery(RecoverSpec<typename Context::value_type> spec)
    {
        m_impl->set_recovery(std::move(spec));
        return *this;
    }

    [[nodiscard]] bool has_recovery() const noexcept { return m_impl->has_recovery(); }

    ParseResult parse(Context& context) const override { return m_impl->parse(context); }

    void collect_rule_refs(std::set<std::string>& refs) const override { refs.insert(m_name); }

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] const std::string& label() const noexcept { return m_impl->label(); }
    [[nodiscard]] bool is_defined() const noexcept { return m_impl->is_defined(); }

    [[nodiscard]] const Impl* impl() const noexcept { return m_impl; }
    [[nodiscard]] Impl* impl() noexcept { return m_impl; }

protected:
    Impl* m_impl;
    std::string m_name;
};

// RuleHandle<Context, ExprType>: the typed handle returned by
// `g["r"] = body`. Carries the body's static ExprType at the type level (no
// runtime storage), so set_action<F> can compile-time-check the action's
// parameter list against the body's derived result type and generate a bridge
// into the type-erased TypedFold stored on NonTerminal.
//
// Usage (the ONLY way to attach a typed value-computation):
//   auto h = (g["add"] = mul >> g.token('+') >> mul);
//   h.set_action([](Context& c, Span sp, AstNode l, AstNode r) { ... });
//
// Contrast with Rule::on_match (a void side-effect hook, not value). The two
// slots are independent: a rule may have either, both, or neither.
template<typename Context, typename ExprType>
struct RuleHandle
{
    using Impl = NonTerminal<Context>;
    using NodeType = typename Context::node_type;

    RuleHandle(Impl* impl, std::string name) : m_impl(impl), m_name(std::move(name)) {}

    operator Rule<Context>() const { return Rule<Context>{m_impl, m_name}; }

    // Typed value-computation attachment (the primary API). Compile-time
    // checks F is invocable as (Context&, Span, Args...) where Args is the
    // body's filtered result type, positionally unpacked:
    //   void body     → F(Context&, Span)
    //   single-result → F(Context&, Span, T)
    //   multi-result  → F(Context&, Span, T0, T1, ...)
    template<typename F>
    RuleHandle& set_action(F f)
    {
        using R = parsers::result_of_t<ExprType>;
        static_assert(parsers::action_matches<F, Context, parsers::flat_args_t<R>>,
                      "peglib: typed action signature does not match the rule body's "
                      "result type (positional, no projection). The action must be "
                      "invocable as F(Context&, Span, <body-result-args>...).");

        typename Impl::TypedFold typed_fold =
            [f = std::move(f)](Context& ctx,
                               const typename Context::ParseTreeNodePtr& n) -> NodeType {
            return parsers::
                fold_and_invoke<F, ExprType, Context, typename Context::ParseTreeNodePtr>(
                    f, ctx, n);
        };
        m_impl->set_typed_fold(std::move(typed_fold));
        return *this;
    }

    RuleHandle& set_label(std::string label)
    {
        m_impl->set_label(std::move(label));
        return *this;
    }

    RuleHandle& set_recovery(RecoverSpec<typename Context::value_type> spec)
    {
        m_impl->set_recovery(std::move(spec));
        return *this;
    }

    [[nodiscard]] bool has_recovery() const noexcept { return m_impl->has_recovery(); }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] bool is_defined() const noexcept { return m_impl->is_defined(); }
    [[nodiscard]] Impl* impl() const noexcept { return m_impl; }

protected:
    Impl* m_impl;
    std::string m_name;
};

template<typename Context>
template<typename ExprType>
    requires(!std::same_as<std::remove_cvref_t<ExprType>, Rule<Context>>)
auto Rule<Context>::operator=(const ParsingExpr<Context, ExprType>& rhs)
    -> RuleHandle<Context, ExprType>
{
    *m_impl = rhs;
    m_impl->set_name(m_name);
    return RuleHandle<Context, ExprType>{m_impl, m_name};
}

template<typename Context>
auto Rule<Context>::operator=(const Rule& rhs) -> RuleHandle<Context, Rule<Context>>
{
    if (this == std::addressof(rhs)) {
        return RuleHandle<Context, Rule<Context>>{m_impl, m_name};
    }
    *m_impl = rhs;
    m_impl->set_name(m_name);
    return RuleHandle<Context, Rule<Context>>{m_impl, m_name};
}

template<typename Context>
// NOLINTNEXTLINE(performance-noexcept-move-constructor)
auto Rule<Context>::operator=(Rule&& rhs) -> RuleHandle<Context, Rule<Context>>
{
    if (this == std::addressof(rhs)) {
        return RuleHandle<Context, Rule<Context>>{m_impl, m_name};
    }
    *m_impl = rhs;
    m_impl->set_name(m_name);
    return RuleHandle<Context, Rule<Context>>{m_impl, m_name};
}

} // namespace parsers
} // namespace peg
