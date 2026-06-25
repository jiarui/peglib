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
// The stub element types below (StubTok / HooklessTok) are trivially-copyable
// for historical simplicity, but non-trivially-copyable token types are now
// fully supported too: see the "non-trivially-copyable token" test cases at
// the end of this file, which use a Token carrying a std::variant (hence not
// trivially-copyable) together with a custom NodeType and the Grammar member
// factories (g.terminal / g.terminalSeq / operators / semantic actions).

#include "peglib.h"

#include "doctest.h"

#include <array>
#include <set>
#include <string>
#include <variant>
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
    // g.terminal(StubTok{...}) is constructible and drives a real parse against
    // a std::vector<StubTok> input (CTAD deduces Context<StubTok>).
    using Ctx = Context<StubTok>;
    Grammar<StubTok> g;
    StubTok want{1};
    g["t"] = g.terminal(want);
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
    // g.terminal(lo, hi) requires operator<= / >= on the element; on failure it
    // emits a Range ExpectedItem joining both endpoints' hook output with "..".
    using Ctx = Context<StubTok>;
    Grammar<StubTok> g;
    StubTok lo{1}, hi{3};
    g["r"] = g.terminal(lo, hi);
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
    // g.terminal(std::set<StubTok>{...}) requires operator<; on failure it emits
    // a Range ExpectedItem listing each element's hook output, comma-joined.
    using Ctx = Context<StubTok>;
    Grammar<StubTok> g;
    std::set<StubTok> values{StubTok{1}, StubTok{2}};
    g["s"] = g.terminal(values);
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
    // g.terminalSeq(std::vector<StubTok>{...}) failing parse yields a Literal
    // ExpectedItem with the concatenated hook output inside double quotes —
    // this is the path routed through to_display_cpo at Terminals.h.
    using Ctx = Context<StubTok>;
    Grammar<StubTok> g;
    std::vector<StubTok> seq{StubTok{1}, StubTok{2}};
    g["seq"] = g.terminalSeq(seq);
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
    g["t"] = g.terminal(HooklessTok{1});
    g.set_start("t");

    std::vector<HooklessTok> input{HooklessTok{9}};
    Ctx ctx{input};
    CHECK_FALSE(g.parse(ctx));

    auto text = literal_text(ctx.take_error(), ExpectedKind::Literal);
    CHECK_FALSE(text.empty());
    CHECK(text == "<<token>>"); // angle-bracketed placeholder
}

// ===========================================================================
// Non-trivially-copyable token + custom NodeType end-to-end.
//
// These cases are the direct validation of the two fixes that made token-level
// parsing with a custom AST node type possible:
//   1. Context::substr was removed (it forced std::basic_string<value_type>,
//      which is ill-formed for a non-trivially-copyable value_type). The
//      PegContext concept no longer names basic_string, so Context<RealTok,N>
//      satisfies it.
//   2. The free peg::terminal factories hardcoded Context<elem> (monostate);
//      they are now Grammar member factories (g.terminal) that close over the
//      Grammar's own Context, so expressions compose and assign into a
//      Rule<Context<CharT, NodeType>> with the correct NodeType.
//
// RealTok carries a std::variant and is therefore NOT trivially copyable —
// exactly the shape that used to trigger libstdc++'s static_assert inside
// std::basic_string<RealTok>.
// ===========================================================================
namespace
{
// A realistic downstream lexer token: an id plus a payload variant. Carrying
// std::variant (and indirectly std::string) makes it non-trivially-copyable,
// which std::basic_string<RealTok> cannot tolerate.
struct RealTok
{
    int id;
    std::variant<long long, double, std::string> payload;

    // Equality/ordering by id only: a token's identity is its kind, not its
    // payload (two NAME tokens are "the same terminal" regardless of lexeme).
    bool operator==(const RealTok& o) const { return id == o.id; }
    auto operator<=>(const RealTok& o) const { return id <=> o.id; }
};
static_assert(!std::is_trivially_copyable_v<RealTok>,
              "RealTok must NOT be trivially copyable — this is the point of the test");

// A user-defined semantic-action product type (non-monostate NodeType).
struct RealNode
{
    int matched_id = 0;
    std::string lexeme;
};

// ADL hook so diagnostics render something meaningful instead of "<token>".
std::string to_display(const RealTok& t)
{
    return "TK" + std::to_string(t.id);
}
} // namespace

