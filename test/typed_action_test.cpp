#include "peglib.h"

#include "doctest.h"

#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace peg;

// ---------------------------------------------------------------------------
// Typed semantic-action tests (post-parse fold model).
//
// Typed actions run in a post-parse fold (Grammar::parse_ast), NOT during
// parse. The fold walks the final tree once, dispatching each node to its
// producer rule's registered typed fold, which owns child values as locals and
// moves them up. This is unconditionally move-safe: no value is stored at a
// shared/multi-reader location, so a move-only NodeType composes freely.
//
// These exercise end-to-end:
//   - RuleHandle returned by `g["r"] = body`
//   - typed set_action<F> with positional, no-projection argument matching
//   - the fold reconstructing typed values for every expression shape
//   - terminal(void) filtering vs token(value_type) keeping
//   - left-fold (`a >> *(op >> a)`) producing vector<pair/tuple>
//   - passthrough aliases flowing values at zero cost
// ---------------------------------------------------------------------------

// Copyable NodeType for the common cases.
struct Node
{
    int value = 0;
};
using Ctx = Context<char, Node>;

// Move-only NodeType (yueshi-style AST with unique_ptr children). This is the
// case that the old extractor could not support (copy-removed) and that the
// fold handles by owning values transitively.
struct MoveNode
{
    int value = 0;
    std::unique_ptr<MoveNode> child;

    MoveNode() = default;
    explicit MoveNode(int v) : value{v} {}
    MoveNode(MoveNode&&) noexcept = default;
    MoveNode& operator=(MoveNode&&) noexcept = default;
    MoveNode(const MoveNode&) = delete;
    MoveNode& operator=(const MoveNode&) = delete;
};
using MCtx = Context<char, MoveNode>;

// ---------------------------------------------------------------------------
// 1. Leaf: void body  →  F(Context&, Span)
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: void-body leaf via g.terminal")
{
    Grammar<char, Node> g;
    auto h = (g["a"] = g.terminal('x'));
    h.set_action([](Ctx& c, Span sp) -> Node { return Node{static_cast<int>(c.at(sp.start))}; });

    std::string in = "x";
    Ctx ctx(in);
    auto ast = g.parse_ast("a", ctx);
    REQUIRE(ast);
    CHECK(ast->value == static_cast<int>('x'));
}

// ---------------------------------------------------------------------------
// 2. TokenExpr: value_type body  →  F(Context&, Span, value_type)
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: TokenExpr keeps the matched element")
{
    Grammar<char, Node> g;
    auto h = (g["a"] = g.token('x'));
    h.set_action([](Ctx&, Span, char ch) -> Node { return Node{static_cast<int>(ch)}; });

    std::string in = "x";
    Ctx ctx(in);
    auto ast = g.parse_ast("a", ctx);
    REQUIRE(ast);
    CHECK(ast->value == static_cast<int>('x'));
}

// ---------------------------------------------------------------------------
// 3. Sequence 1-tuple unwrap: terminal(void) + rule  →  F(Context&, Span, T)
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: sequence filters void terminal, unwraps single result")
{
    Grammar<char, Node> g;
    auto inner = (g["inner"] = g.token('1'));
    inner.set_action([](Ctx&, Span, char) -> Node { return Node{7}; });

    auto h = (g["outer"] = g.terminal('a') >> g["inner"]);
    h.set_action([](Ctx&, Span, Node n) -> Node { return Node{n.value + 1}; });
    std::string in = "a1";
    Ctx ctx(in);
    auto ast = g.parse_ast("outer", ctx);
    REQUIRE(ast);
    CHECK(ast->value == 8);
}

