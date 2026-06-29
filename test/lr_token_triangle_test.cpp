// ---------------------------------------------------------------------------
// Token-level left-recursion seed-grow, in the real-world shape a tokenizing
// parser (e.g. yueshi's Lua grammar) uses: a non-integral token type AND a
// functioncall written with a repetition body
//   functioncall = (call_plain | call_method) >> *(args | call_method_tail)
// routed through the indirect/mutual LR cycle
//   exp → prefixexp → var → var_field → prefixexp
//
// This exercises the grow-loop fixed-point fix (NonTerminal.h parseImpl): when
// an inner head grows a suffix in one iteration then re-parses to a regressed
// (shorter) result in the next, the grown result must survive. Before the fix
// the grown suffix was dropped.
//
// ORDERING (PEG ordered choice + seed-grow — see lr_triangle_repro_test):
//   - var branches: RECURSIVE suffixes (var_field/var_index) BEFORE base
//     (var_name), or the base matches the seed and short-circuits before the
//     suffix can extend it.
//   - prefixexp branches: functioncall BEFORE var, or var's bare Name matches
//     the seed and short-circuits before a call suffix can extend it.
// This is the ordering a correct Lua grammar uses (yueshi documents the same).
//
// EOS semantics: the token stream ends with a T_EOF sentinel that NO grammar
// rule consumes (mirrors yueshi: parse() succeeds when consumed == size-1, the
// sentinel left unconsumed). So the helper checks consumed == size-1, NOT
// context.ended() (which would demand the sentinel itself be consumed).
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <cctype>
#include <string>
#include <vector>

using namespace peg;

// ===========================================================================
// PART 1 — char-level permutation matrix (baseline data).
//
// Prints a Y/N matrix of all 6 orderings of `var`'s branches × 5 inputs. Reads
// as DATA (no hard assertion): it shows empirically that ONLY the orderings
// with a recursive suffix first (perms 3,4,5 — var_field/var_index before
// var_name) grow `a.b`/`a.b.c`. Base-first orderings (perms 0,1,2) match only
// the seed. This is the PEG ordered-choice + LR interaction the fix does NOT
// change (it is a grammar-ordering requirement, not a library bug).
// ===========================================================================
namespace
{
// `Name` matches any ASCII letter, so `a.b`, `a.b.c` are genuinely valid.
auto name_pred()
{
    return [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; };
}

bool ok_char(const Grammar<>& g, std::string input)
{
    Context<char> context(input);
    return g.parse(context) && context.ended();
}

using RuleRef = decltype(std::declval<Grammar<>>()["x"]);

void build_factored_triangle(Grammar<>& g, int perm)
{
    static const int perms[6][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2},
        {1, 2, 0}, {2, 0, 1}, {2, 1, 0},
    };
    const int* p = perms[perm];

    g["Name"] = g.terminal(name_pred());
    g["args"] = g.terminal('(') >> g.terminal(')');

    g["var_name"]  = g["Name"];
    g["var_field"] = g["prefixexp"] >> g.terminal('.') >> g["Name"];
    g["var_index"] = g["prefixexp"] >> g.terminal('[') >> g["exp"] >> g.terminal(']');

    g["call_plain"]  = g["prefixexp"] >> g["args"];
    g["call_method"] = g["prefixexp"] >> g.terminal(':') >> g["Name"] >> g["args"];

    g["functioncall"] = g["call_method"] | g["call_plain"];
    // functioncall BEFORE var (see ordering note): otherwise a bare Name seed
    // short-circuits before a call suffix can extend it.
    g["prefixexp"] = g["functioncall"] | g["var"] | (g.terminal('(') >> g["exp"] >> g.terminal(')'));
    g["exp"] = g["prefixexp"] | g.terminal('0', '9');

    RuleRef parts[3] = { g["var_name"], g["var_index"], g["var_field"] };
    g["var"] = parts[p[0]] | parts[p[1]] | parts[p[2]];

