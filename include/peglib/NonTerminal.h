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

// ---------------------------------------------------------------------------
// NonTerminal: internal grammar-tree node with stable identity.
//
// Supports packrat memoization and left-recursion (seed-grow algorithm).
// NonTerminal is non-copyable — identity (the `this` pointer) is the memo
// key and seed-grow anchor. Users interact via Rule (a bare-pointer,
// non-owning handle), never directly with NonTerminal.
//
// Value/side-effect model (both run post-parse, in the fold, via parse_ast):
//   - parse() returns ParseResult { success, tree }: pure structure. The
//     complete ParseResult is cached in RuleState::m_cached_result; memo
//     hits replay the cached tree without re-parsing.
//   - A typed fold (m_typed_fold, set via RuleHandle::set_action) computes
//     the rule's value bottom-up, owning child values as locals.
//   - A side-effect hook (m_on_match, set via Rule::on_match) fires once per
//     committed-tree node for observational actions.
//   Neither runs during parse, so backtracked matches never trigger them.
// ---------------------------------------------------------------------------
template<typename Context>
struct NonTerminal : ParsingExpr<Context, NonTerminal<Context>>
{
public:
    using ParseResult = typename ParsingExpr<Context, NonTerminal<Context>>::ParseResult;
    using ParseTreeNodePtr = typename ParsingExpr<Context, NonTerminal<Context>>::ParseTreeNodePtr;
    using NodeType = typename Context::node_type;

    // Typed fold: the post-parse value builder registered by
    // RuleHandle::set_action<F>. Invoked by the fold driver (ResultType.h
    // fold_rule / fold_start) on each node whose producer points here.
    using TypedFold = std::function<NodeType(Context&, const ParseTreeNodePtr&)>;
    void set_typed_fold(TypedFold f) { m_typed_fold = std::move(f); }
    [[nodiscard]] const TypedFold& typed_fold() const noexcept { return m_typed_fold; }

    // Side-effect hook: a void callback fired once per committed-tree node
    // during the post-parse fold (parse_ast only). Use it for actions that
    // observe a match without producing a value (tokenization, tracing,
    // symbol-table population). Never fires for backtracked matches — the
    // fold visits only the accepted tree.
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

    // Assign a body expression. The typed fold is registered separately by
    // RuleHandle::set_action (which captures the body's static ExprType). A
    // rule with no typed fold is transparent: the fold follows node->producer
    // to an action-bearing rule, or yields a default-constructed NodeType.
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

    // Configure recovery for this rule. When the body fails AND no cut is
    // committed at the failure site, the rule scans forward to the next
    // sync token (per spec.is_sync_token), records a diagnostic at the
    // original failure position, consumes the sync token, and reports
    // success with a transparent null tree. Parsing then continues from
    // the sync point.
    void set_recovery(RecoverSpec<typename Context::value_type> spec)
    {
        m_recover = std::move(spec);
    }

    [[nodiscard]] bool has_recovery() const noexcept { return m_recover.configured(); }

    [[nodiscard]] bool is_defined() const noexcept { return m_rule != nullptr; }

    // Debug-only lifetime aid: ~Grammar() calls this on every NonTerminal
    // before releasing its owning shared_ptr. Clearing the body makes any
    // dangling Rule handle (one that outlived its Grammar) trip the
    // `assert(m_rule && ...)` in parseImpl instead of silently calling into
    // freed memory. No-op in release builds (NDEBUG defined).
    void clear_body_for_debug() noexcept { m_rule.reset(); }

