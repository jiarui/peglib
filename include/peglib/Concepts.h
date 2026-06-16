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
//   - Required typedefs: input_source_type, iterator, value_type, node_type, match_range
//   - Position / state API: mark(), next(), reset(iterator), ended()
//   - Value stack API: push_node(node_type), pop_node(), peek_node(), node_count(),
//                      clear_stack()
//   - Error tracking API: record_failure(size_t, ExpectedItem),
//                          furthest_failure_pos(), take_error()
//
// This concept enforces signature shapes, not full semantic correctness.
// ---------------------------------------------------------------------------

template<typename C>
concept PegContext =
    requires {
        // Required typedefs
        typename C::iterator;
        typename C::value_type;
        typename C::node_type;
        typename C::match_range;
        typename C::Rule;
    } &&
    requires(
        C c, typename C::iterator it, typename C::node_type n, ExpectedItem ei, std::size_t pos) {
        // Position / state
        { c.mark() } -> std::same_as<typename C::iterator>;
        { c.next() } -> std::same_as<void>;
        { c.reset(it) } -> std::same_as<void>;
        { c.ended() } -> std::convertible_to<bool>;

        // Value stack
        { c.push_node(std::move(n)) } -> std::same_as<void>;
        { c.pop_node() } -> std::same_as<typename C::node_type>;
        { c.peek_node() } -> std::same_as<const typename C::node_type&>;
        { c.node_count() } -> std::convertible_to<std::size_t>;
        { c.clear_stack() } -> std::same_as<void>;

        // Error tracking
        { c.record_failure(pos, std::move(ei)) } -> std::same_as<void>;
        { c.furthest_failure_pos() } -> std::same_as<std::size_t>;
        { c.take_error() } -> std::same_as<std::optional<Diagnostic>>;
    };

// Static assertion: the default Context specialization satisfies PegContext.
template<typename S, typename N>
inline constexpr bool is_peg_context_v = PegContext<Context<S, N>>;

} // namespace peg