TEST_CASE("non-trivial-token-satisfies-pegcontext-with-custom-node-type")
{
    // Problem 1 fix: PegContext must hold for Context<RealTok, RealNode>.
    // Before the fix, the concept named std::basic_string<RealTok> via the
    // substr requirement and failed libstdc++'s trivially-copyable assert.
    static_assert(PegContext<Context<RealTok, RealNode>>,
                  "Context<RealTok, RealNode> must satisfy PegContext");
    static_assert(PegContext<Context<RealTok>>,
                  "Context<RealTok> (monostate) must satisfy PegContext");
}

TEST_CASE("non-trivial-token-grammar-member-factories-compile")
{
    // Problem 2 fix: Grammar member factories build expressions whose Context
    // carries the Grammar's NodeType, so they assign into the Grammar's rules
    // and compose via the free operators (which deduce Context from operands).
    Grammar<RealTok, RealNode> g;

    g["single"] = g.terminal(RealTok{1, {}});
    g["range"] = g.terminal(RealTok{1, {}}, RealTok{9, {}});
    g["pred"] = g.terminal([](const RealTok& t) { return t.id > 0; });
    g["set"] = g.terminal(std::set<RealTok>{RealTok{1, {}}, RealTok{2, {}}});
    g["seq"] = g.terminalSeq(std::vector<RealTok>{RealTok{1, {}}, RealTok{2, {}}});

    // Combining operators deduce Context<RealTok, RealNode> from operands.
    g["combo"] = g["single"] >> g["range"] | g["pred"];
    g["star"] = *g["single"];
    g["opt"] = -g["single"];
    g["not"] = !g["single"];
    g["and"] = &g["single"];
    g["lex"] = g.lexeme(+g["single"]);

    // Mixed-literal operator expr >> value_type binds to expr's Context.
    g["mix"] = g["single"] >> RealTok{2, {}};

    // Sanity: the grammar is well-formed (no undefined rules).
    CHECK(g.undefined_rules().empty());
}

TEST_CASE("non-trivial-token-end-to-end-parse-with-semantic-action")
{
    // Full pipeline: token stream in, custom NodeType out via a semantic
    // action that reads the matched token via ctx.at(start_offset).
    Grammar<RealTok, RealNode> g;

    // Match three specific tokens in sequence: open(1), mid(5), close(2).
    g["open"] = g.terminal(RealTok{1, {}});
    g["mid"] = g.terminal(RealTok{5, {}});
    g["close"] = g.terminal(RealTok{2, {}});
    auto group = (g["group"] = g["open"] >> g["mid"] >> g["close"]);
    group.set_action([](Context<RealTok, RealNode>& ctx,
                        peg::Span sp,
                        const RealNode& /*open*/,
                        const RealNode& /*mid*/,
                        const RealNode& /*close*/) {
        // Tokens carry their own identity — read the opener by offset, no
        // string slicing needed. (The open/mid/close child RealNodes are
        // unused; this action keys on the opener token's id.)
        RealNode n;
        n.matched_id = ctx.at(sp.start).id;
        n.lexeme = "group";
        return n;
    });
    g.set_start("group");

    std::vector<RealTok> input{RealTok{1, std::string{"x"}}, RealTok{5, 42}, RealTok{2, 3.14}};
    Context<RealTok, RealNode> ctx{input};

    auto ast = g.parse_ast("group", ctx);
    REQUIRE(ast);
    CHECK(ast->matched_id == 1);
    CHECK(ast->lexeme == "group");
    CHECK(ctx.ended()); // whole stream consumed
}

TEST_CASE("non-trivial-token-failure-renders-via-adl-hook")
{
    // A non-trivially-copyable token's diagnostic still routes through the
    // to_display ADL hook (defined above for RealTok), producing "<TK1>".
    using Ctx = Context<RealTok, RealNode>;
    Grammar<RealTok, RealNode> g;
    g["t"] = g.terminal(RealTok{1, {}});
    g.set_start("t");

    std::vector<RealTok> input{RealTok{9, {}}};
    Ctx ctx{input};
    CHECK_FALSE(g.parse(ctx));

    auto text = literal_text(ctx.take_error(), ExpectedKind::Literal);
    CHECK_FALSE(text.empty());
    CHECK(text == "<TK1>");
}
