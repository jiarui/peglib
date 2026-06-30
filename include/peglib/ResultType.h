#pragma once

// ===========================================================================
// ResultType: compile-time result-type derivation + post-parse typed fold for
// typed semantic actions.
//
//   - peg::Span                       : (defined in Context.h) the span handed
//                                       to every typed action and matcher fn.
//   - peg::parsers::result_of<E>      : compile-time result type of expression E.
//   - peg::parsers::seq_result<Ts...> : filter-void + 1-tuple-unpack collapse.
//   - peg::parsers::action_matches    : concept checking F is invocable as
//                                       (C&, Span, Args...) with Args derived
//                                       positionally from E's result type.
//   - peg::parsers::fold<E>(ctx,node) : post-parse typed value builder (below).
//
// Terminal result model:
//   - terminal/terminal-seq/empty/cut/And/Not → void (filtered from sequences).
//   - MatcherExpr → void (a recognizer; what it matched is observable via
//     on_match reading the node's span, not via the typed fold).
//   - TokenExpr → value_type (the matched element, kept).
//   - NonTerminal/Rule → node_type.
// ===========================================================================

#include "peglib/Combinators.h"
#include "peglib/NonTerminal.h"
#include "peglib/Terminals.h"

#include <concepts>
#include <cstddef>
#include <optional>
#include <tuple>
#include <type_traits>
#include <vector>

