#pragma once
#include "peglib/Context.h"
#include "peglib/ParseError.h"
#include "peglib/PegAst.h"

#include <concepts>
#include <cstddef>
#include <optional>
#include <ranges>
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

// ---------------------------------------------------------------------------
// Element-type contracts for terminal matching.
//
// Each terminal factory in Rule.h builds a TerminalExpr / TerminalSeqExpr
// whose `parse()` matches via the symbolConsumable overloads in ParserFwd.h.
// Those overloads use specific operators on the element type; the matching
// concepts below name exactly the operators each shape needs, and are applied
// as `requires` clauses at the factories so a wrong element type fails at the
// factory boundary with one clear diagnostic instead of deep inside a
// combinator's instantiation.
//
// Note on rendering: a separate concern, handled by the to_display CPO in
// ParseError.h. A type can satisfy PegValue yet still render only as
// "<token>" if it provides no ADL `to_display(const T&)` hook — that is a
// diagnostic-quality issue, not a compile-correctness one.
// ---------------------------------------------------------------------------

// terminal(elem): symbolConsumable(v, value) does `v == value`. Requires
// equality on const refs (operator== as a const member or free function).
template<typename T>
concept PegValue = std::movable<T> && std::equality_comparable<T>;

// terminal(set): the value is stored in std::set<elem>, which needs operator<
// (std::less<elem>) in addition to equality.
template<typename T>
concept PegValueSet = PegValue<T> && std::totally_ordered<T>;

// terminal(lo, hi) / terminal(std::array<elem, 2>): symbolConsumable asserts
// `values[0] <= values[1]` and evaluates `v >= values[0] && v <= values[1]`,
// so the element needs <= and >= (subsumed by totally_ordered).
template<typename T>
concept PegValueRange = PegValue<T> && std::totally_ordered<T>;

// terminalSeq(range): a random-access range whose element type satisfies
// PegValue (sequence matching is element-wise equality).
template<typename Seq>
concept PegValueSeq = std::ranges::random_access_range<Seq> && PegValue<typename Seq::value_type>;

// Self-checks: the Tier-1 element types must satisfy every relevant concept.
static_assert(PegValue<char>, "char must satisfy PegValue");
static_assert(PegValue<char32_t>, "char32_t must satisfy PegValue");
static_assert(PegValueSet<char>, "char must satisfy PegValueSet");
static_assert(PegValueSet<char32_t>, "char32_t must satisfy PegValueSet");
static_assert(PegValueRange<char>, "char must satisfy PegValueRange");
static_assert(PegValueRange<char32_t>, "char32_t must satisfy PegValueRange");
static_assert(PegValueSeq<std::string>, "std::string must satisfy PegValueSeq");
static_assert(PegValueSeq<std::u32string>, "std::u32string must satisfy PegValueSeq");

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
