// PegContext concept: mirrors the full Context API that peglib's combinators
// depend on. Applied as a constraint on Grammar's template parameter so a
// malformed Context fails fast with a single concept diagnostic instead of a
// deep template error inside a combinator.
//
// Also defines the element-type contracts (PegValue / PegValueSet /
// PegValueRange / PegValueSeq) applied at the terminal factories.
#pragma once
#include "peglib/Context.h"
#include "peglib/ParseError.h"

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

template<typename C>
concept PegContext =
    requires {
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
        { c.mark() } -> std::same_as<std::size_t>;
        { c.next() } -> std::same_as<void>;
        { c.reset(pos) } -> std::same_as<void>;
        { c.ended() } -> std::convertible_to<bool>;
        { c.state() } -> std::same_as<typename C::State>;
        { c.state(s) } -> std::same_as<void>;
        { c.current() } -> std::same_as<typename C::value_type>;
        { c.at(pos) } -> std::same_as<typename C::value_type>;

        // Input slicing (extracting matched text by offset) is delegated to
        // InputSource via Context::input(); NOT part of this contract since
        // its return type is ill-formed for non-character value types.
        { c.rule_state(rule, pos) } -> std::convertible_to<std::tuple<bool, typename C::RuleState>>;
        { c.update_rule_state(rule, pos, rs) } -> std::same_as<bool>;

        { c.cut(true) } -> std::same_as<void>;
        { c.cut() } -> std::convertible_to<bool>;
        { c.init_cut() } -> std::same_as<void>;
        { c.remove_cut() } -> std::same_as<void>;

        { c.record_failure(pos, std::move(ei)) } -> std::same_as<void>;
        { cc.furthest_failure_pos() } -> std::same_as<std::size_t>;
        { cc.expected() } -> std::same_as<const typename C::expected_set&>;
        { cc.has_error() } -> std::convertible_to<bool>;
        { c.take_error() } -> std::same_as<std::optional<Diagnostic>>;
    };

template<typename CharT, typename NodeType>
inline constexpr bool is_peg_context_v = PegContext<Context<CharT, NodeType>>;

// Element-type contracts for terminal matching. A type satisfying PegValue
// may still render only as "<token>" in diagnostics if it provides no ADL
// to_display(const T&) hook — that is a diagnostic-quality issue, not a
// compile-correctness one.

// terminal(elem): needs operator== on const refs.
template<typename T>
concept PegValue = std::movable<T> && std::equality_comparable<T>;

// terminal(set): stored in std::set<elem>, needs operator<.
template<typename T>
concept PegValueSet = PegValue<T> && std::totally_ordered<T>;

// terminal(lo, hi) / terminal(array<elem, 2>): needs <= and >=.
template<typename T>
concept PegValueRange = PegValue<T> && std::totally_ordered<T>;

// terminalSeq(range): random-access range of PegValue elements.
template<typename Seq>
concept PegValueSeq = std::ranges::random_access_range<Seq> && PegValue<typename Seq::value_type>;

static_assert(PegValue<char>, "char must satisfy PegValue");
static_assert(PegValue<char32_t>, "char32_t must satisfy PegValue");
static_assert(PegValueSet<char>, "char must satisfy PegValueSet");
static_assert(PegValueSet<char32_t>, "char32_t must satisfy PegValueSet");
static_assert(PegValueRange<char>, "char must satisfy PegValueRange");
static_assert(PegValueRange<char32_t>, "char32_t must satisfy PegValueRange");
static_assert(PegValueSeq<std::string>, "std::string must satisfy PegValueSeq");
static_assert(PegValueSeq<std::u32string>, "std::u32string must satisfy PegValueSeq");

static_assert(PegContext<Context<char>>, "default char Context must satisfy PegContext");
static_assert(PegContext<Context<char32_t>>,
              "char32_t Context must satisfy PegContext (Tier 1 char)");

static_assert(!PegContext<int>, "int must not satisfy PegContext");
static_assert(!PegContext<std::string>, "std::string must not satisfy PegContext");

} // namespace peg
