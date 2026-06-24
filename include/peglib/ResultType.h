#pragma once

// ===========================================================================
// ResultType: compile-time result-type derivation for typed semantic actions.
//
// This header is the metaprogramming backbone of peglib's typed-action model
// (Model A). It is **purely additive**: no existing expression type is
// modified. It provides:
//
//   - `peg::Span`                         : the source/token span handed to
//                                            every typed action.
//   - `peg::parsers::result_of<E>`        : the compile-time result type of a
//                                            parsing expression E.
//   - `peg::parsers::seq_result<Ts...>`   : filter-void + 1-tuple-unpack rule
//                                            used to collapse sequence results.
//   - `peg::parsers::action_matches<F,C>` : concept checking that a callable F
//                                            is invocable as `(C&, Span, Args...)`
//                                            with Args derived positionally
//                                            from an expression's result type.
//
// Model A (terminal result model):
//   - TerminalExpr/TerminalSeqExpr/EmptyExpr/CutExpr/AndExpr/NotExpr → `void`
//     (filtered out of sequence results; structural tokens never appear as
//      action parameters, no "drop" combinator needed in the grammar).
//   - TokenExpr → `value_type` (the matched element is kept; operator identity
//     is visible to left-fold actions).
//   - NonTerminal/Rule → `node_type` (passthrough via node->value).
//
// The extractor (`peg::parsers::extract`) is the inverse of tree-building and
// lives in this header too; see its own block below.
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
// ---------------------------------------------------------------------------
// Span: the match span handed to every typed action.
//
// `start`/`end` mirror `ParseTreeNode::start_offset`/`end_offset`, whose
// semantics are determined by `Context::value_type`:
//   - char-level Context<char, ...>      → byte/character offsets
//   - token-level Context<Token, ...>    → token indices into the token stream
// A token-level leaf action reads the token via `ctx.at(sp.start)` and then
// recovers character offsets from `token.start`/`token.end`.
// ---------------------------------------------------------------------------
struct Span
{
    std::size_t start{};
    std::size_t end{};
};

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
    static constexpr bool value =
        std::is_same_v<T0, T1> && all_same<typelist<T1, Rest...>>::value;
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

// --- Terminal family (Model A: void — filtered) --------------------------
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

// --- TokenExpr (Model A: value_type — kept) -------------------------------
// NOTE: TokenExpr is added by this refactor in Terminals.h. It mirrors
// TerminalExpr's match behaviour but produces a tree node carrying the
// matched element in ParseTreeNode::token_value, so its result type is the
// element/value type, not void.
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

// --- Rules / non-terminals (passthrough via node->value) -----------------
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
// Each subclass carries its own CRTP identity via the Repetition Self hook
// (Combinators.h), so it is a distinct type and gets its own specialisation.
// `result_of<Repetition<C,Child>>` covers the bare default-Self case (rarely
// instantiated directly); the subclass specialisations below take precedence
// for `*e` / `+e` / `n*e` / `?e`.
template<typename C, typename Child, typename Self>
struct result_of<Repetition<C, Child, Self>>
{
    using type = std::vector<typename result_of<Child>::type>;
};
template<typename C, typename Child>
struct result_of<ZeroOrMoreExpr<C, Child>>
{
    using type = std::vector<typename result_of<Child>::type>;
};
template<typename C, typename Child>
struct result_of<OneOrMoreExpr<C, Child>>
{
    using type = std::vector<typename result_of<Child>::type>;
};
template<typename C, typename Child>
struct result_of<NTimesExpr<C, Child>>
{
    using type = std::vector<typename result_of<Child>::type>;
};
template<typename C, typename Child>
struct result_of<OptionalExpr<C, Child>>
{
    using type = std::optional<typename result_of<Child>::type>;
};

template<typename E>
using result_of_t = typename result_of<E>::type;

