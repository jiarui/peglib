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
        typename C::value_type;
        typename C::node_type;
        typename C::NonTerminalType;
        typename C::RuleState;
        typename C::State;
        typename C::ParseTreeNode;
        typename C::ParseTreeNodePtr;
        typename C::ParseResult;
        typename C::expected_set;
    } && requires(C c,
                  const C cc,
                  const typename C::State s,
                  const typename C::NonTerminalType* rule,
                  const typename C::RuleState rs,
                  ExpectedItem ei,
                  std::size_t pos) {
        // Position / state ----------------------------------------------------
        // Positions are byte/item offsets (std::size_t), not iterators.
        { c.mark() } -> std::same_as<std::size_t>;
        { c.next() } -> std::same_as<void>;
        { c.reset(pos) } -> std::same_as<void>;
        { c.ended() } -> std::convertible_to<bool>;
        { c.state() } -> std::same_as<typename C::State>;
        { c.state(s) } -> std::same_as<void>;
        // Character / slice access (replaces the old get_input() that exposed
        // the underlying source). Actions read matched text via substr().
        { c.current() } -> std::same_as<typename C::value_type>;
        { c.at(pos) } -> std::same_as<typename C::value_type>;
        { c.substr(pos, pos) } -> std::same_as<std::basic_string<typename C::value_type>>;

        // Memo ----------------------------------------------------------------
        // rule_state returns {inserted?, RuleState&}; we only constrain the
        // tuple<bool, RuleState> value shape.
        { c.rule_state(rule, pos) } -> std::convertible_to<std::tuple<bool, typename C::RuleState>>;
        { c.update_rule_state(rule, pos, rs) } -> std::same_as<bool>;

        // Cut -----------------------------------------------------------------
        { c.cut(true) } -> std::same_as<void>;
        { c.cut() } -> std::convertible_to<bool>;
        { c.init_cut() } -> std::same_as<void>;
        { c.remove_cut() } -> std::same_as<void>;

        // Error tracking ------------------------------------------------------
        // record_failure takes a size_t offset; NonTerminal / TerminalExpr
        // convert their mark() offsets before calling.
        { c.record_failure(pos, std::move(ei)) } -> std::same_as<void>;
        { cc.furthest_failure_pos() } -> std::same_as<std::size_t>;
        { cc.expected() } -> std::same_as<const typename C::expected_set&>;
        { cc.has_error() } -> std::convertible_to<bool>;
        { c.take_error() } -> std::same_as<std::optional<Diagnostic>>;
    };

// Static helper: true iff Context<CharT, NodeType> satisfies PegContext.
template<typename CharT, typename NodeType>
inline constexpr bool is_peg_context_v = PegContext<Context<CharT, NodeType>>;

// Self-checks: the Context specializations peglib ships with must all satisfy
// PegContext. If one of these fails, the concept has drifted from the real
// Context implementation and must be reconciled.
static_assert(PegContext<Context<char>>, "default char Context must satisfy PegContext");
static_assert(PegContext<Context<char, PegAstNodePtr>>,
              "meta-grammar Context (PegAstNodePtr) must satisfy PegContext");
static_assert(PegContext<Context<char32_t>>,
              "char32_t Context must satisfy PegContext (Tier 1 char)");

// Negative self-check: a type that does not provide the Context API must NOT
// satisfy PegContext. Guards against an over-permissive concept.
static_assert(!PegContext<int>, "int must not satisfy PegContext");
static_assert(!PegContext<std::string>, "std::string must not satisfy PegContext");

} // namespace peg