// ---------------------------------------------------------------------------
// 4. Sequence 2-tuple: token + rule  →  F(Context&, Span, char, Node)
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: sequence keeps tuple when two non-void children")
{
    Grammar<char, Node> g;
    auto inner = (g["inner"] = g.token('1'));
    inner.set_action([](Ctx&, Span, char) -> Node { return Node{9}; });

    auto h = (g["outer"] = g.token('a') >> g["inner"]);
    h.set_action([](Ctx&, Span /*sp*/, char op, Node n) -> Node {
        return Node{static_cast<int>(op) + n.value};
    });

    std::string in = "a1";
    Ctx ctx(in);
    auto ast = g.parse_ast("outer", ctx);
    REQUIRE(ast);
    CHECK(ast->value == static_cast<int>('a') + 9);
}

// ---------------------------------------------------------------------------
// 5. Repetition → vector<T>
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: zero-or-more yields vector")
{
    Grammar<char, Node> g;
    auto d = (g["d"] = g.token('1'));
    d.set_action([](Ctx&, Span, char) -> Node { return Node{1}; });

    auto h = (g["outer"] = *g["d"]);
    h.set_action([](Ctx&, Span, std::vector<Node> rest) -> Node {
        int sum = 0;
        for (auto& n : rest)
            sum += n.value;
        return Node{sum};
    });

    std::string in = "111";
    Ctx ctx(in);
    auto ast = g.parse_ast("outer", ctx);
    REQUIRE(ast);
    CHECK(ast->value == 3);
}

// ---------------------------------------------------------------------------
// 6. Left-fold: a >> *(op >> a)  →  F(Context&, Span, T, vector<tuple<op,T>>)
//    The canonical yueshi-style operator-precedence shape.
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: left-fold yields vector of (operator, operand) tuples")
{
    Grammar<char, Node> g;
    auto d = (g["d"] = g.token('1'));
    d.set_action([](Ctx&, Span, char) -> Node { return Node{1}; });

    auto h = (g["expr"] = g["d"] >> *(g.token('+') >> g["d"]));
    h.set_action([](Ctx&, Span, Node first, std::vector<std::tuple<char, Node>> rest) -> Node {
        int acc = first.value;
        for (auto& [op, rhs] : rest) {
            (void)op;
            acc += rhs.value;
        }
        return Node{acc};
    });

    for (std::string in : {"1", "1+1", "1+1+1+1"}) {
        Ctx ctx(in);
        auto ast = g.parse_ast("expr", ctx);
        REQUIRE(ast);
        int expected = static_cast<int>(in.size() + 1) / 2;
        CHECK(ast->value == expected);
    }
}

// ---------------------------------------------------------------------------
// 7. Passthrough alias: g["wrap"] = g["d"]  (no set_action) flows the value
//    through at zero cost — the typed value is inherited via the fold.
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: alias rule passes value through with no action")
{
    Grammar<char, Node> g;
    auto d = (g["d"] = g.token('1'));
    d.set_action([](Ctx&, Span, char) -> Node { return Node{42}; });

    g["wrap"] = g["d"]; // alias — no set_action
    std::string in = "1";
    Ctx ctx(in);
    auto ast = g.parse_ast("wrap", ctx);
    REQUIRE(ast);
    CHECK(ast->value == 42);
}

// ---------------------------------------------------------------------------
// 8. Optional → std::optional<T>
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: optional yields std::optional")
{
    Grammar<char, Node> g;
    auto d = (g["d"] = g.token('1'));
    d.set_action([](Ctx&, Span, char) -> Node { return Node{5}; });

    auto h = (g["outer"] = -g["d"]);
    h.set_action([](Ctx&, Span, std::optional<Node> n) -> Node { return Node{n ? n->value : -1}; });

    SUBCASE("present")
    {
        std::string in = "1";
        Ctx ctx(in);
        auto ast = g.parse_ast("outer", ctx);
        REQUIRE(ast);
        CHECK(ast->value == 5);
    }
    SUBCASE("absent")
    {
        std::string in = "";
        Ctx ctx(in);
        auto ast = g.parse_ast("outer", ctx);
        REQUIRE(ast);
        CHECK(ast->value == -1);
    }
}

