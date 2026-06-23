// Non-integral value_type smoke test: verifies that Context<TokenStruct> can
// drive a real parse through the value / set / range / sequence terminal
// factories, and that the diagnostic layer renders a non-integral element via
// the to_display customization point (ADL hook) — or falls back to the
// "<token>" placeholder when no hook is provided.
//
// This is the user-visible payoff of decoupling peglib's rendering path from
// the assumption that Context::value_type is integral: a downstream token
// type is now first-class for matching AND error reporting without wrapping
// it in a predicate terminal.
//
// NOTE: the stub element types below are deliberately trivially-copyable.
// peglib's Context::substr() returns std::basic_string<value_type>, whose
// libstdc++ implementation requires char_traits<value_type> and therefore a
// trivially-copyable value_type. Supporting a non-trivially-copyable token
// struct (e.g. one carrying a std::string lexeme) requires further work in
// the Context source layer, which is outside the scope of the rendering fix
// exercised here. These stubs still validate the complete non-integral
// rendering path: integral casts are bypassed, the to_display CPO is reached,
// the ADL hook fires, and the placeholder fallback is exercised.

#include "peglib.h"

#include "doctest.h"

#include <array>
#include <set>
#include <string>
#include <vector>

using namespace peg;

namespace
{
// A non-integral, trivially-copyable element type standing in for a downstream
// lexer token. Provides == and defaulted <=> so it satisfies PegValue,
// PegValueSet and PegValueRange (relational ordering is by id).
struct StubTok
{
    int id;

    bool operator==(const StubTok&) const = default;
    auto operator<=>(const StubTok&) const = default;
};

// ADL customization hook discovered by to_display_cpo's non-integral branch.
// Defining it in StubTok's namespace (here, the anonymous namespace) is the
// supported way to give a token type meaningful diagnostic text.
std::string to_display(const StubTok& t)
{
    return "TK" + std::to_string(t.id);
}

// A second non-integral element type that deliberately provides NO to_display
// hook, to exercise the "<token>" fallback path.
struct HooklessTok
{
    int id;
    bool operator==(const HooklessTok&) const = default;
    auto operator<=>(const HooklessTok&) const = default;
};

// A type with no equality, used only as a negative concept self-check.
struct NoEq
{};
} // namespace

// ---------------------------------------------------------------------------
// Concept self-checks: the stubs satisfy exactly the contracts they claim to.
// ---------------------------------------------------------------------------
static_assert(PegContext<Context<StubTok>>, "Context<StubTok> must satisfy PegContext");
static_assert(PegValue<StubTok>, "StubTok must satisfy PegValue");
static_assert(PegValueSet<StubTok>, "StubTok must satisfy PegValueSet");
static_assert(PegValueRange<StubTok>, "StubTok must satisfy PegValueRange");
static_assert(PegValueSeq<std::vector<StubTok>>, "vector<StubTok> must satisfy PegValueSeq");
static_assert(PegValue<HooklessTok>, "HooklessTok must satisfy PegValue");
static_assert(!PegValue<NoEq>, "NoEq must NOT satisfy PegValue");

// ---------------------------------------------------------------------------
// Local helper: extract the single Literal/Range ExpectedItem text from a
// Diagnostic, mirroring the idiom in char32_smoke_test.cpp.
// ---------------------------------------------------------------------------
namespace
{
std::string literal_text(const std::optional<Diagnostic>& diag, ExpectedKind kind)
{
    REQUIRE(diag.has_value());
    const auto& ref = diag.value();
    REQUIRE_FALSE(ref.expected().empty());
    for (const auto& item : ref.expected()) {
        if (item.kind == kind) {
            return item.text;
        }
    }
    return {};
}
} // namespace

TEST_CASE("token-char-single-terminal-matches-and-renders")
{
    // terminal(StubTok{...}) is constructible and drives a real parse against
    // a std::vector<StubTok> input (CTAD deduces Context<StubTok>).
    using Ctx = Context<StubTok>;
    Grammar<StubTok> g;
    StubTok want{1};
    g["t"] = terminal(want);
    g.set_start("t");

    SUBCASE("match succeeds and consumes one token")
    {
        std::vector<StubTok> input{StubTok{1}, StubTok{2}};
        Ctx ctx{input};
        CHECK(g.parse(ctx));
        CHECK_FALSE(ctx.ended()); // exactly one token consumed
    }

    SUBCASE("failure renders the expected token via the ADL hook")
    {
        std::vector<StubTok> input{StubTok{9}};
        Ctx ctx{input};
        CHECK_FALSE(g.parse(ctx));

        auto text = literal_text(ctx.take_error(), ExpectedKind::Literal);
        CHECK_FALSE(text.empty());
        // Hook output is angle-bracketed to signal a non-character token.
        CHECK(text == "<TK1>");
    }
}

