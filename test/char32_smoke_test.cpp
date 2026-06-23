// Tier 1 char generality smoke test: verifies that Context<char32_t> drives a
// real parse, and that the diagnostic layer renders char32_t codepoints
// correctly (UTF-8 / \UNNNNNNNN) instead of truncating them to char.
//
// This is the user-visible payoff of the char-debt cleanup: a non-char
// value_type is now first-class for matching AND error reporting. The
// PEG-text compilation layer (GrammarCompiler) still produces char-only
// grammars — Tier 2/3 (UTF-8 decoder, codepoint-aware '.') is future work.

#include "peglib.h"

#include "doctest.h"

#include <string>
#include <string_view>

using namespace peg;

TEST_CASE("char32-context-matches-u32string-literal")
{
    // Build a char32_t terminal sequence and parse it against a u32string.
    using C32 = Context<char32_t>;
    Grammar<char32_t> g;
    g["hello"] = terminalSeq(std::u32string{U"hello"});
    g.set_start("hello");

    std::u32string input = U"hello";
    C32 ctx{input};
    CHECK(g.parse(ctx));
    CHECK(ctx.ended());
}

TEST_CASE("char32-context-matches-single-codepoint-terminal")
{
    using C32 = Context<char32_t>;
    Grammar<char32_t> g;
    // U+4E2D ('middle' / '中') as a single-codepoint terminal.
    g["cjk"] = terminal(U'\u4E2D');
    g.set_start("cjk");

    std::u32string input = U"\u4E2D";
    C32 ctx{input};
    CHECK(g.parse(ctx));
    CHECK(ctx.ended());
}

TEST_CASE("char32-failure-diagnostic-renders-codepoint-not-garbled")
{
    // When a char32_t terminal fails, record_expected must render the expected
    // codepoint in a readable form. U+4E2D is not printable ASCII, so it
    // should appear as \U00004E2D (the full-width hex escape), NOT as a
    // truncated \xNN or a garbled byte.
    using C32 = Context<char32_t>;
    Grammar<char32_t> g;
    g["cjk"] = terminal(U'\u4E2D');
    g.set_start("cjk");

    std::u32string input = U"x"; // does not match U+4E2D
    C32 ctx{input};
    CHECK_FALSE(g.parse(ctx));

    auto diag = ctx.take_error();
    REQUIRE(diag.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by REQUIRE above; clang-tidy
    // can't model doctest's macro control flow.
    const auto& diag_ref = *diag;
    REQUIRE_FALSE(diag_ref.expected().empty());
    // The expected set contains both the rule name ("cjk") and the literal
    // terminal. Find the Literal item — its text must carry the full-width
    // codepoint escape, proving the value was not narrowed to char (which
    // would yield \x2D, the low byte of 0x4E2D).
    std::string literal_text;
    for (const auto& item : diag_ref.expected()) {
        if (item.kind == ExpectedKind::Literal) {
            literal_text = item.text;
            break;
        }
    }
    CHECK_FALSE(literal_text.empty());
    CHECK(literal_text.find("\\U00004E2D") != std::string::npos);
}

TEST_CASE("char32-ascii-codepoint-renders-as-itself")
{
    // A printable-ASCII-valued char32_t (e.g. U+0061 'a') renders as 'a', same
    // as the char path — no regression for the common ASCII subset.
    using C32 = Context<char32_t>;
    Grammar<char32_t> g;
    g["a"] = terminal(U'a');
    g.set_start("a");

    std::u32string input = U"x";
    C32 ctx{input};
    CHECK_FALSE(g.parse(ctx));

    auto diag = ctx.take_error();
    REQUIRE(diag.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by REQUIRE above; clang-tidy
    // can't model doctest's macro control flow.
    const auto& diag_ref = *diag;
    REQUIRE_FALSE(diag_ref.expected().empty());
    // Find the Literal item (the set also contains the rule name "a").
    std::string literal_text;
    for (const auto& item : diag_ref.expected()) {
        if (item.kind == ExpectedKind::Literal) {
            literal_text = item.text;
            break;
        }
    }
    CHECK(literal_text == "'a'");
}
