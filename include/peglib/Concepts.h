#pragma once
#include "peglib/Context.h"
#include "peglib/ParseError.h"
#include "peglib/PegAst.h"

#include <concepts>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>

namespace peg
{

// ---------------------------------------------------------------------------
// PegContext: concept describing the FULL Context API that peglib's
// combinators depend on.
//
// A type satisfying PegContext must provide every member type and method
// that SequenceExpr / AlternationExpr / Repetition / NonTerminal / the
// terminal leaves actually call. A custom Context type that satisfies this
// concept is guaranteed to instantiate cleanly when used as a Grammar's
// template argument — the constraint is applied at the Grammar entry point
// (see Grammar.h), so a malformed Context fails fast with a single concept
// diagnostic instead of a deep template error inside a combinator.
//
// This concept checks signature shapes, not full semantic correctness.
// ---------------------------------------------------------------------------

template<typename C>
concept PegContext =
    requires {
        // Nested types ---------------------------------------------------------
        typename C::iterator;
        typename C::value_type;
        typename C::node_type;
        typename C::match_range;
        typename C::NonTerminalType;
        typename C::RuleState;
        typename C::State;
        typename C::ParseTreeNode;
        typename C::ParseTreeNodePtr;
        typename C::ParseResult;
        typename C::expected_set;
    } && requires(C c,
                  const C cc,
                  typename C::iterator it,
                  const typename C::iterator cit,
                  const typename C::State s,
                  const typename C::NonTerminalType* rule,
                  const typename C::RuleState rs,
                  ExpectedItem ei,
                  std::size_t pos) {
        // Position / state ----------------------------------------------------
        { c.mark() } -> std::same_as<typename C::iterator>;
        { c.next() } -> std::same_as<void>;
        { c.reset(it) } -> std::same_as<void>;
        { c.ended() } -> std::convertible_to<bool>;
        { c.state() } -> std::same_as<typename C::State>;
        { c.state(s) } -> std::same_as<void>;
        { cc.offset_of(cit) } -> std::same_as<std::size_t>;
        // get_input() returns a const reference to the underlying source. Actions
        // index into it (get_input()[offset]) and iterate it; we only require the
        // expression to be well-formed, not a specific type — the concrete return
        // type is the InputSource template argument, which is not a public
        // typedef.
        cc.get_input();

        // Memo ----------------------------------------------------------------
        // rule_state returns {inserted?, RuleState&}; we only constrain the
        // tuple<bool, RuleState> value shape.
        { c.rule_state(rule, it) } -> std::convertible_to<std::tuple<bool, typename C::RuleState>>;
        { c.update_rule_state(rule, it, rs) } -> std::same_as<bool>;

        // Cut -----------------------------------------------------------------
        { c.cut(true) } -> std::same_as<void>;
        { c.cut() } -> std::convertible_to<bool>;
        { c.init_cut() } -> std::same_as<void>;
        { c.remove_cut() } -> std::same_as<void>;

        // Error tracking ------------------------------------------------------
        // record_failure has two overloads (by size_t offset and by iterator);
        // both are exercised by NonTerminal / TerminalExpr.
        { c.record_failure(pos, std::move(ei)) } -> std::same_as<void>;
        { c.record_failure(it, std::move(ei)) } -> std::same_as<void>;
        { cc.furthest_failure_pos() } -> std::same_as<std::size_t>;
        { cc.expected() } -> std::same_as<const typename C::expected_set&>;
        { cc.has_error() } -> std::convertible_to<bool>;
        { c.take_error() } -> std::same_as<std::optional<Diagnostic>>;
    };

// Static helpers: true iff the default Context specialization with the given
// InputSource / NodeType satisfies PegContext. Provided for user-facing
// static_asserts and self-checks.
template<typename S, typename N>
inline constexpr bool is_peg_context_v = PegContext<Context<S, N>>;

// Self-checks: the three Context specializations peglib ships with must all
// satisfy PegContext. If one of these fails, the concept has drifted from the
// real Context implementation and must be reconciled.
static_assert(PegContext<Context<std::span<const char>>>,
              "default span-backed Context must satisfy PegContext");
static_assert(PegContext<Context<std::span<const char>, PegAstNodePtr>>,
              "meta-grammar Context (PegAstNodePtr) must satisfy PegContext");
static_assert(PegContext<Context<FileSource<char>>>,
              "FileSource-backed Context must satisfy PegContext");

// Negative self-check: a type that does not provide the Context API must NOT
// satisfy PegContext. Guards against an over-permissive concept.
static_assert(!PegContext<int>, "int must not satisfy PegContext");
static_assert(!PegContext<std::string>, "std::string must not satisfy PegContext");

} // namespace peg