TEST_CASE("token-char-range-terminal-renders")
{
    // terminal(lo, hi) requires operator<= / >= on the element; on failure it
    // emits a Range ExpectedItem joining both endpoints' hook output with "..".
    using Ctx = Context<StubTok>;
    Grammar<StubTok> g;
    StubTok lo{1}, hi{3};
    g["r"] = terminal(lo, hi);
    g.set_start("r");

    SUBCASE("inside range matches")
    {
        std::vector<StubTok> input{StubTok{2}};
        Ctx ctx{input};
        CHECK(g.parse(ctx));
        CHECK(ctx.ended());
    }

    SUBCASE("outside range fails and renders 'lo'..'hi'")
    {
        std::vector<StubTok> input{StubTok{9}};
        Ctx ctx{input};
        CHECK_FALSE(g.parse(ctx));

        auto text = literal_text(ctx.take_error(), ExpectedKind::Range);
        CHECK_FALSE(text.empty());
        CHECK(text == "<TK1>..<TK3>");
    }
}

TEST_CASE("token-char-set-terminal-renders")
{
    // terminal(std::set<StubTok>{...}) requires operator<; on failure it emits
    // a Range ExpectedItem listing each element's hook output, comma-joined.
    using Ctx = Context<StubTok>;
    Grammar<StubTok> g;
    std::set<StubTok> values{StubTok{1}, StubTok{2}};
    g["s"] = terminal(values);
    g.set_start("s");

    std::vector<StubTok> input{StubTok{9}};
    Ctx ctx{input};
    CHECK_FALSE(g.parse(ctx));

    auto text = literal_text(ctx.take_error(), ExpectedKind::Range);
    CHECK_FALSE(text.empty());
    // std::set iterates in ascending order by StubTok's defaulted <=>, so the
    // rendering order is deterministic: "<TK1>, <TK2>".
    CHECK(text == "<TK1>, <TK2>");
}

TEST_CASE("token-char-seq-terminal-renders")
{
    // terminalSeq(std::vector<StubTok>{...}) failing parse yields a Literal
    // ExpectedItem with the concatenated hook output inside double quotes —
    // this is the path routed through to_display_cpo at Terminals.h.
    using Ctx = Context<StubTok>;
    Grammar<StubTok> g;
    std::vector<StubTok> seq{StubTok{1}, StubTok{2}};
    g["seq"] = terminalSeq(seq);
    g.set_start("seq");

    SUBCASE("full sequence matches")
    {
        std::vector<StubTok> input{StubTok{1}, StubTok{2}};
        Ctx ctx{input};
        CHECK(g.parse(ctx));
        CHECK(ctx.ended());
    }

    SUBCASE("mismatch fails and renders concatenated hook text")
    {
        std::vector<StubTok> input{StubTok{9}};
        Ctx ctx{input};
        CHECK_FALSE(g.parse(ctx));

        auto text = literal_text(ctx.take_error(), ExpectedKind::Literal);
        CHECK_FALSE(text.empty());
        // Each element rendered via to_display_cpo, concatenated, then wrapped
        // in double quotes by the narrow escape_string_for_expected overload.
        CHECK(text == "\"TK1TK2\"");
    }
}

TEST_CASE("token-char-no-hook-falls-back-to-placeholder")
{
    // A non-integral element type with NO ADL to_display hook must still
    // compile and produce a non-empty diagnostic using the "<token>" fallback.
    using Ctx = Context<HooklessTok>;
    Grammar<HooklessTok> g;
    g["t"] = terminal(HooklessTok{1});
    g.set_start("t");

    std::vector<HooklessTok> input{HooklessTok{9}};
    Ctx ctx{input};
    CHECK_FALSE(g.parse(ctx));

    auto text = literal_text(ctx.take_error(), ExpectedKind::Literal);
    CHECK_FALSE(text.empty());
    CHECK(text == "<<token>>"); // angle-bracketed placeholder
}