    g["chunk"] = g["exp"];
    g.set_start("chunk");
}
} // namespace

TEST_CASE("lr-token-triangle: char-level permutation matrix (baseline data)")
{
    const char* inputs[] = {"a", "a.b", "a.b.c", "a()", "a()()"};
    for (int perm = 0; perm < 6; ++perm) {
        Grammar<> g;
        build_factored_triangle(g, perm);
        std::string row;
        for (const char* in : inputs) {
            row += ok_char(g, in) ? "Y " : "N ";
        }
        MESSAGE("perm " << perm << "  " << row);
    }
    // Hard claim: perms with a recursive suffix first (3,4,5) grow a.b.c;
    // base-first perms (0,1,2) do not.
    for (int perm = 0; perm < 6; ++perm) {
        Grammar<> g;
        build_factored_triangle(g, perm);
        bool grows_ab = ok_char(g, "a.b");
        bool suffix_first = (perm >= 3);
        CHECK_MESSAGE(grows_ab == suffix_first,
                      "perm " << perm << (suffix_first ? " (suffix-first)" : " (base-first)")
                              << " a.b=" << (grows_ab ? "Y" : "N"));
    }
}

// ===========================================================================
// PART 2 — token-level triangle in yueshi's real shape.
// ===========================================================================
namespace
{
struct Tok
{
    int id;
    std::string text; // only meaningful for id == NAME

    bool operator==(const Tok&) const = default;
};

enum : int { T_NAME = 1, T_DOT, T_LBRACK, T_RBRACK, T_LPAREN, T_RPAREN,
             T_COLON, T_DIGIT, T_COMMA, T_EOF };

using TokCtx = Context<Tok>;
using TokGrammar = Grammar<Tok>;

auto tok(TokGrammar& g, int id)
{
    return g.terminal([id](const Tok& t) { return t.id == id; });
}

// yueshi EOS semantics: the stream ends in a T_EOF sentinel that no rule
// consumes. A full parse consumes everything EXCEPT the sentinel, i.e.
// consumed == input_size - 1. (context.ended() would wrongly demand the
// sentinel itself be consumed.)
bool ok_tok(const TokGrammar& g, std::vector<Tok> input)
{
    TokCtx ctx(input);
    bool parsed = g.parse(ctx);
    return parsed && ctx.mark() + 1 == ctx.input_size();
}

// Build yueshi's var/functioncall/prefixexp triangle at the TOKEN level,
// keeping functioncall as a repetition body (the shape yueshi uses).
// `order` ∈ {0,1}: 0 = base-first (BROKEN: matches only the seed),
//                  1 = suffix-first (CORRECT: grows). See ordering note.
void build_token_triangle(TokGrammar& g, int order)
{
    g["args"] = tok(g, T_LPAREN) >> -g["explist"] >> tok(g, T_RPAREN);
    g["explist"] = g["exp"] >> *(tok(g, T_COMMA) >> g["exp"]);

    g["var_name"]  = g["Name"];
    g["var_field"] = g["prefixexp"] >> tok(g, T_DOT) >> g["Name"];
    g["var_index"] = g["prefixexp"] >> tok(g, T_LBRACK) >> g["exp"] >> tok(g, T_RBRACK);

    g["call_plain"]  = g["prefixexp"] >> g["args"];
    g["call_method"] = g["prefixexp"] >> tok(g, T_COLON) >> g["Name"] >> g["args"];
    g["call_method_tail"] = tok(g, T_COLON) >> g["Name"] >> g["args"];

    g["functioncall"] =
        (g["call_plain"] | g["call_method"]) >> *(g["args"] | g["call_method_tail"]);

    // functioncall BEFORE var (ordering note): otherwise var's bare Name seed
    // short-circuits prefixexp before a call suffix can extend it.
    g["prefixexp"] = g["functioncall"] | g["var"] | (tok(g, T_LPAREN) >> g["exp"] >> tok(g, T_RPAREN));
    g["exp"] = g["prefixexp"] | g["digit"];

    if (order == 0)
        g["var"] = g["var_name"] | g["var_index"] | g["var_field"];  // base-first: BROKEN
    else
        g["var"] = g["var_field"] | g["var_index"] | g["var_name"];  // suffix-first: CORRECT

    g["Name"]  = g.terminal([](const Tok& t) { return t.id == T_NAME; });
    g["digit"] = g.terminal([](const Tok& t) { return t.id == T_DIGIT; });

    g["chunk"] = g["exp"];
    g.set_start("chunk");
}
} // namespace

