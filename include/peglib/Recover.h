#pragma once
#include "peglib/ParseError.h"

#include <functional>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>

namespace peg
{

// ---------------------------------------------------------------------------
// RecoverSpec: describes how a rule resyncs after a failure.
//
// A rule with a RecoverSpec set (via Rule::set_recovery or peg::recover) does
// not simply fail when its body fails to match. Instead, after recording a
// diagnostic at the failure position, it scans input forward one character
// at a time until `is_sync_token(c)` returns true (or input ends), consumes
// that one sync character, and reports the rule as having succeeded with a
// transparent null tree. Parsing then continues from the sync point, so a
// single malformed construct does not abort the whole file — the parser can
// report many errors per run.
//
// The predicate form is deliberately permissive: it covers fixed token sets
// ({';', '}'}), end-of-line, end-of-input, or any user-supplied condition.
// Sync tokens are matched as single characters; multi-token sync sequences
// are a future enhancement (would need a try-parse probe, not a char test).
// ---------------------------------------------------------------------------
template<typename CharT>
struct RecoverSpec
{
    // Returns true if `c` is a character the parser can resume from.
    // Empty (default-constructed) means "no recovery configured" — the rule
    // fails normally.
    std::function<bool(CharT)> is_sync_token;

    // Optional human-readable description used in the recorded diagnostic
    // (falls back to the rule's name when empty).
    std::string label;

    [[nodiscard]] bool configured() const noexcept { return static_cast<bool>(is_sync_token); }
};

// ---------------------------------------------------------------------------
// Convenience builders. Each returns a RecoverSpec with the predicate set
// and an optional label.
// ---------------------------------------------------------------------------

// Recover at any of the given single-character sync tokens.
//   peg::recover_set<char>({';', '}', ','})
template<typename CharT>
RecoverSpec<CharT> recover_set(std::set<CharT> sync, std::string label = {})
{
    auto sync_copy = std::make_shared<std::set<CharT>>(std::move(sync));
    return RecoverSpec<CharT>{
        [sync_copy](CharT c) { return sync_copy->count(c) > 0; },
        std::move(label),
    };
}

// Initializer-list overload for the common `{';', '}'}` spelling.
template<typename CharT>
RecoverSpec<CharT> recover_set(std::initializer_list<CharT> sync, std::string label = {})
{
    return recover_set<CharT>(std::set<CharT>{sync}, std::move(label));
}

// Recover at end of line (newline). Useful for line-oriented grammars.
template<typename CharT>
RecoverSpec<CharT> recover_eol(std::string label = {})
{
    return RecoverSpec<CharT>{
        [](CharT c) { return c == static_cast<CharT>('\n'); },
        std::move(label),
    };
}

// Recover at end of input. Last-ditch: consume the rest of the file.
template<typename CharT>
RecoverSpec<CharT> recover_eof(std::string label = {})
{
    // The scanner stops at EOF regardless; a predicate that never matches
    // forces it to run to the end and then report recovered success.
    return RecoverSpec<CharT>{
        [](CharT) { return false; },
        std::move(label),
    };
}

// Recover using an arbitrary predicate.
//   peg::recover_predicate<char>([](char c){ return c==';' || c=='\n'; })
template<typename CharT>
RecoverSpec<CharT> recover_predicate(std::function<bool(CharT)> pred, std::string label = {})
{
    return RecoverSpec<CharT>{std::move(pred), std::move(label)};
}

// ---------------------------------------------------------------------------
// peg::recover — sugar for `rule.set_recovery(spec)`.
//
//   g["stmt"] = peg::recover(g["stmt"], peg::recover_set<char>({';', '}'}));
//
// Equivalent to:
//   g["stmt"].set_recovery(peg::recover_set<char>({';', '}'}));
//
// Templated on the rule type so this header does not need to include
// NonTerminal.h (which includes this header). Any type with a compatible
// set_recovery(RecoverSpec<CharT>) method works.
// ---------------------------------------------------------------------------
template<typename RuleLike, typename CharT>
RuleLike&& recover(RuleLike&& rule, RecoverSpec<CharT> spec)
{
    rule.set_recovery(std::move(spec));
    return std::forward<RuleLike>(rule);
}

} // namespace peg
