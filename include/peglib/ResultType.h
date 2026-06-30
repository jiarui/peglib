// Compile-time result-type derivation + post-parse typed fold for typed
// semantic actions.
//
//   result_of<E>           : compile-time result type of expression E.
//   fold<E>(ctx, node)     : post-parse typed value builder.
//   action_matches<F,C,A>  : concept checking F is invocable as
//                            (C&, Span, A...) with A derived positionally
//                            from E's result type.
//
// Terminal result model:
//   - terminal/terminal-seq/empty/cut/And/Not → void (filtered).
//   - MatcherExpr → void (recognizer; observe via on_match).
//   - TokenExpr   → value_type (the matched element, kept).
//   - NonTerminal/Rule → node_type.
#pragma once

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
namespace parsers
{
namespace detail
{
template<typename... Ts>
struct typelist
{
    static constexpr std::size_t size = sizeof...(Ts);
};

template<typename L, typename R>
struct concat;
template<typename... As, typename... Bs>
struct concat<typelist<As...>, typelist<Bs...>>
{
    using type = typelist<As..., Bs...>;
};
template<typename L, typename R>
using concat_t = typename concat<L, R>::type;

// Filter void out of a typelist.
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
//   0 → void, 1 → T, ≥2 → std::tuple<T...>.
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

template<typename List>
struct head;
template<typename T, typename... Ts>
struct head<typelist<T, Ts...>>
{
    using type = T;
};
} // namespace detail

// Primary template undefined: used on an unsupported expression = hard error.
template<typename E>
struct result_of;

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

// TokenExpr keeps the matched element (recovered by the fold via
// ctx.at(span.start)).
template<typename C, typename V>
struct result_of<TokenExpr<C, V>>
{
    using type = typename C::value_type;
};

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

// MatcherExpr builds a node (on_match observes its span) but contributes no
// typed value to actions.
template<typename C, typename Fn>
struct result_of<MatcherExpr<C, Fn>>
{
    using type = void;
};

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

template<typename C, typename Ch>
struct result_of<LexemeExpr<C, Ch>>
{
    using type = typename result_of<Ch>::type;
};

template<typename C, typename... Children>
struct result_of<SequenceExpr<C, Children...>>
{
private:
    using raw = detail::typelist<typename result_of<Children>::type...>;
    using filtered = detail::filter_void_t<raw>;

public:
    using type = detail::collapse_t<filtered>;
};

// Alternation: all branches must share a result type.
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

// Repetition → std::vector<child result>; Optional → std::optional<child>;
// void child → void (vector<void>/optional<void> are ill-formed).
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

// Child type of any Repetition-family expression (Repetition base +
// ZeroOrMore / OneOrMore / NTimes). Absent for everything else: one
// result_of specialization + one fold_expr_impl overload cover all four forms.
template<typename E>
struct repetition_child;
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
// Fold: post-parse typed value builder. Walks a ParseTreeNode and
// reconstructs E's typed result, owning each child value as a local and
// moving it up once. Because no value is stored on the shared tree, a
// move-only NodeType composes freely. Runs once, after parse, on the final
// (acyclic) tree.
// ===========================================================================
namespace detail
{
struct Cursor
{
    std::size_t index = 0;
};

// True when E's parse() builds a node on a successful match (so the fold
// cursor must advance past it). Most void-result expressions push no node;
// MatcherExpr is the exception (void-result yet builds a node so on_match
// can observe its span).
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
template<typename C, typename Fn, typename Ctx, typename NodePtr>
void fold_expr_impl(const MatcherExpr<C, Fn>*, Ctx&, const NodePtr&, Cursor&)
{}

// TokenExpr: one matched element, recovered from (ctx, span).
template<typename C, typename V, typename Ctx, typename NodePtr>
auto fold_expr_impl(const TokenExpr<C, V>*, Ctx& ctx, const NodePtr& node, Cursor&)
{
    return ctx.at(node->start_offset);
}

template<typename C, typename Ch, typename Ctx, typename NodePtr>
auto fold_expr_impl(const LexemeExpr<C, Ch>*, Ctx& ctx, const NodePtr& node, Cursor& cur)
{
    return fold_expr<Ch>(ctx, node, cur);
}

// Alternation: parseAlt stamps node->alt_winner with the winning index; the
// fold dispatches over branch types via a runtime jump table. All branches
// share a result type (enforced by result_of<AlternationExpr>).
namespace altimpl
{
template<std::size_t I, typename Ctx, typename NodePtr, typename Cursor, typename... Children>
struct dispatch;
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
        return;
    } else {
        return altimpl::dispatch<0, Ctx, NodePtr, Cursor, Children...>::run(ctx, node, cur);
    }
}

template<typename Child, typename Ctx, typename NodePtr>
auto fold_rep(Ctx& ctx, const NodePtr& node, Cursor& cur)
{
    using R = result_of_t<Child>;
    if constexpr (std::is_void_v<R>) {
        cur.index = node->children.size();
        return;
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
        return;
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
        // Void-result child contributes no value, but may still push a node
        // (MatcherExpr does) — advance past it in that case.
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
        return;
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

// NonTerminal/Rule: dispatch via fold_rule (defined after NonTerminal.h).
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
        return;
    } else {
        return fold_expr_impl(static_cast<const E*>(nullptr), ctx, node, cur);
    }
}
} // namespace detail

// fold<E>(ctx, node): public entry. Used by the typed-action bridge and by
// Grammar::parse_ast.
template<typename E, typename Ctx, typename NodePtr>
auto fold(Ctx& ctx, const NodePtr& node) -> result_of_t<E>
{
    detail::Cursor cur;
    return detail::fold_expr<E>(ctx, node, cur);
}

// fold_rule: the Rule/NonTerminal case for Rule references INSIDE a body.
// Dispatches on node->producer's typed fold (the innermost rule that built
// the node — producer is preserved, not overwritten, so alias/alternation
// passthrough keeps the right target).
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

// fire_on_match: side-effect walk. Visits every node in pre-order and fires
// the producer's on_match hook. Independent of the typed fold: a rule with
// only on_match (no typed fold) still has its hook fire.
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

// flat_args_t<R>: inverse of seq_result's collapse. Used by
// RuleHandle::set_action to derive the action's expected argument list from
// result_of_t<ExprType>.
//   void        → typelist<>       → F(Context&, Span)
//   scalar T    → typelist<T>      → F(Context&, Span, T)
//   tuple<T...> → typelist<T...>   → F(Context&, Span, T0, T1, ...)
namespace detail
{
template<typename R>
struct flat_args
{
    using type = typelist<R>;
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

// fold_and_invoke<F, E>(f, ctx, node): the typed-action bridge. Folds E's
// result from `node`, then calls f(ctx, span, <args>) unpacked per the
// seq_result collapse. Registered by RuleHandle::set_action.
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
        // (NonTerminal::parse wrap-if-typed-fold branch). Fold the body from
        // that child — folding from `node` itself would re-dispatch on
        // node->producer == this rule's own typed_fold and recurse forever.
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

template<typename... Ts>
using seq_result_t = detail::collapse_t<detail::filter_void_t<detail::typelist<Ts...>>>;

template<typename... Ts>
using seq_arglist_t = detail::filter_void_t<detail::typelist<Ts...>>;

// action_applyable<F, C, ArgList>: is F invocable as (C&, Span, ArgList...)?
namespace detail
{
template<typename F, typename C, typename ArgList>
struct action_applyable;

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

template<typename F, typename C, typename ArgList>
concept action_matches = detail::action_applyable<std::remove_cvref_t<F>, C, ArgList>::value;

} // namespace parsers
} // namespace peg
