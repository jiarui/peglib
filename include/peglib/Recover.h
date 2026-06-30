// RecoverSpec: how a rule resyncs after its body fails. On body failure
// (with no cut committed), the rule scans forward one char at a time until
// is_sync_token(c) is true (or input ends), consumes that one sync char,
// records a diagnostic at the original failure position, and reports success
// with a transparent null tree.
#pragma once
#include "peglib/ParseError.h"

#include <functional>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace peg
{

template<typename CharT>
struct RecoverSpec
{
    std::function<bool(CharT)> is_sync_token;
    std::string label; // optional; falls back to the rule's name in diagnostics

    [[nodiscard]] bool configured() const noexcept { return static_cast<bool>(is_sync_token); }
};

// Builders. recover_set covers fixed token sets ({';', '}'}); recover_eol /
// recover_eof cover line/file boundaries; recover_predicate covers arbitrary
// conditions.
template<typename CharT>
RecoverSpec<CharT> recover_set(std::set<CharT> sync, std::string label = {})
{
    auto sync_copy = std::make_shared<std::set<CharT>>(std::move(sync));
    return RecoverSpec<CharT>{
        [sync_copy](CharT c) { return sync_copy->count(c) > 0; },
        std::move(label),
    };
}

template<typename CharT>
RecoverSpec<CharT> recover_set(std::initializer_list<CharT> sync, std::string label = {})
{
    return recover_set<CharT>(std::set<CharT>{sync}, std::move(label));
}

template<typename CharT>
RecoverSpec<CharT> recover_eol(std::string label = {})
{
    return RecoverSpec<CharT>{
        [](CharT c) { return c == static_cast<CharT>('\n'); },
        std::move(label),
    };
}

// Predicate that never matches: forces the scanner to EOF, then reports
// recovered success.
template<typename CharT>
RecoverSpec<CharT> recover_eof(std::string label = {})
{
    return RecoverSpec<CharT>{
        [](CharT) { return false; },
        std::move(label),
    };
}

template<typename CharT>
RecoverSpec<CharT> recover_predicate(std::function<bool(CharT)> pred, std::string label = {})
{
    return RecoverSpec<CharT>{std::move(pred), std::move(label)};
}

// Sugar for `rule.set_recovery(spec)`. Templated on the rule type so this
// header does not need to include NonTerminal.h.
//   peg::recover(g["stmt"], peg::recover_set<char>({';', '}'}));
template<typename RuleLike, typename CharT>
RuleLike&& recover(RuleLike&& rule, RecoverSpec<CharT> spec)
{
    rule.set_recovery(std::move(spec));
    return std::forward<RuleLike>(rule);
}

} // namespace peg