// ===========================================================================
// Extractor: the inverse of tree-building.
//
// `extract<ExprType>(node)` reads a ParseTreeNode built by ExprType's parse()
// and reconstructs the typed result. It is the runtime counterpart of
// result_of<ExprType>. The design mirrors the tree-construction rules
// verbatim (see Combinators.h):
//
//   - terminal/terminal-seq/empty/cut/Not/And  : produce no node, contribute
//     nothing (skip; do not advance the child cursor).
//   - TokenExpr                                 : occupies one child slot;
//     reads child->token_value (the matched element).
//   - NonTerminal/Rule                          : occupies one child slot
//     (even if its action returned a null value — the node is always kept,
//     NonTerminal.h:152); reads child->value.
//   - SequenceExpr                              : walks m_children in order,
//     skipping void-contributors, and collapses the survivors to
//     void / T / tuple<T...> per seq_result_t.
//   - AlternationExpr                           : passes the winning branch's
//     ParseResult through (no wrapper); the rule's node IS the branch's node,
//     so we read node->value directly.
//   - Repetition family (Zero/One/NTimes/Rep)   : each child node is one
//     iteration → std::vector<child result>.
//   - OptionalExpr                              : zero children → nullopt;
//     one child → extract<Child> wrapped in optional.
//   - LexemeExpr                                : forwards to extract<Child>.
//
// Passthrough: for an alias rule `g["exp"] = g["or_exp"]`, ExprType is Rule<C>,
// so the top-level extract reads node->value — and that node is or_exp's node
// (same shared_ptr), whose value was filled by or_exp's action. No action on
// `exp` is needed: the typed value flows through at zero cost.
//
// A cursor threads the child index through the recursion so that void
// contributors don't desynchronise the positional correspondence.
// ===========================================================================
namespace detail
{
// Mutable index into node->children. Threading it by reference lets the
// recursion advance it exactly once per non-void contributor, mirroring how
// SequenceExpr/repeat_parse_impl push only non-null child trees.
struct Cursor
{
    std::size_t index = 0;
};

// Forward declaration of the dispatch entry point.
template<typename E, typename NodePtr>
auto extract_expr(const NodePtr& node, Cursor& cur) -> result_of_t<E>;

// ---------------------------------------------------------------------------
// Tag-dispatched extractors. CONTRACT: extract_expr<E>(node, cur) operates on
// `node` as E's OWN result node — i.e. the node that E's parse() produced.
// Leaf/rule extractors read node->value / node->token_value directly. Container
// extractors (Sequence, Repetition, Optional) walk node->children, fetching each
// child node and recursively extracting on IT. The `cur` parameter is used only
// by container extractors to track their own child position.
// receives E's own node — uniform, no dual meaning.
// ---------------------------------------------------------------------------

// --- Terminal family (void: no node produced, contributes nothing) --------
template<typename C, typename V, typename NodePtr>
void extract_expr_impl(const TerminalExpr<C, V>*, const NodePtr&, Cursor&)
{}
template<typename C, typename S, typename NodePtr>
void extract_expr_impl(const TerminalSeqExpr<C, S>*, const NodePtr&, Cursor&)
{}

// --- TokenExpr: read token_value from its own node -----------------------
template<typename C, typename V, typename NodePtr>
auto extract_expr_impl(const TokenExpr<C, V>*, const NodePtr& node, Cursor&)
{
    return *node->token_value;
}

// --- Leaves that never produce a tree (void) ------------------------------
template<typename C, typename NodePtr>
void extract_expr_impl(const EmptyExpr<C>*, const NodePtr&, Cursor&)
{}
template<typename C, typename NodePtr>
void extract_expr_impl(const CutExpr<C>*, const NodePtr&, Cursor&)
{}
template<typename C, typename Ch, typename NodePtr>
void extract_expr_impl(const NotExpr<C, Ch>*, const NodePtr&, Cursor&)
{}
template<typename C, typename Ch, typename NodePtr>
void extract_expr_impl(const AndExpr<C, Ch>*, const NodePtr&, Cursor&)
{}

// --- NonTerminal / Rule: read value from its own node --------------------
// The node is always kept (NonTerminal.h:152), even if the action returned a
// null value (transparent rule). So a transparent rule surfaces here as a null
// NodeType argument — an honest reflection of the tree, not a silent skip.
template<typename C, typename NodePtr>
auto extract_expr_impl(const NonTerminal<C>*, const NodePtr& node, Cursor&)
{
    return node->value;
}
template<typename C, typename NodePtr>
auto extract_expr_impl(const Rule<C>*, const NodePtr& node, Cursor&)
{
    return node->value;
}

// --- LexemeExpr: forward (does not change tree shape) ---------------------
template<typename C, typename Ch, typename NodePtr>
auto extract_expr_impl(const LexemeExpr<C, Ch>*, const NodePtr& node, Cursor& cur)
{
    return extract_expr<Ch>(node, cur);
}

// --- AlternationExpr: the winning branch's ParseResult is passed through --
// (Combinators.h:109), so the rule's node IS the branch's node. Its value was
// filled by the branch — read node->value directly. The Children pack is
// variadic (AlternationExpr<C, Children...>), so the tag must match it.
template<typename C, typename... Children, typename NodePtr>
auto extract_expr_impl(const AlternationExpr<C, Children...>*, const NodePtr& node, Cursor&)
{
    return node->value;
}

// --- Repetition family: node->children holds one entry per iteration -----
// For each child node, extract<Child> on THAT child node (it is one iteration's
// result node — e.g. an anonymous SequenceExpr node for `*(token >> mul)`, or
// a rule's NonTerminal node for `*rule`).
template<typename Child, typename NodePtr>
auto extract_rep(const NodePtr& node, Cursor& cur)
{
    using R = result_of_t<Child>;
    std::vector<R> out;
    while (cur.index < node->children.size()) {
        const auto& iter_node = node->children[cur.index];
        ++cur.index;
        Cursor iter_cur;
        out.push_back(extract_expr<Child>(iter_node, iter_cur));
    }
    return out;
}
template<typename C, typename Ch, typename NodePtr>
auto extract_expr_impl(const ZeroOrMoreExpr<C, Ch>*, const NodePtr& node, Cursor& cur)
{
    return extract_rep<Ch>(node, cur);
}
template<typename C, typename Ch, typename NodePtr>
auto extract_expr_impl(const OneOrMoreExpr<C, Ch>*, const NodePtr& node, Cursor& cur)
{
    return extract_rep<Ch>(node, cur);
}
template<typename C, typename Ch, typename NodePtr>
auto extract_expr_impl(const NTimesExpr<C, Ch>*, const NodePtr& node, Cursor& cur)
{
    return extract_rep<Ch>(node, cur);
}
template<typename C, typename Ch, typename Self, typename NodePtr>
auto extract_expr_impl(const Repetition<C, Ch, Self>*, const NodePtr& node, Cursor& cur)
{
    return extract_rep<Ch>(node, cur);
}

// --- OptionalExpr: node->children has 0 (absent) or 1 (present) entry ----
template<typename C, typename Ch, typename NodePtr>
auto extract_expr_impl(const OptionalExpr<C, Ch>*, const NodePtr& node, Cursor& cur)
{
    using R = result_of_t<Ch>;
    if (cur.index >= node->children.size() || !node->children[cur.index])
        return std::optional<R>{std::nullopt};
    const auto& child = node->children[cur.index];
    ++cur.index;
    Cursor iter_cur;
    return std::optional<R>{extract_expr<Ch>(child, iter_cur)};
}

// --- SequenceExpr: walk children, skip void, collapse to void/T/tuple -----
//
// parseSeq (Combinators.h:54) pushes a child's tree into node->children ONLY if
// that child produced a non-null tree. Void-contributors (terminal/predicate/
// cut/empty) produce {true, nullptr} and are NOT pushed. So the i-th
// SequenceExpr child maps to node->children only if its result is non-void.
//
// The walker recurses over the Children pack by index:
//   - void child     : skip (no node in children, no cursor advance), recurse.
//   - non-void child : fetch node->children[cur.index++], extract<Child> on
//                      that child node, prepend to the survivor tuple, recurse.
// Finally collapse: 0 survivors → void, 1 → the value, ≥2 → the tuple.
namespace seqimpl
{
template<typename NodePtr, typename... Children, typename Acc, std::size_t I>
auto step(const NodePtr& node, detail::Cursor& cur, Acc acc,
          std::integral_constant<std::size_t, I>)
{
    using Child = std::tuple_element_t<I, std::tuple<Children...>>;
    using R = result_of_t<Child>;
    if constexpr (std::is_void_v<R>) {
        // Void contributor: not in node->children. Skip and recurse.
        if constexpr (I + 1 < sizeof...(Children)) {
            return step<NodePtr, Children...>(
                node, cur, std::move(acc), std::integral_constant<std::size_t, I + 1>{});
        } else {
            return acc;
        }
    } else {
        // Fetch this child's node and extract on IT (per the extractor contract).
        const auto& child_node = node->children.at(cur.index);
        ++cur.index;
        detail::Cursor child_cur;
        auto v = detail::extract_expr<Child>(child_node, child_cur);
        auto acc2 = std::tuple_cat(std::move(acc), std::make_tuple(std::move(v)));
        if constexpr (I + 1 < sizeof...(Children)) {
            return step<NodePtr, Children...>(
                node, cur, std::move(acc2), std::integral_constant<std::size_t, I + 1>{});
        } else {
            return acc2;
        }
    }
}
} // namespace seqimpl

// Specialisation: SequenceExpr extractor drives seqimpl over its Children.
template<typename C, typename... Children, typename NodePtr>
auto extract_expr_impl(const SequenceExpr<C, Children...>*, const NodePtr& node, Cursor& cur)
{
    using R = result_of_t<SequenceExpr<C, Children...>>;
    if constexpr (sizeof...(Children) == 0 || std::is_void_v<R>) {
        // No survivors: walk children for consistency (advances cursor only for
        // any non-void contributors — but R=void means there are none), return void.
        seqimpl::step<NodePtr, Children...>(
            node, cur, std::make_tuple(), std::integral_constant<std::size_t, 0>{});
        return; // void
    } else {
        auto acc = seqimpl::step<NodePtr, Children...>(
            node, cur, std::make_tuple(), std::integral_constant<std::size_t, 0>{});
        if constexpr (std::tuple_size_v<decltype(acc)> == 1) {
            return std::move(std::get<0>(acc));
        } else {
            return acc; // already a tuple<R...>
        }
    }
}

// Dispatch entry point: tag-dispatch on the expression type. The null pointer
// is a compile-time type carrier only — never dereferenced.
template<typename E, typename NodePtr>
auto extract_expr(const NodePtr& node, Cursor& cur) -> result_of_t<E>
{
    if constexpr (std::is_void_v<result_of_t<E>>) {
        extract_expr_impl(static_cast<const E*>(nullptr), node, cur);
        return; // void
    } else {
        return extract_expr_impl(static_cast<const E*>(nullptr), node, cur);
    }
}
} // namespace detail

