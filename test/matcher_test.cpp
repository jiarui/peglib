#include "peglib.h"

#include <optional>
#include <string>
#include <vector>

#include <doctest.h>

using namespace peg;

// ---------------------------------------------------------------------------
// MatcherExpr tests: the match-time primitive (weakened lpeg.Cmt).
//
// A matcher fn reads the context read-only and returns the span it consumed;
// MatcherExpr::parse advances the position and builds a node. Its result is
// void — it is a recognizer. Observation of what it matched flows through
// on_match reading the node's span.
//
// These exercise:
//   - g.matcher(fn) composing with >> (it participates in combinators)
//   - on_match firing with the matcher's correct offsets
//   - the committed-tree guarantee: a matcher whose match is backtracked away
//     does NOT fire on_match
//   - rejection (fn returning std::nullopt) failing the parse
//   - a realistic mini long-bracket (count a repeat char, verify the close)
// ---------------------------------------------------------------------------

using Ctx = Context<char, int>;
using NodePtr = Ctx::ParseTreeNodePtr;

// Mini long-bracket: matches `<n><body<n>` — an opening run of N identical
// chars, then any body, then a closing run of the same N chars. The level N is
// decided at match time (not statically), which is exactly what a static
// combinator tree cannot express and a matcher can. Returns the full span.
//
// This mirrors the shape of Lua's [==[ ... ]==] without the bracket noise.
// Templated on the context type so it works under any NodeType.
template<typename C>
static auto match_balanced(char delim)
{
    return [delim](C& c, Span /*start*/) -> std::optional<Span> {
        std::size_t pos = c.mark();
        // Count the opening run of `delim`.
        std::size_t open_end = pos;
        while (open_end < c.input_size() && c.at(open_end) == delim)
            ++open_end;
        std::size_t level = open_end - pos;
        if (level == 0)
            return std::nullopt;
        // Scan for a closing run of the same length, all `delim`.
        std::size_t scan = open_end;
        while (scan + level <= c.input_size()) {
            bool close = true;
            for (std::size_t i = 0; i < level; ++i)
                if (c.at(scan + i) != delim) {
                    close = false;
                    break;
                }
            if (close)
                return Span{pos, scan + level};
            ++scan;
        }
        return std::nullopt;
    };
}

TEST_CASE("matcher-composes-in-sequence")
{
    // matcher >> token: the matcher's node is in the tree; the token's value
    // flows through the typed fold. Both compose because MatcherExpr inherits
    // ParsingExpr.
    // Input "xxaxxa": match_balanced('x') consumes "xxaxx" (level-2 pair:
    // "xx", body "a", "xx"), then token('a') consumes the trailing 'a'.
    Grammar<char, int> g;
    auto h = (g["rule"] = g.matcher(match_balanced<Ctx>('x')) >> g.token('a'));
    (void)h;

    Ctx ctx(std::string{"xxaxxa"});
    REQUIRE(g.parse_ast("rule", ctx));
    CHECK(ctx.mark() == 6);
    CHECK(ctx.ended());
}

TEST_CASE("matcher-on-match-observes-span")
{
    // on_match fires once for the matcher's node, with the matcher's offsets.
    Grammar<char, int> g;
    struct Capture
    {
        std::size_t start = 0;
        std::size_t end = 0;
        int hits = 0;
    } cap;
    g["balanced"] = g.matcher(match_balanced<Ctx>('x'));
    g["balanced"].on_match([&cap](Ctx&, const NodePtr& n) {
        cap.start = n->start_offset;
        cap.end = n->end_offset;
        ++cap.hits;
    });

    // "xxx body xxx" is 12 chars: a level-3 pair (xxx ... xxx) with body
    // " body ". The matcher consumes the whole thing.
    Ctx ctx(std::string{"xxx body xxx"});
    REQUIRE(g.parse_ast("balanced", ctx));
    CHECK(cap.start == 0);
    CHECK(cap.end == 12);
    CHECK(cap.hits == 1);
}

TEST_CASE("matcher-backtracking-does-not-fire-on-match")
{
    // Committed-tree guarantee: an alternation whose first branch matches via
    // a matcher but is then backtracked away (because a later sequence element
    // fails) must NOT fire on_match for that speculative match.
    //
    // Grammar: rule = (balanced >> terminal('!')) / terminal('a')
    // Input:   "xxaxx" — balanced matches the whole input (level-2 pair), but
    //          the following '!' is absent (EOF), so branch 1 fails and
    //          backtracks; branch 2 fails too (input isn't 'a').
    // The matcher's on_match must NOT have fired for the speculative match.
    Grammar<char, int> g;
    int hits = 0;
    g["balanced"] = g.matcher(match_balanced<Ctx>('x'));
    g["balanced"].on_match([&hits](Ctx&, const NodePtr&) { ++hits; });
    // Force backtrack: balanced succeeds but the following terminal fails, so
    // the whole sequence fails and the matcher's match is discarded.
    g["rule"] = (g["balanced"] >> g.terminal('!')) | g.terminal('a');

    Ctx ctx(std::string{"xxaxx"});
    // Parse fails: '!' absent (EOF after the matcher) and input isn't 'a'.
    CHECK_FALSE(g.parse_ast("rule", ctx));
    // The matcher matched the whole input speculatively, but that match was
    // backtracked away — on_match must not have fired.
    CHECK(hits == 0);
}

TEST_CASE("matcher-reject-fails-parse")
{
    // fn returning std::nullopt rejects; the parse fails and on_match does not
    // fire.
    Grammar<char, int> g;
    int hits = 0;
    g["rule"] = g.matcher([](Ctx& c, Span) -> std::optional<Span> {
        // Reject unless the first char is 'y'.
        if (!c.ended() && c.at(c.mark()) == 'y')
            return Span{c.mark(), c.mark() + 1};
        return std::nullopt;
    });
    g["rule"].on_match([&hits](Ctx&, const NodePtr&) { ++hits; });

    Ctx ctx(std::string{"n"});
    CHECK_FALSE(g.parse_ast("rule", ctx));
    CHECK(hits == 0);

    Ctx ctx2(std::string{"y"});
    REQUIRE(g.parse_ast("rule", ctx2));
    CHECK(hits == 1);
}

TEST_CASE("matcher-result-is-void-filtered-from-sequence")
{
    // A matcher in a sequence contributes no typed argument: the action
    // receives only the token's value, not a matcher slot. This confirms
    // result_of<MatcherExpr> is void (filtered like terminal).
    struct Node
    {
        char matched = '\0';
    };
    using NCtx = Context<char, Node>;
    Grammar<char, Node> g;
    auto h = (g["rule"] = g.matcher(match_balanced<NCtx>('x')) >> g.token('a'));
    // The action takes just (Context&, Span, char) — no matcher argument.
    h.set_action([](NCtx&, Span, char ch) -> Node { return Node{ch}; });

    // Input "xxaxxa": match_balanced('x') consumes "xxaxx", token('a') the 'a'.
    NCtx ctx(std::string{"xxaxxa"});
    auto ast = g.parse_ast("rule", ctx);
    REQUIRE(ast);
    CHECK(ast->matched == 'a');
}
