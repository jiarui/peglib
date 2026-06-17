#pragma once
#include "peglib/Context.h"
#include "peglib/ParseError.h"

#include <concepts>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace peg
{

// ---------------------------------------------------------------------------
// PegContext: concept constraining a well-formed Context specialization.
//
// A type satisfying PegContext must provide:
//   - Required typedefs: iterator, value_type, node_type, match_range,
//     ParseTreeNode, ParseTreeNodePtr, ParseResult
//   - Position / state API: mark(), next(), reset(iterator), ended()
//   - Memo API: rule_state(), update_rule_state()
//   - Error tracking API: record_failure(size_t, ExpectedItem),
//                          furthest_failure_pos(), take_error()
//
// This concept enforces signature shapes, not full semantic correctness.
// ---------------------------------------------------------------------------

template<typename C>
concept PegContext =
    requires {
        typename C::iterator;
        typename C::value_type;
        typename C::node_type;
        typename C::match_range;
        typename C::ParseTreeNode;
        typename C::ParseTreeNodePtr;
        typename C::ParseResult;
    } &&
    requires(
        C c, typename C::iterator it, ExpectedItem ei, std::size_t pos) {
        // Position / state
        { c.mark() } -> std::same_as<typename C::iterator>;
        { c.next() } -> std::same_as<void>;
        { c.reset(it) } -> std::same_as<void>;
        { c.ended() } -> std::convertible_to<bool>;

        // Error tracking
        { c.record_failure(pos, std::move(ei)) } -> std::same_as<void>;
        { c.furthest_failure_pos() } -> std::same_as<std::size_t>;
        { c.take_error() } -> std::same_as<std::optional<Diagnostic>>;
    };

// Static assertion: the default Context specialization satisfies PegContext.
template<typename S, typename N>
inline constexpr bool is_peg_context_v = PegContext<Context<S, N>>;

} // namespace peg