// ---------------------------------------------------------------------------
// extract<E>(node): public entry point. Used by the typed set_action bridge to
// reconstruct the typed result from the body's parse tree.
// ---------------------------------------------------------------------------
template<typename E, typename NodePtr>
auto extract(const NodePtr& node) -> result_of_t<E>
{
    detail::Cursor cur;
    return detail::extract_expr<E>(node, cur);
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
// invoke_action<F, E>(f, ctx, node): the erase-bridge. Extracts E's typed
// result from `node`, then calls f(ctx, span, <args>) where <args> is the
// extracted value unpacked per the seq_result collapse:
//   - void  : f(ctx, span)
//   - T     : f(ctx, span, value)
//   - tuple : f(ctx, span, t0, t1, ...)  via std::apply
// Used by RuleHandle::set_action to turn a typed action F into the type-erased
// std::function<NodeType(Context&, ParseTreeNodePtr)> that NonTerminal stores.
// ---------------------------------------------------------------------------
namespace detail
{
// Detect std::tuple specialisations (to decide unpack-vs-scalar at compile time).
template<typename T>
struct is_std_tuple : std::false_type
{};
template<typename... Ts>
struct is_std_tuple<std::tuple<Ts...>> : std::true_type
{};
template<typename T>
inline constexpr bool is_std_tuple_v = is_std_tuple<T>::value;

template<typename F, typename C>
auto call_action_void(const F& f, C& ctx, Span sp) -> decltype(auto)
{
    return f(ctx, sp);
}
template<typename F, typename C, typename T>
auto call_action_scalar(const F& f, C& ctx, Span sp, T&& v) -> decltype(auto)
{
    return f(ctx, sp, std::forward<T>(v));
}
template<typename F, typename C, typename... Ts>
auto call_action_tuple(const F& f, C& ctx, Span sp, std::tuple<Ts...>&& t) -> decltype(auto)
{
    return std::apply([&](Ts&&... args) { return f(ctx, sp, std::forward<Ts>(args)...); },
                      std::move(t));
}
} // namespace detail

template<typename F, typename E, typename C, typename NodePtr>
auto invoke_action(const F& f, C& ctx, const NodePtr& node) -> decltype(auto)
{
    Span sp{node->start_offset, node->end_offset};
    using R = result_of_t<E>;
    if constexpr (std::is_void_v<R>) {
        return detail::call_action_void(f, ctx, sp);
    } else {
        auto value = extract<E>(node);
        if constexpr (detail::is_std_tuple_v<R>) {
            return detail::call_action_tuple(f, ctx, sp, std::move(value));
        } else {
            return detail::call_action_scalar(f, ctx, sp, std::move(value));
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