    ParseResult parse(Context& context) const override
    {
        auto start_pos = context.mark();
        auto [ok, rule_state] = context.rule_state(this, start_pos);

        if (!ok) {
            // Memo hit: return the cached result (tree + action value).
            context.reset(rule_state.m_last_pos);
            return rule_state.m_cached_result;
        }

        // First-time parse: seed-grow loop to find the longest match.
        auto inner = parseImpl(context, start_pos, rule_state);

        if (!inner.success) {
            if (!m_label.empty()) {
                context.record_failure(
                    start_pos, ExpectedItem{.kind = ExpectedKind::RuleLabel, .text = m_label});
            } else if (!m_name.empty()) {
                context.record_failure(
                    start_pos, ExpectedItem{.kind = ExpectedKind::RuleName, .text = m_name});
            }
            // Recovery: if a RecoverSpec is configured and no cut is
            // committed at this site, resync to the next sync token and
            // report success with a transparent null tree. Cut-committed
            // failures are NOT recovered — cut is an explicit programmer
            // commitment that this branch must succeed.
            if (m_recover.configured() && !context.cut()) {
                std::size_t scan = start_pos;
                while (scan < context.input_size() && !m_recover.is_sync_token(context.at(scan))) {
                    ++scan;
                }
                // Position past the sync token if one was found; at EOF
                // otherwise (recover_eof reaches here with scan == size).
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

        // Use the body's tree directly as this rule's tree. If the body
        // produced no tree (e.g. only predicates/terminals), create a fresh
        // node. The action can read body sub-results from node->children.
        auto node = inner.tree;
        if (!node) {
            node = std::make_shared<typename Context::ParseTreeNode>();
        }
        node->name = m_name;
        // Stamp producer only if the node doesn't already have one: a rule
        // that adopts its body's node (alias, alternation-passthrough) must
        // NOT clobber the innermost producer, or the typed fold loses the
        // reference to the rule whose action built the value.
        if (!node->producer)
            node->producer = this;
        node->start_offset = start_pos;
        node->end_offset = context.mark();

        // parse() is pure structure: it builds and caches the tree. Value
        // computation (m_typed_fold) and side-effect hooks (m_on_match) run
        // later, in the post-parse fold (parse_ast → fold_start/fold_rule).
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
    ParseResult parseImpl(Context& context,
                          std::size_t start_pos,
                          typename Context::RuleState& rule_state) const
    {
        assert(m_rule && "NonTerminal::parse called on an unassigned rule");
        auto current_pos = context.mark();
        context.update_rule_state(this, start_pos, rule_state);

        ParseResult best{false, nullptr};

        while (true) {
            context.reset(current_pos);
            auto result = m_rule->parse(context);
            auto end_pos = context.mark();
            if (result.success) {
                if (end_pos > rule_state.m_last_pos) {
                    rule_state.m_last_pos = end_pos;
                    best = result;
                    // Cache intermediate result so recursive memo hits
                    // during seed-grow see the latest successful match.
                    rule_state.m_cached_result = result;
                    if (!context.update_rule_state(this, start_pos, rule_state)) {
                        break;
                    }
                } else {
                    best = std::move(result);
                    rule_state.m_cached_result = best;
                    context.update_rule_state(this, start_pos, rule_state);
                    break;
                }
            } else {
                break;
            }
        }
        context.reset(rule_state.m_last_pos);
        return best;
    }

protected:
    std::shared_ptr<ParsingExprInterface<Context>> m_rule;
    std::string m_name;
    std::string m_label;
    RecoverSpec<typename Context::value_type> m_recover;
    TypedFold m_typed_fold; // value computation; set via RuleHandle::set_action
    OnMatch m_on_match;     // side-effect hook; set via Rule::on_match
};

// Forward declaration: Rule::operator= returns RuleHandle (defined after Rule).
template<typename Context, typename ExprType>
struct RuleHandle;

// ---------------------------------------------------------------------------
// Rule: non-owning handle to a NonTerminal, returned by Grammar::operator[].
//
// Stores a bare NonTerminal* (Grammar is the sole owner via shared_ptr) plus
// a copied rule name (std::string). Copy is shallow — multiple Rule copies
// can point to the same NonTerminal. Expression trees (SequenceExpr,
// AlternationExpr, Repetition, etc.) store Rule copies by value (~40 bytes on
// libstdc++: 8-byte pointer + SSO string).
//
// **Design constraint**: a Rule cannot outlive its Grammar. This is
// intentional — it eliminates shared_ptr cycles at the source (recursive
// grammars no longer form reference cycles, so ~Grammar needs no special
// handling).
//
// **Lifetime of handles**:
//   - After `~Grammar()`, any outstanding Rule handle dangles. In debug
//     builds `~Grammar()` poisons each NonTerminal's body, so a dangling
//     Rule's next parse() hits `assert(m_rule && ...)` in parseImpl; under
//     ASan the same misuse is caught as a use-after-free.
//   - After `Grammar` move (`Grammar g2 = std::move(g1)`), Rule handles
//     obtained from `g1` *remain valid*: `shared_ptr` move leaves the source
//     map empty but the owned NonTerminal objects stay alive (re-homed in
//     `g2`) at the same addresses, so the bare pointers in existing Rule
//     handles still resolve correctly.
//
// Two assignment semantics:
//   - operator=(ParsingExpr<...>) : assign a body to the underlying
//     NonTerminal (auto-names from m_name).
//   - operator=(Rule rhs)         : alias — make this rule's body delegate to
//     rhs's NonTerminal (g["A"] = g["B"] makes A an alias for B).
// ---------------------------------------------------------------------------
template<typename Context>
struct Rule : ParsingExpr<Context, Rule<Context>>
{
    using Impl = NonTerminal<Context>;
    using ParseResult = typename ParsingExpr<Context, Rule<Context>>::ParseResult;
    using OnMatch = typename Impl::OnMatch;

    Rule(Impl* impl, std::string name) : m_impl(impl), m_name(std::move(name)) {}

    Rule(const Rule&) = default;
    Rule(Rule&&) = default;
    // NOTE: copy/move *assignment* have alias semantics (see below), not
    // the usual shallow rebind. This is because Rule handles are views
    // returned by Grammar::operator[]; assigning one view to another
    // mutates the underlying NonTerminal, not the view itself. Expression
    // trees only copy-construct Rule (via std::make_tuple), never
    // copy-assign, so this is safe.

    // Assign a body expression to the underlying NonTerminal. Auto-names
    // the rule from m_name. Returns a RuleHandle carrying the body's static
    // type — its set_action<F> does compile-time type checking against the
    // body's result type and bridges into the type-erased storage.
    template<typename ExprType>
        requires(!std::same_as<std::remove_cvref_t<ExprType>, Rule<Context>>)
    auto operator=(const ParsingExpr<Context, ExprType>& rhs) -> RuleHandle<Context, ExprType>;

    // Alias assignment: make this rule's body delegate to rhs's NonTerminal.
    // The body becomes a Rule copy pointing at rhs's NonTerminal. This is
    // **lazy**: if rhs's body is later reassigned, parsing this rule sees
    // the update (because parsing delegates to rhs's NonTerminal, not a
    // snapshot of its body).
    // Example: g["A"] = g["B"];  // A delegates to B
    // NOTE: std::addressof is required because peglib overloads unary
    // operator& as the and-predicate combinator (Rule.h), so &rhs would
    // build an AndExpr rather than yielding a Rule*.
    auto operator=(const Rule& rhs) -> RuleHandle<Context, Rule<Context>>;
    // Not noexcept: NonTerminal::template operator= does make_shared (can
    // throw bad_alloc), and set_name does a std::string assign. Same throwing
    // nature as the legacy Rule copy/move assignment (documented & accepted).
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    auto operator=(Rule&& rhs) -> RuleHandle<Context, Rule<Context>>;

    // Side-effect hook: register a void callback fired once per committed-
    // tree node during the post-parse fold (parse_ast only). For actions
    // that observe a match without producing a value (tokenization,
    // tracing). The typed value-computation API lives on RuleHandle (the
    // return value of operator=):
    //   auto h = (g["r"] = body);  h.set_action([](Context&, Span, ...){...});
    // Clear the hook by passing nullptr (e.g. for teardown).
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

    // Configure recovery on the underlying NonTerminal.
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

    // Direct access to the underlying NonTerminal (for Grammar internals).
    // Non-owning.
    [[nodiscard]] const Impl* impl() const noexcept { return m_impl; }
    [[nodiscard]] Impl* impl() noexcept { return m_impl; }

protected:
    Impl* m_impl;
    std::string m_name;
};

// ---------------------------------------------------------------------------
// RuleHandle<Context, ExprType>: the typed handle returned by `g["r"] = body`.
//
// Carries the body's static ExprType at the type level (no runtime storage),
// so its set_action<F> can compile-time-check the action's parameter list
// against the body's derived result type and generate a bridge into the
// type-erased TypedFold stored on NonTerminal::m_typed_fold.
//
// Usage (the ONLY way to attach a typed value-computation):
//   auto h = (g["add"] = mul >> g.token('+') >> mul);
//   h.set_action([](Context& c, Span sp, AstNode l, AstNode r) { ... });
//
// Contrast with Rule (returned by g["r"]): Rule::on_match attaches a void
// side-effect hook (NonTerminal::m_on_match), not a value computation. The
// two slots are independent: a rule may have either, both, or neither.
//
// RuleHandle is a view: it does not own the NonTerminal (Grammar does). It
// implicitly converts to Rule so all Rule's introspection/recovery methods
// remain available on a handle.
// ---------------------------------------------------------------------------
template<typename Context, typename ExprType>
struct RuleHandle
{
    using Impl = NonTerminal<Context>;
    using NodeType = typename Context::node_type;

    RuleHandle(Impl* impl, std::string name) : m_impl(impl), m_name(std::move(name)) {}

    // Implicit conversion to the Rule view (for introspection, aliasing,
    // recovery, re-reference in another rule's body, etc.).
    operator Rule<Context>() const { return Rule<Context>{m_impl, m_name}; }

    // Typed value-computation attachment (the primary API).
    //
    // Compile-time checks F is invocable as (Context&, Span, Args...) where
    // Args is the body's filtered result type, positionally unpacked:
    //   - void body     : F(Context&, Span)
    //   - single-result : F(Context&, Span, T)
    //   - multi-result  : F(Context&, Span, T0, T1, ...)
    //
    // Registers the typed fold on the underlying NonTerminal. It does NOT run
    // during parse; it runs in the post-parse fold (Grammar::parse_ast →
    // fold_start → this fold), where child values are owned locals moved up
    // once — unconditionally move-safe. Consumers retrieve results via
    // parse_ast. (For side-effects without a value, use Rule::on_match.)
    // Parse/memo/cut/recovery are unchanged.
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

// ---------------------------------------------------------------------------
// Rule::operator= definitions (declared inside Rule; return RuleHandle).
// ---------------------------------------------------------------------------
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
        // self-alias would create a cycle; just return a handle to self.
        return RuleHandle<Context, Rule<Context>>{m_impl, m_name};
    }
    *m_impl = rhs; // NonTerminal template operator= stores a Rule body
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