namespace peg
{
// peg::Span is defined in Context.h (available wherever ParseTreeNode is).
namespace parsers
{
namespace detail
{
// -------------------------------------------------------------------------
// typelist: the lightweight compile-time list used by the metaprogramming
// below. We keep values out of it — it is a pure type container.
// -------------------------------------------------------------------------
template<typename... Ts>
struct typelist
{
    static constexpr std::size_t size = sizeof...(Ts);
};

// Concatenate two typelists.
template<typename L, typename R>
struct concat;
template<typename... As, typename... Bs>
struct concat<typelist<As...>, typelist<Bs...>>
{
    using type = typelist<As..., Bs...>;
};
template<typename L, typename R>
using concat_t = typename concat<L, R>::type;

// Filter `void` out of a typelist.
template<typename List>
struct filter_void;
template<>
struct filter_void<typelist<>>
{
    using type = typelist<>;
};
template<typename Head, typename... Tail>
struct filter_void<typelist<Head, Tail...>>
{
    using tail_filtered = typename filter_void<typelist<Tail...>>::type;
    using type = std::conditional_t<std::is_same_v<Head, void>,
                                    tail_filtered,
                                    concat_t<typelist<Head>, tail_filtered>>;
};
template<typename List>
using filter_void_t = typename filter_void<List>::type;

// Collapse a filtered typelist into the "sequence result" shape:
//   0 elements → void
//   1 element  → T          (unwrap; e.g. `*(term >> rule)` iteration yields T)
//   ≥2 elements→ std::tuple<T...>
template<typename List>
struct collapse;
template<>
struct collapse<typelist<>>
{
    using type = void;
};
template<typename T>
struct collapse<typelist<T>>
{
    using type = T;
};
template<typename T0, typename T1, typename... Rest>
struct collapse<typelist<T0, T1, Rest...>>
{
    using type = std::tuple<T0, T1, Rest...>;
};
template<typename List>
using collapse_t = typename collapse<List>::type;

// Are all types in a typelist identical?
template<typename List>
struct all_same;
template<>
struct all_same<typelist<>>
{
    static constexpr bool value = true;
};
template<typename T>
struct all_same<typelist<T>>
{
    static constexpr bool value = true;
};
template<typename T0, typename T1, typename... Rest>
struct all_same<typelist<T0, T1, Rest...>>
{
    static constexpr bool value = std::is_same_v<T0, T1> && all_same<typelist<T1, Rest...>>::value;
};

// First type of a typelist (for picking the common type).
template<typename List>
struct head;
template<typename T, typename... Ts>
struct head<typelist<T, Ts...>>
{
    using type = T;
};
} // namespace detail

// ---------------------------------------------------------------------------
// result_of<E>: the result type of parsing expression E.
//
// Specialised per expression type (see the table in the design plan §2.1).
// The primary template is deliberately left undefined — using result_of on an
// unsupported expression is a compile error, which surfaces missing support
// immediately rather than degrading silently to void.
// ---------------------------------------------------------------------------
template<typename E>
struct result_of; // primary: undefined → used on unsupported types = hard error

// --- Terminal family (void — filtered) -----------------------------------
template<typename C, typename V>
struct result_of<TerminalExpr<C, V>>
{
    using type = void;
};
template<typename C, typename S>
struct result_of<TerminalSeqExpr<C, S>>
{
    using type = void;
};

// --- TokenExpr (value_type — kept) ---------------------------------------
// TokenExpr (Terminals.h) mirrors TerminalExpr's match behaviour but keeps the
// matched element (recovered by the fold from ctx.at(span.start)), so its
// result type is value_type, not void.
template<typename C, typename V>
struct result_of<TokenExpr<C, V>>
{
    using type = typename C::value_type;
};

// --- Leaves that never produce a tree ------------------------------------
template<typename C>
struct result_of<EmptyExpr<C>>
{
    using type = void;
};
template<typename C>
struct result_of<CutExpr<C>>
{
    using type = void;
};
template<typename C, typename Ch>
struct result_of<NotExpr<C, Ch>>
{
    using type = void;
};
template<typename C, typename Ch>
struct result_of<AndExpr<C, Ch>>
{
    using type = void;
};

// --- MatcherExpr (void — a recognizer, filtered from sequences) ----------
// It builds a node so its span is observable via on_match, but it surfaces no
// typed value to actions (what it matched is read off the node's offsets, not
// recomputed by the fold — see the fold_expr_impl no-op below).
template<typename C, typename Fn>
struct result_of<MatcherExpr<C, Fn>>
{
    using type = void;
};

// --- Rules / non-terminals (node_type; dispatched via node->producer) ----
template<typename C>
struct result_of<NonTerminal<C>>
{
    using type = typename C::node_type;
};
template<typename C>
struct result_of<Rule<C>>
{
    using type = typename C::node_type;
};

// --- Lexeme: forwards its child's result ---------------------------------
template<typename C, typename Ch>
struct result_of<LexemeExpr<C, Ch>>
{
    using type = typename result_of<Ch>::type;
};

// --- Sequence: filter void, then collapse (0→void, 1→T, ≥2→tuple) --------
template<typename C, typename... Children>
struct result_of<SequenceExpr<C, Children...>>
{
private:
    using raw = detail::typelist<typename result_of<Children>::type...>;
    using filtered = detail::filter_void_t<raw>;

public:
    using type = detail::collapse_t<filtered>;
};

// --- Alternation: all branches must share a result type ------------------
template<typename C, typename... Children>
struct result_of<AlternationExpr<C, Children...>>
{
private:
    using raw = detail::typelist<typename result_of<Children>::type...>;
    static_assert(detail::all_same<raw>::value,
                  "peglib: alternation branches must share a result type "
                  "(all alternatives must produce the same typed result).");

public:
    using type = typename detail::head<raw>::type;
};

// --- Repetition family ----------------------------------------------------
// Repetition / ZeroOrMore / OneOrMore / NTimes → std::vector<child result>.
// OptionalExpr → std::optional<child result>.
//
// All four repetition forms (the Repetition base and its three Self-hooked
// subclasses) share one result type and one fold body. `repetition_child<E>`
// exposes the child type for any of them and is absent otherwise, so a single
// result_of specialization and a single fold_expr_impl overload cover the lot.
//
// void-collapse: a repetition/optional of a void-result child is itself void
// (e.g. `*terminal` / `?cut()` produce no values). `vector<void>` and
// `optional<void>` are ill-formed, so we map void→void here via rep_of/opt_of.
namespace detail
{
template<typename T>
struct rep_of
{
    using type = std::vector<T>;
};
template<>
struct rep_of<void>
{
    using type = void;
};
template<typename T>
struct opt_of
{
    using type = std::optional<T>;
};
template<>
struct opt_of<void>
{
    using type = void;
};
template<typename T>
using rep_of_t = typename rep_of<T>::type;
template<typename T>
using opt_of_t = typename opt_of<T>::type;

// repetition_child<E>: the child expression type of any Repetition-family
// expression (Repetition base, ZeroOrMoreExpr, OneOrMoreExpr, NTimesExpr).
// Absent for everything else. Lets one result_of specialization + one
// fold_expr_impl overload cover all four forms.
template<typename E>
struct repetition_child; // undefined
template<typename C, typename Child, typename Self>
struct repetition_child<Repetition<C, Child, Self>>
{
    using type = Child;
};
template<typename C, typename Child>
struct repetition_child<ZeroOrMoreExpr<C, Child>>
{
    using type = Child;
};
template<typename C, typename Child>
struct repetition_child<OneOrMoreExpr<C, Child>>
{
    using type = Child;
};
template<typename C, typename Child>
struct repetition_child<NTimesExpr<C, Child>>
{
    using type = Child;
};
template<typename E>
    requires requires { typename repetition_child<E>::type; }
using repetition_child_t = typename repetition_child<E>::type;
} // namespace detail

// One specialization for every repetition form: the child's result wrapped in
// rep_of_t (vector<T>, or void if the child is void-result).
template<typename E>
    requires requires { typename detail::repetition_child_t<E>; }
struct result_of<E>
{
    using type = detail::rep_of_t<typename result_of<detail::repetition_child_t<E>>::type>;
};

template<typename C, typename Child>
struct result_of<OptionalExpr<C, Child>>
{
    using type = detail::opt_of_t<typename result_of<Child>::type>;
};

template<typename E>
using result_of_t = typename result_of<E>::type;

// ===========================================================================
// Fold: post-parse typed value builder.
//
// fold<E>(ctx, node) walks a ParseTreeNode and reconstructs E's typed result,
// owning each child value as a local and moving it up once. Because no value
// is stored on the shared tree, a move-only NodeType composes freely. The
// fold runs once, after parse, on the final (acyclic) tree.
//
// Each fold_expr_impl overload below mirrors a tree-construction rule in
// Combinators.h.
// ===========================================================================
namespace detail
{
// Index into node->children, threaded by reference so contributors that push
// no node (terminals/predicates/cut/empty) don't desynchronise the positional
// correspondence.
struct Cursor
{
    std::size_t index = 0;
};

// pushes_node<E>: true when E's parse() builds a node on a successful match
// (so the node IS present in the parent's children and the fold cursor must
// advance past it). False for pure recognisers (terminal/empty/cut/predicates)
// that contribute no node. Most void-result expressions push no node; the
// exception is MatcherExpr, which is void-result yet builds a node (so on_match
// can observe its span). The sequence fold consults this to decide whether a
// void-result child still consumes a children slot.
template<typename E>
struct pushes_node : std::false_type
{};
template<typename C, typename Fn>
struct pushes_node<MatcherExpr<C, Fn>> : std::true_type
{};
template<typename E>
inline constexpr bool pushes_node_v = pushes_node<E>::value;

template<typename E, typename Ctx, typename NodePtr>
auto fold_expr(Ctx& ctx, const NodePtr& node, Cursor& cur) -> result_of_t<E>;

// CONTRACT: fold_expr_impl<E>(ctx, node, cur) operates on `node` as E's OWN
// result node. Container cases walk node->children; each child fold receives
// a fresh child_cur. The null tag pointer is a compile-time type carrier.

// terminal/terminal-seq/empty/cut/Not/And: produce no node, contribute nothing.
template<typename C, typename V, typename Ctx, typename NodePtr>
void fold_expr_impl(const TerminalExpr<C, V>*, Ctx&, const NodePtr&, Cursor&)
{}
template<typename C, typename S, typename Ctx, typename NodePtr>
void fold_expr_impl(const TerminalSeqExpr<C, S>*, Ctx&, const NodePtr&, Cursor&)
{}
template<typename C, typename Ctx, typename NodePtr>
void fold_expr_impl(const EmptyExpr<C>*, Ctx&, const NodePtr&, Cursor&)
{}
template<typename C, typename Ctx, typename NodePtr>
void fold_expr_impl(const CutExpr<C>*, Ctx&, const NodePtr&, Cursor&)
{}
template<typename C, typename Ch, typename Ctx, typename NodePtr>
void fold_expr_impl(const NotExpr<C, Ch>*, Ctx&, const NodePtr&, Cursor&)
{}
template<typename C, typename Ch, typename Ctx, typename NodePtr>
void fold_expr_impl(const AndExpr<C, Ch>*, Ctx&, const NodePtr&, Cursor&)
{}
// MatcherExpr: void recognizer. Its node is in the tree (so on_match and
// parse_tree observe its span), but the typed fold contributes no value.
template<typename C, typename Fn, typename Ctx, typename NodePtr>
void fold_expr_impl(const MatcherExpr<C, Fn>*, Ctx&, const NodePtr&, Cursor&)
{}

// TokenExpr: one matched element, recovered from (ctx, span).
template<typename C, typename V, typename Ctx, typename NodePtr>
auto fold_expr_impl(const TokenExpr<C, V>*, Ctx& ctx, const NodePtr& node, Cursor&)
{
    return ctx.at(node->start_offset);
}

// LexemeExpr: forwards (no tree-shape change).
template<typename C, typename Ch, typename Ctx, typename NodePtr>
auto fold_expr_impl(const LexemeExpr<C, Ch>*, Ctx& ctx, const NodePtr& node, Cursor& cur)
{
    return fold_expr<Ch>(ctx, node, cur);
}

// AlternationExpr: the winner's node IS the rule's node (Combinators.h:109),
// passed through. The fold cannot statically know which branch won, so
// parseAlt stamps node->alt_winner with the winning index and we dispatch over
// the branch types via a runtime jump table. All branches share a result type
// (enforced by result_of<AlternationExpr>'s all_same static_assert), so each
// branch's fold yields the right type.
namespace altimpl
{
template<std::size_t I, typename Ctx, typename NodePtr, typename Cursor, typename... Children>
struct dispatch;
// Single remaining branch: it must be the winner (alt_winner == I). No base
// case for the empty pack — this avoids instantiating a default-constructed
// result for move-only / void result types.
template<std::size_t I, typename Ctx, typename NodePtr, typename Cursor, typename Head>
struct dispatch<I, Ctx, NodePtr, Cursor, Head>
{
    static auto run(Ctx& ctx, const NodePtr& node, Cursor& cur) -> result_of_t<Head>
    {
        return fold_expr<Head>(ctx, node, cur);
    }
};
template<std::size_t I,
         typename Ctx,
         typename NodePtr,
         typename Cursor,
         typename Head,
         typename... Tail>
struct dispatch<I, Ctx, NodePtr, Cursor, Head, Tail...>
{
    static auto run(Ctx& ctx, const NodePtr& node, Cursor& cur) -> result_of_t<Head>
    {
        if (node->alt_winner == I)
            return fold_expr<Head>(ctx, node, cur);
        return dispatch<I + 1, Ctx, NodePtr, Cursor, Tail...>::run(ctx, node, cur);
    }
};
} // namespace altimpl

template<typename C, typename... Children, typename Ctx, typename NodePtr>
auto fold_expr_impl(const AlternationExpr<C, Children...>*,
                    Ctx& ctx,
                    const NodePtr& node,
                    Cursor& cur)
{
    if constexpr (sizeof...(Children) == 0) {
        return; // void (empty alternation — never matches)
    } else {
        return altimpl::dispatch<0, Ctx, NodePtr, Cursor, Children...>::run(ctx, node, cur);
    }
}

// Repetition: one child per iteration → vector<child result>; void→void.
template<typename Child, typename Ctx, typename NodePtr>
auto fold_rep(Ctx& ctx, const NodePtr& node, Cursor& cur)
{
    using R = result_of_t<Child>;
    if constexpr (std::is_void_v<R>) {
        cur.index = node->children.size();
        return; // void
    } else {
        std::vector<R> out;
        while (cur.index < node->children.size()) {
            const auto& iter_node = node->children[cur.index];
            ++cur.index;
            Cursor iter_cur;
            out.push_back(fold_expr<Child>(ctx, iter_node, iter_cur));
        }
        return out;
    }
}
// One overload for every repetition form (Repetition base + the three
// Self-hooked subclasses). Dispatches via repetition_child_t<E>.
template<typename E, typename Ctx, typename NodePtr>
    requires requires { typename repetition_child_t<E>; }
auto fold_expr_impl(const E*, Ctx& ctx, const NodePtr& node, Cursor& cur)
{
    return fold_rep<repetition_child_t<E>>(ctx, node, cur);
}

// Optional: 0 children → nullopt; 1 → fold<Child>; void→void.
template<typename C, typename Ch, typename Ctx, typename NodePtr>
auto fold_expr_impl(const OptionalExpr<C, Ch>*, Ctx& ctx, const NodePtr& node, Cursor& cur)
{
    using R = result_of_t<Ch>;
    if constexpr (std::is_void_v<R>) {
        if (cur.index < node->children.size())
            ++cur.index;
        return; // void
    } else {
        if (cur.index >= node->children.size() || !node->children[cur.index])
            return std::optional<R>{std::nullopt};
        const auto& child = node->children[cur.index];
        ++cur.index;
        Cursor iter_cur;
        return std::optional<R>{fold_expr<Ch>(ctx, child, iter_cur)};
    }
}

// Sequence: walk children, skip void, collapse to void/T/tuple.
namespace seqimpl
{
template<typename NodePtr, typename Ctx, typename... Children, typename Acc, std::size_t I>
auto step(
    Ctx& ctx, const NodePtr& node, Cursor& cur, Acc acc, std::integral_constant<std::size_t, I>)
{
    using Child = std::tuple_element_t<I, std::tuple<Children...>>;
    using R = result_of_t<Child>;
    if constexpr (std::is_void_v<R>) {
        // Void-result child: contributes no typed value. But it may still
        // push a node into children (MatcherExpr does, so on_match can read
        // its span) — advance the cursor past it in that case, then recurse.
        if constexpr (pushes_node_v<Child>) {
            ++cur.index;
        }
        if constexpr (I + 1 < sizeof...(Children)) {
            return step<NodePtr, Ctx, Children...>(
                ctx, node, cur, std::move(acc), std::integral_constant<std::size_t, I + 1>{});
        } else {
            return acc;
        }
    } else {
        const auto& child_node = node->children.at(cur.index);
        ++cur.index;
        Cursor child_cur;
        auto v = fold_expr<Child>(ctx, child_node, child_cur);
        auto acc2 = std::tuple_cat(std::move(acc), std::make_tuple(std::move(v)));
        if constexpr (I + 1 < sizeof...(Children)) {
            return step<NodePtr, Ctx, Children...>(
                ctx, node, cur, std::move(acc2), std::integral_constant<std::size_t, I + 1>{});
        } else {
            return acc2;
        }
    }
}
} // namespace seqimpl

template<typename C, typename... Children, typename Ctx, typename NodePtr>
auto fold_expr_impl(const SequenceExpr<C, Children...>*, Ctx& ctx, const NodePtr& node, Cursor& cur)
{
    using R = result_of_t<SequenceExpr<C, Children...>>;
    if constexpr (sizeof...(Children) == 0 || std::is_void_v<R>) {
        seqimpl::step<NodePtr, Ctx, Children...>(
            ctx, node, cur, std::make_tuple(), std::integral_constant<std::size_t, 0>{});
        return; // void
    } else {
        auto acc = seqimpl::step<NodePtr, Ctx, Children...>(
            ctx, node, cur, std::make_tuple(), std::integral_constant<std::size_t, 0>{});
        if constexpr (std::tuple_size_v<decltype(acc)> == 1) {
            return std::move(std::get<0>(acc));
        } else {
            return acc;
        }
    }
}

// NonTerminal/Rule: pointer dispatch via fold_rule (defined out-of-line after
// NonTerminal.h exposes the typed-fold field).
template<typename C, typename Ctx, typename NodePtr>
auto fold_expr_impl(const NonTerminal<C>*, Ctx& ctx, const NodePtr& node, Cursor&) ->
    typename C::node_type;
template<typename C, typename Ctx, typename NodePtr>
auto fold_expr_impl(const Rule<C>*, Ctx& ctx, const NodePtr& node, Cursor&) ->
    typename C::node_type;

template<typename E, typename Ctx, typename NodePtr>
auto fold_expr(Ctx& ctx, const NodePtr& node, Cursor& cur) -> result_of_t<E>
{
    if constexpr (std::is_void_v<result_of_t<E>>) {
        fold_expr_impl(static_cast<const E*>(nullptr), ctx, node, cur);
        return; // void
    } else {
        return fold_expr_impl(static_cast<const E*>(nullptr), ctx, node, cur);
    }
}
} // namespace detail

// fold<E>(ctx, node): public entry. Used by the typed-action bridge (registered
// on the producing NonTerminal) and by Grammar::parse_ast.
template<typename E, typename Ctx, typename NodePtr>
auto fold(Ctx& ctx, const NodePtr& node) -> result_of_t<E>
{
    detail::Cursor cur;
    return detail::fold_expr<E>(ctx, node, cur);
}

// fold_rule: the Rule/NonTerminal case for Rule references INSIDE a body.
// Dispatches on node->producer's typed fold (the innermost rule that built
// the node — producer is preserved, not overwritten, so alias/alternation-
// passthrough keeps the right target). Pure value computation; side-effect
// hooks fire in a separate walk (fire_on_match below).
template<typename Ctx, typename NodePtr>
auto fold_rule(Ctx& ctx, const NodePtr& node) -> typename Ctx::node_type
{
    if (node->producer && node->producer->typed_fold()) {
        return node->producer->typed_fold()(ctx, node);
    }
    return typename Ctx::node_type{};
}

// fold_start: the ROOT entry (parse_ast). Dispatches on the START rule's
// NonTerminal (known explicitly), NOT on node->producer — so the start rule's
// fold runs even when it adopted a body node whose producer is inner.
template<typename Ctx, typename NodePtr, typename NonTerminalPtr>
auto fold_start(Ctx& ctx, const NodePtr& node, const NonTerminalPtr& start) ->
    typename Ctx::node_type
{
    if (start && start->typed_fold()) {
        return start->typed_fold()(ctx, node);
    }
    return fold_rule<Ctx, NodePtr>(ctx, node);
}

// fire_on_match: the side-effect walk. Visits every node in the (committed,
// acyclic) tree in pre-order and fires the producer's on_match hook where one
// is registered. Independent of the typed fold: a rule with only an on_match
// (no typed fold) still has its hook fire, because this walk is structural —
// it recurses through node->children regardless of whether any value
// computation is registered. Dispatch order mirrors the value fold: the
// START rule's hook is tried first, then node->producer.
template<typename Ctx, typename NodePtr, typename NonTerminalPtr>
void fire_on_match(Ctx& ctx, const NodePtr& node, const NonTerminalPtr& start)
{
    if (!node)
        return;
    if (start && start->on_match()) {
        start->on_match()(ctx, node);
    } else if (node->producer && node->producer->on_match()) {
        node->producer->on_match()(ctx, node);
    }
    for (const auto& child : node->children) {
        // Children carry their own producer (inner rules); pass nullptr as
        // the start so each child dispatches on its own producer.
        fire_on_match<Ctx, NodePtr, NonTerminalPtr>(ctx, child, nullptr);
    }
}

template<typename C, typename Ctx, typename NodePtr>
auto detail::fold_expr_impl(const NonTerminal<C>*, Ctx& ctx, const NodePtr& node, Cursor&) ->
    typename C::node_type
{
    return fold_rule<Ctx, NodePtr>(ctx, node);
}
template<typename C, typename Ctx, typename NodePtr>
auto detail::fold_expr_impl(const Rule<C>*, Ctx& ctx, const NodePtr& node, Cursor&) ->
    typename C::node_type
{
    return fold_rule<Ctx, NodePtr>(ctx, node);
}

// ---------------------------------------------------------------------------
// flat_args_t<R>: expand a collapsed result type R back into the flat
// per-argument typelist a typed action must accept (excluding Context&/Span):
//   - void          → typelist<>       → F(Context&, Span)
//   - scalar T      → typelist<T>      → F(Context&, Span, T)
//   - tuple<T...>   → typelist<T...>   → F(Context&, Span, T0, T1, ...)
// This is the inverse of seq_result's collapse: used by RuleHandle::set_action
// to derive the action's expected argument list from result_of_t<ExprType>.
// ---------------------------------------------------------------------------
namespace detail
{
template<typename R>
struct flat_args
{
    using type = typelist<R>; // scalar default
};
template<typename... Ts>
struct flat_args<std::tuple<Ts...>>
{
    using type = typelist<Ts...>;
};
template<>
struct flat_args<void>
{
    using type = typelist<>;
};
} // namespace detail
template<typename R>
using flat_args_t = typename detail::flat_args<R>::type;

// ---------------------------------------------------------------------------
// fold_and_invoke<F, E>(f, ctx, node): the typed-action bridge. Folds E's
// result from `node`, then calls f(ctx, span, <args>) unpacked per the
// seq_result collapse (void → f(ctx,sp); T → f(ctx,sp,T); tuple → apply).
// Registered by RuleHandle::set_action as the NonTerminal's typed fold.
// ---------------------------------------------------------------------------
namespace detail
{
template<typename T>
struct is_std_tuple : std::false_type
{};
template<typename... Ts>
struct is_std_tuple<std::tuple<Ts...>> : std::true_type
{};
template<typename T>
inline constexpr bool is_std_tuple_v = is_std_tuple<T>::value;
} // namespace detail

template<typename F, typename E, typename C, typename NodePtr>
auto fold_and_invoke(const F& f, C& ctx, const NodePtr& node) -> decltype(auto)
{
    Span sp{node->start_offset, node->end_offset};
    using R = result_of_t<E>;
    if constexpr (std::is_void_v<R>) {
        return f(ctx, sp);
    } else {
        // An action-bearing rule WRAPS its body node as node->children[0]
        // (NonTerminal::parse wrap-if-typed-fold branch), so that each rule in
        // a chain (root = middle = inner) is a distinct dispatchable node
        // instead of collapsing onto the innermost producer. Fold the body
        // from that child — folding from `node` itself would re-dispatch on
        // node->producer == this rule's own typed_fold and recurse forever.
        // children empty (a void-only body that built no node) → fold from
        // `node` as the historical fallback. Span is read from the wrapper
        // (the rule's full match span) in either case.
        const NodePtr& body_node = node->children.empty() ? node : node->children[0];
        auto value = fold<E>(ctx, body_node);
        if constexpr (detail::is_std_tuple_v<R>) {
            return std::apply(
                [&](auto&&... args) { return f(ctx, sp, std::forward<decltype(args)>(args)...); },
                std::move(value));
        } else {
            return f(ctx, sp, std::move(value));
        }
    }
}

// ---------------------------------------------------------------------------
// seq_result / seq_arglist: the filtered argument shape derived from a
// sequence of per-child result types. Used both to compute the user-visible
// type (void / T / tuple) and the flat argument list for arity checks.
// ---------------------------------------------------------------------------

// User-visible shape (what the action receives as a single aggregate, after
// tuple-unwrapping for the 1-element case).
template<typename... Ts>
using seq_result_t = detail::collapse_t<detail::filter_void_t<detail::typelist<Ts...>>>;

// Flat argument list (one entry per non-void child result) — for arity/type
// checks against the action's parameter list.
template<typename... Ts>
using seq_arglist_t = detail::filter_void_t<detail::typelist<Ts...>>;

// ---------------------------------------------------------------------------
// action_applyable<F, C, ArgList>: is F invocable as (C&, Span, ArgList...)?
//
// ArgList is a detail::typelist<...>. The specialisations below split on the
// argument count so a 0-arg (void) sequence matches `F(C&, Span)` and a
// multi-arg sequence matches `F(C&, Span, T0, T1, ...)`.
// ---------------------------------------------------------------------------
namespace detail
{
template<typename F, typename C, typename ArgList>
struct action_applyable; // primary: undefined

template<typename F, typename C>
struct action_applyable<F, C, typelist<>>
{
    static constexpr bool value = std::is_invocable_v<F, C&, Span>;
};

template<typename F, typename C, typename T>
struct action_applyable<F, C, typelist<T>>
{
    static constexpr bool value = std::is_invocable_v<F, C&, Span, T>;
};

template<typename F, typename C, typename T0, typename T1, typename... Rest>
struct action_applyable<F, C, typelist<T0, T1, Rest...>>
{
    static constexpr bool value = std::is_invocable_v<F, C&, Span, T0, T1, Rest...>;
};
} // namespace detail

// Convenience concept: does the action F match an expression whose filtered
// result argument list is ArgList?
template<typename F, typename C, typename ArgList>
concept action_matches = detail::action_applyable<std::remove_cvref_t<F>, C, ArgList>::value;

} // namespace parsers
} // namespace peg