TEST_CASE("lr-token-triangle: token-level yueshi-shape matrix (baseline data)")
{
    auto mk = [](int id, std::string t = "") {
        return Tok{id, std::move(t)};
    };
    struct Input { const char* label; std::vector<Tok> toks; };
    std::vector<Input> inputs = {
        {"a",        {mk(T_NAME, "a")}},
        {"a.b",      {mk(T_NAME, "a"), mk(T_DOT), mk(T_NAME, "b")}},
        {"a.b.c",    {mk(T_NAME, "a"), mk(T_DOT), mk(T_NAME, "b"),
                      mk(T_DOT), mk(T_NAME, "c")}},
        {"a[1]",     {mk(T_NAME, "a"), mk(T_LBRACK), mk(T_DIGIT, "1"), mk(T_RBRACK)}},
        {"a()",      {mk(T_NAME, "a"), mk(T_LPAREN), mk(T_RPAREN)}},
        {"a()()",    {mk(T_NAME, "a"), mk(T_LPAREN), mk(T_RPAREN),
                      mk(T_LPAREN), mk(T_RPAREN)}},
    };

    for (int order = 0; order < 2; ++order) {
        TokGrammar g;
        build_token_triangle(g, order);
        std::string row;
        for (auto& in : inputs) {
            auto stream = in.toks;
            stream.push_back(mk(T_EOF));
            row += ok_tok(g, stream) ? "Y " : "N ";
        }
        MESSAGE("token order=" << order
                               << (order == 1 ? " (suffix-first, CORRECT)" : " (base-first, BROKEN)")
                               << "  " << row);
    }
}

// ---------------------------------------------------------------------------
// HARD assertions (GREEN after the grow-loop fix): the suffix-first ordering
// grows every suffix form at the token level, including the repetition-bodied
// functioncall. Uses CHECK_MESSAGE so one failing input does not mask the rest.
// ---------------------------------------------------------------------------
TEST_CASE("lr-token-triangle: suffix-first grows all suffix forms")
{
    TokGrammar g;
    build_token_triangle(g, /*suffix-first*/ 1);
    auto mk = [](int id, std::string t = "") { return Tok{id, std::move(t)}; };

    auto run = [&](std::vector<Tok> toks) {
        toks.push_back(mk(T_EOF));
        return ok_tok(g, toks);
    };

    CHECK_MESSAGE(run({mk(T_NAME, "a")}), "base case a — seed must at least plant");
    CHECK_MESSAGE(run({mk(T_NAME, "a"), mk(T_DOT), mk(T_NAME, "b")}), "a.b — one field suffix");
    CHECK_MESSAGE(run({mk(T_NAME, "a"), mk(T_DOT), mk(T_NAME, "b"),
                       mk(T_DOT), mk(T_NAME, "c")}), "a.b.c — two field suffixes");
    CHECK_MESSAGE(run({mk(T_NAME, "a"), mk(T_LBRACK), mk(T_DIGIT, "1"), mk(T_RBRACK)}),
                  "a[1] — index suffix");
    CHECK_MESSAGE(run({mk(T_NAME, "a"), mk(T_LPAREN), mk(T_RPAREN)}), "a() — call suffix");
    CHECK_MESSAGE(run({mk(T_NAME, "a"), mk(T_LPAREN), mk(T_RPAREN),
                       mk(T_LPAREN), mk(T_RPAREN)}), "a()() — chained-call suffix");
}
