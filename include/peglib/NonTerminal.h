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
// Post-parse action model:
//   parse() returns ParseResult { success, tree }. On the first successful
//   match at a given position, a ParseTreeNode is built from the body's
//   result tree, the semantic action is invoked (receiving the node so it
//   can read children->value), and the complete ParseResult is cached in
//   RuleState::m_cached_result. Subsequent memo hits return the cached
//   result directly — no action re-execution, no value stack, no conflict.
//
// Tree retention: the tree node is always kept (even if the action returns
//   a null value). Parent actions skip children whose ->value is null.
// ---------------------------------------------------------------------------
template<typename Context>
struct NonTerminal : ParsingExpr<Context, NonTerminal<Context>>
{
public:
    using ParseResult = typename ParsingExpr<Context, NonTerminal<Context>>::ParseResult;
    using ParseTreeNodePtr = typename ParsingExpr<Context, NonTerminal<Context>>::ParseTreeNodePtr;
    using NodeType = typename Context::node_type;

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
        node->start_offset = start_pos;
        node->end_offset = context.mark();

        // Execute semantic action (if any). The action receives the node
        // and can read node->children[i]->value to access sub-rule results.
        ParseResult result{true, node};
        if (this->m_action) {
            node->value = this->m_action(context, node);
        }
        // Note: even if the action returns null (transparent rule), we keep
        // the tree node. This lets parent actions reliably access children
        // by position. Parents that build user-facing AST skip children
        // whose ->value is null.

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
// DynSequenceExpr, etc.) store Rule copies by value (~40 bytes on libstdc++:
// 8-byte pointer + SSO string).
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
    using SemanticAction = typename ParsingExpr<Context, Rule<Context>>::SemanticAction;

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
    auto operator=(Rule&& rhs) -> RuleHandle<Context, Rule<Context>>;

    // Untyped semantic-action hook (dynamic-path escape hatch). The typed
    // action API lives on RuleHandle (the return value of operator=); to
    // attach a compile-time-checked action, capture the assignment result:
    //   auto h = (g["r"] = body);  h.set_action([](Context&, Span, ...){...});
    // This overload here is kept for the dynamic (GrammarCompiler) path and
    // for ad-hoc untyped binding.
    Rule& set_action(SemanticAction action)
    {
        m_impl->set_action(std::move(action));
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

    // Direct access to the underlying NonTerminal (for Grammar internals and
    // GrammarCompiler). Non-owning.
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
// against the body's derived result type and generate an extractor-backed
// bridge into the type-erased std::function<NodeType(Context&, ParseTreeNodePtr)>
// that NonTerminal::m_action stores.
//
// Usage (the ONLY way to attach a typed action):
//   auto h = (g["add"] = mul >> g.token('+') >> mul);
//   h.set_action([](Context& c, Span sp, AstNode l, AstNode r) { ... });
//
// Contrast with Rule (returned by g["r"]): Rule's set_action is untyped
// (the dynamic-path escape hatch). The two do not conflict — they are
// different types.
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
    using SemanticAction = typename ParsingExpr<Context, Rule<Context>>::SemanticAction;

    RuleHandle(Impl* impl, std::string name) : m_impl(impl), m_name(std::move(name)) {}

    // Implicit conversion to the untyped Rule view (for introspection, aliasing,
    // recovery, re-reference in another rule's body, etc.).
    operator Rule<Context>() const { return Rule<Context>{m_impl, m_name}; }

    // Typed semantic-action attachment.
    //
    // Compile-time checks that F is invocable as (Context&, Span, Args...) where
    // Args is the body's filtered result type, positionally unpacked:
    //   - void body        : F(Context&, Span)
    //   - single-result    : F(Context&, Span, T)
    //   - multi-result     : F(Context&, Span, T0, T1, ...)
    // On mismatch the static_assert below fires with a readable message.
    // Internally type-erases into a std::function<NodeType(Context&, ParseTreeNodePtr)>
    // via the extractor bridge — so NonTerminal's existing action-invocation
    // path (NonTerminal.h:153) and packrat memoisation are unchanged.
    template<typename F>
    RuleHandle& set_action(F f)
    {
        using R = parsers::result_of_t<ExprType>;
        // flat_args_t<R> (a free alias in ResultType.h) expands the collapsed
        // result back into the flat per-argument typelist the action must
        // accept: void→<>, scalar T→<T>, tuple<T...>→<T...>.
        static_assert(parsers::action_matches<F, Context, parsers::flat_args_t<R>>,
                      "peglib: typed action signature does not match the rule body's "
                      "result type (positional, no projection). The action must be "
                      "invocable as F(Context&, Span, <body-result-args>...).");

        // Type-erase: bridge closure that extract<ExprType> + invokes f.
        // NonTerminal's action-invocation path (NonTerminal.h:153) and packrat
        // memoisation are unchanged — the stored std::function has the same
        // signature as an untyped action.
        SemanticAction erased = [f = std::move(f)](Context& ctx,
                                                   const typename Context::ParseTreeNodePtr& n) -> NodeType {
            return parsers::invoke_action<F, ExprType, Context, typename Context::ParseTreeNodePtr>(
                f, ctx, n);
        };
        m_impl->set_action(std::move(erased));
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