// ---------------------------------------------------------------------------
// 9. Alternation: both branches share result type
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: alternation requires shared result type")
{
    Grammar<char, Node> g;
    auto a = (g["a"] = g.token('a'));
    a.set_action([](Ctx&, Span, char) -> Node { return Node{1}; });
    auto b = (g["b"] = g.token('b'));
    b.set_action([](Ctx&, Span, char) -> Node { return Node{2}; });

    auto h = (g["outer"] = g["a"] | g["b"]);
    h.set_action([](Ctx&, Span, Node n) -> Node { return Node{n.value * 10}; });

    for (auto [in, expect] :
         {std::pair<std::string, int>{"a", 10}, std::pair<std::string, int>{"b", 20}}) {
        Ctx ctx(in);
        auto ast = g.parse_ast("outer", ctx);
        REQUIRE(ast);
        CHECK(ast->value == expect);
    }
}

// ===========================================================================
// Defect regression cases (the two bugs this fold model fixes).
// ===========================================================================

// ---------------------------------------------------------------------------
// 10. Defect 2: alternation-of-tokens. g.token('+') | g.token('-') has
//     result_of = value_type (char). The old extractor read node->value
//     (NodeType) for alternations → type mismatch / compile error. The fold
//     dispatches on the winner's type, so the matched element is recovered
//     from (ctx, span) regardless of the branch.
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: alternation-of-tokens (Defect 2)")
{
    Grammar<char, Node> g;
    auto h = (g["op"] = g.token('+') | g.token('-'));
    h.set_action([](Ctx&, Span, char op) -> Node { return Node{static_cast<int>(op)}; });

    for (auto [in, expect] :
         {std::pair<std::string, int>{"+", '+'}, std::pair<std::string, int>{"-", '-'}}) {
        Ctx ctx(in);
        auto ast = g.parse_ast("op", ctx);
        REQUIRE(ast);
        CHECK(ast->value == expect);
    }
}

// ---------------------------------------------------------------------------
// 11. Defect 1: move-only NodeType. The old extractor `return node->value;`
//     copy-removed a move-only type → compile error. The fold owns child
//     values transitively, so a move-only NodeType with unique_ptr children
//     composes: by-value params AND vector<MoveOnly>.
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: move-only NodeType (Defect 1)")
{
    Grammar<char, MoveNode> g;
    auto d = (g["d"] = g.token('1'));
    d.set_action([](MCtx&, Span, char) -> MoveNode { return MoveNode{1}; });

    // outer = d >> *(('+' | '-') >> d)  →  F(MCtx&, Span, MoveNode, vector<tuple<char,MoveNode>>)
    // The vector<tuple<char,MoveNode>> holds move-only MoveNode by value — the
    // case that cannot compile under any copy-based or shared_ptr-free model
    // except the fold.
    auto h = (g["expr"] = g["d"] >> *((g.token('+') | g.token('-')) >> g["d"]));
    h.set_action(
        [](MCtx&, Span, MoveNode first, std::vector<std::tuple<char, MoveNode>> rest) -> MoveNode {
            MoveNode acc{first.value};
            for (auto& alt : rest) {
                auto& rhs = std::get<1>(alt);
                acc.value += rhs.value;
            }
            return acc;
        });

    for (std::string in : {"1", "1+1", "1+1-1+1"}) {
        MCtx ctx(in);
        auto ast = g.parse_ast("expr", ctx);
        REQUIRE(ast);
        int expected = static_cast<int>(in.size() + 1) / 2;
        CHECK(ast->value == expected);
    }
}

// ---------------------------------------------------------------------------
// 12. Memo-collision / aliasing hazard (the case the std::move approach
//     broke). Two successful parents reference the same sub-rule S at the
//     same position via a backtracking alternation. Under std::move the first
//     parent's action moved from node->value, leaving the memo-cached node
//     moved-from for the second parent. The fold visits the FINAL tree once
//     (the backtracked branch is gone), so each node is folded exactly once
//     — no double-read, no moved-from hazard.
//
//     Grammar: shared = (P1 >> 'x') | P2 ;  P1 = S ;  P2 = S ;  S = token('s')
//     On "s": P1 matches S (cached at (S,0)), then 'x' fails, branch
//     backtracks. P2 memo-hits (S,0). In the final tree only P2→S survives.
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: backtracking memo-collision folds each node once")
{
    Grammar<char, Node> g;
    auto s = (g["S"] = g.token('s'));
    s.set_action([](Ctx&, Span, char) -> Node { return Node{7}; });

    // P1/P2 are plain aliases of S (no action) — the fold reaches S's action
    // via the producer chain.
    g["P1"] = g["S"];
    g["P2"] = g["S"];

    auto h = (g["shared"] = (g["P1"] >> g.terminal('x')) | g["P2"]);
    h.set_action([](Ctx&, Span, Node n) -> Node { return Node{n.value + 1}; });

    std::string in = "s";
    Ctx ctx(in);
    auto ast = g.parse_ast("shared", ctx);
    REQUIRE(ast);
    // P2 won (P1's 'x' failed): S's action returned 7, shared's action +1.
    CHECK(ast->value == 8);
}

// ===========================================================================
// Hard-feature regression (action + memo / cut / recovery). These use the
// untyped hook where they read node->value directly; the typed path coexists.
// ===========================================================================

// ---------------------------------------------------------------------------
// 13. Packrat memoisation + typed action: a sub-rule referenced twice composes
//     with the fold (each reference folds independently from the final tree).
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: packrat memo composes with the fold")
{
    Grammar<char, Node> g;
    auto d = (g["d"] = g.token('1'));
    d.set_action([](Ctx&, Span, char) -> Node { return Node{7}; });

    auto h = (g["outer"] = g["d"] >> g["d"]);
    h.set_action([](Ctx&, Span, Node a, Node b) -> Node { return Node{a.value + b.value}; });

    std::string in = "11";
    Ctx ctx(in);
    auto ast = g.parse_ast("outer", ctx);
    REQUIRE(ast);
    CHECK(ast->value == 14); // 7 + 7
}

// ---------------------------------------------------------------------------
// 14. Cut-committed failure: the parse fails, parse_ast returns nullopt, no
//     value is produced.
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: cut-committed failure surfaces as parse failure")
{
    Grammar<char, Node> g;
    auto d = (g["d"] = g.token('1'));
    d.set_action([](Ctx&, Span, char) -> Node { return Node{1}; });

    auto h = (g["outer"] = g["d"] >> g.cut() >> g.terminal('x'));
    h.set_action([](Ctx&, Span, Node n) -> Node { return Node{n.value}; });

    std::string in = "1y";
    Ctx ctx(in);
    auto ast = g.parse_ast("outer", ctx);
    CHECK_FALSE(ast); // cut-committed failure → no AST
}

// ---------------------------------------------------------------------------
// 15. Error recovery: a recovered rule's action does NOT fire (the recovery
//     path returns a transparent null tree). parse_ast returns nullopt for a
//     null tree; the diagnostic is still recorded.
// ---------------------------------------------------------------------------
TEST_CASE("typed-action: recovered rule does not invoke the action")
{
    Grammar<char, Node> g;
    int action_runs = 0;
    auto h = (g["outer"] = g.token('1') >> g.terminal(';'));
    h.set_action([&action_runs](Ctx&, Span, char) -> Node {
        ++action_runs;
        return Node{1};
    });
    g["outer"].set_recovery(peg::recover_set<char>({';'}, "outer"));

    std::string in = "9;";
    Ctx ctx(in);
    auto ast = g.parse_ast("outer", ctx);
    CHECK_FALSE(ast);        // recovery → null tree → nullopt
    CHECK(action_runs == 0); // the typed action did NOT run
    CHECK_FALSE(ctx.take_diagnostics().empty());
}
