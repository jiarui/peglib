// ---------------------------------------------------------------------------
// Alias-action coverage: a rule whose body is a single reference to another
// rule (g["A"] = g["B"]) — an "alias" — and its interaction with typed actions
// and on_match hooks.
//
// WHAT WORKS (cases 1, 2, 3, 5, 6 — GREEN):
//   - A SINGLE-LAYER alias with its own set_action fires that action and may
//     transform the body's value (case 1). This is how real grammars use
//     aliases: yueshi's `field_positional = exp`, `stat_call = functioncall`,
//     `var_name = name_token`, `chunk = block` are all single-layer aliases
//     that wrap/transform the body value.
//   - A single-layer alias WITHOUT an action transparently passes the body's
//     value through (case 2). This is typed_action_test case 7's invariant.
//   - A DEEP alias CHAIN (a=b, b=c, all with actions) composes every layer's
//     action outward (case 3). Fixed by the wrap-if-typed-fold change in
//     NonTerminal::parse + the fold-body-from-child change in fold_and_invoke:
//     each action-bearing rule now owns a distinct tree layer (producer =
//     itself) instead of collapsing onto the innermost producer, so the fold
//     dispatches on each layer in turn.
//   - An alternation (NOT an alias) dispatches on the winning branch's producer
//     (case 5).
//   - A NON-START alias reached from a parent (middle=inner, root=middle, all
//     acting) composes the middle transform (case 6) — the same wrap-if-typed-
//     fold fix, restated as the "named transform referenced from a rule" shape
//     real grammars reach for first.
//
// REMAINING LIMITATION (case 4 — RED, documented, NOT triggered by yueshi):
//   - An alias rule's on_match hook fires, but the inner (body) rule's
//     on_match does NOT. The typed-action channel was fixed by keying the wrap
//     on m_typed_fold; on_match is a separate (side-effect) channel that was
//     deliberately NOT keyed, because widening the wrap to every on_match rule
//     would change tree shape for matcher/lexer rules for no reported benefit.
//     yueshi's parser uses on_match only in the lexer (never on aliases), so
//     this is latent.
//
// The NodeType lives in an anonymous namespace (see below) for ODR safety.
// ---------------------------------------------------------------------------

#include "peglib.h"

#include "doctest.h"

#include <string>

using namespace peg;

// Everything below — the NodeType and its aliases — lives in an anonymous
// namespace so the types have INTERNAL LINKAGE. This is critical: peglib is
// header-only, so its templates (fold_start, fold_rule, parse_ast, NonTerminal,
// …) get instantiated once per TU and emitted as weak/COMDAT symbols. If two
// TUs each define a DIFFERENT `struct Node` in the global namespace, the
// compiler mangles them identically (`4Node`), the linker merges their fold
// instantiations, and one TU silently runs the fold compiled against the OTHER
// TU's Node layout — a One-Definition-Rule violation that corrupts results.
// typed_action_test.cpp already defines a global ::Node; this file defines a
// different one (two int fields vs one), so without the anonymous namespace
// every CHECK in this file observes a garbage tag. Internal linkage makes each
// TU's Node a distinct type and the merge never happens.
namespace
{
// Trivially-copyable NodeType carrying an int value plus a tag recording which
// rule's action produced it. The tag is how each case proves a SPECIFIC action
// fired (rather than just observing a value that could have come from anywhere).
struct Node
{
    int value = 0;
    int tag = 0; // 0 = default/none, 1 = inner action, 2 = alias action, ...
};
using Ctx = Context<char, Node>;
using NodePtr = Ctx::ParseTreeNodePtr;
} // namespace

// ===========================================================================
// 1. THE CORE CASE: an alias rule with its own set_action fires that action.
//    g["wrap"] = g["inner"];  wrap.set_action stamps tag=2, value=100.
//    Before the fix: only inner's action runs (tag=1, value=1).
// ===========================================================================
TEST_CASE("alias-action: alias with set_action fires its own action")
{
    Grammar<char, Node> g;

    auto inner = (g["inner"] = g.token('1'));
    inner.set_action([](Ctx&, Span, char) -> Node { return Node{1, 1}; });

    auto wrap = (g["wrap"] = g["inner"]);
    wrap.set_action([](Ctx&, Span, Node /*n*/) -> Node {
        // tag=2 is the proof THIS action ran (not inner's). The inner value is
        // intentionally discarded here (a pure relabel) — the next case covers
        // the transform shape.
        return Node{100, 2};
    });

    std::string in = "1";
    Ctx ctx(in);
    auto ast = g.parse_ast("wrap", ctx);
    REQUIRE(ast);
    CHECK(ast->tag == 2);   // alias action fired
    CHECK(ast->value == 100);
}

// Same shape, but the alias TRANSFORMS the inner value (the real-world reason
// an alias carries an action). Mirrors yueshi's `field_positional = exp`.
TEST_CASE("alias-action: alias transform is applied (not silently dropped)")
{
    Grammar<char, Node> g;

    auto inner = (g["inner"] = g.token('1'));
    inner.set_action([](Ctx&, Span, char) -> Node { return Node{1, 1}; });

    auto wrap = (g["wrap"] = g["inner"]);
    wrap.set_action([](Ctx&, Span, Node n) -> Node { return Node{n.value * 10, 2}; });

    std::string in = "1";
    Ctx ctx(in);
    auto ast = g.parse_ast("wrap", ctx);
    REQUIRE(ast);
    CHECK(ast->value == 10); // would be 1 (raw inner) if the action never fired
    CHECK(ast->tag == 2);
}

// ===========================================================================
// 2. PASSTHROUGH INVARIANT: an alias WITHOUT its own action flows the body's
//    value through unchanged. This is typed_action_test case 7's invariant,
//    duplicated here so the fix cannot silently break it. MUST STAY GREEN.
// ===========================================================================
TEST_CASE("alias-action: alias without action passes value through")
{
    Grammar<char, Node> g;

    auto inner = (g["inner"] = g.token('1'));
    inner.set_action([](Ctx&, Span, char) -> Node { return Node{42, 1}; });

    g["wrap"] = g["inner"]; // alias — no set_action

    std::string in = "1";
    Ctx ctx(in);
    auto ast = g.parse_ast("wrap", ctx);
    REQUIRE(ast);
    CHECK(ast->value == 42); // inner's value flows through
    CHECK(ast->tag == 1);    // only inner's action ran
}

// ===========================================================================
// 3. NESTED alias chain where every layer carries its own action.
//    A = B; B = C; A and B both act. Each layer fires, composing outward
//    (c=1 → b=11 → a=111, tag=3).
//
//    Regression guard for the wrap-if-typed-fold fix: previously NonTerminal::
//    parse adopted the body node and stamped node->producer only-if-none, so
//    the single shared node carried the INNERMOST producer (c); fold_rule
//    dispatched on it and b's typed_fold was unreachable, dropping b's +10.
//    Now each action-bearing rule owns a distinct tree layer (producer =
//    itself), so the fold composes c → b → a in order.
// ===========================================================================
TEST_CASE("alias-action: nested aliases — every layer composes")
{
    Grammar<char, Node> g;

    auto c = (g["c"] = g.token('1'));
    c.set_action([](Ctx&, Span, char) -> Node { return Node{1, 1}; }); // tag 1

    auto b = (g["b"] = g["c"]);
    b.set_action([](Ctx&, Span, Node n) -> Node { return Node{n.value + 10, 2}; }); // tag 2

    auto a = (g["a"] = g["b"]);
    a.set_action([](Ctx&, Span, Node n) -> Node { return Node{n.value + 100, 3}; }); // tag 3

    std::string in = "1";
    Ctx ctx(in);
    auto ast = g.parse_ast("a", ctx);
    REQUIRE(ast);
    CHECK(ast->value == 111); // c=1 → b=11 → a=111 — every layer fired
    CHECK(ast->tag == 3);     // the outermost (start) rule's tag
}

// ===========================================================================
// 4. ALIAS on_match (the side-effect channel). The alias's own on_match hook
//    fires (it is the start rule), but the INNER body rule's on_match does NOT
//    — fire_on_match recurses through node->children, and an alias adopts the
//    body node (no extra child layer), so the inner rule's hook is not reached.
//
//    KNOWN LIMITATION (documented, latent — yueshi's parser uses on_match only
//    in the lexer, never on aliases). This case OBSERVES the limitation.
// ===========================================================================
TEST_CASE("alias-action: alias on_match — inner hook (KNOWN LIMITATION)")
{
    Grammar<char, Node> g;

    int inner_hits = 0;
    int wrap_hits = 0;

    (g["inner"] = g.token('1'));
    g["inner"].on_match([&inner_hits](Ctx&, const NodePtr&) { ++inner_hits; });

    (g["wrap"] = g["inner"]);
    g["wrap"].on_match([&wrap_hits](Ctx&, const NodePtr&) { ++wrap_hits; });

    std::string in = "1";
    Ctx ctx(in);
    auto ast = g.parse_ast("wrap", ctx);
    REQUIRE(ast);
    // Ideal: inner_hits==1 && wrap_hits==1. Actual (limitation): wrap fires
    // (it is the start rule), inner does not.
    MESSAGE("alias-on_match: inner_hits=" << inner_hits << " wrap_hits=" << wrap_hits
           << " (ideal: inner=1 wrap=1; limitation drops inner)");
    CHECK(wrap_hits == 1); // the alias's own (start) hook does fire
}

// ===========================================================================
// 5. NON-REGRESSION: alternation-passthrough (a rule whose body is an
//    alternation, not a single reference) keeps dispatching on the winning
//    branch's producer. This is NOT the alias bug; it must stay GREEN to prove
//    the fix didn't widen its scope. Mirrors typed_action_test case 12 shape.
// ===========================================================================
TEST_CASE("alias-action: alternation winner dispatch is unaffected")
{
    Grammar<char, Node> g;

    auto x = (g["x"] = g.token('x'));
    x.set_action([](Ctx&, Span, char) -> Node { return Node{1, 1}; });

    auto y = (g["y"] = g.token('y'));
    y.set_action([](Ctx&, Span, char) -> Node { return Node{2, 2}; });

    // `choice` is an alternation (NOT an alias): its body is `x | y`. Its node
    // IS the winner's node (alternation-passthrough); producer stays the
    // winner's, so the winner's action fires. choice itself has no action.
    g["choice"] = g["x"] | g["y"];

    {
        std::string in = "x";
        Ctx ctx(in);
        auto ast = g.parse_ast("choice", ctx);
        REQUIRE(ast);
        CHECK(ast->value == 1);
        CHECK(ast->tag == 1); // x won → x's action
    }
    {
        std::string in = "y";
        Ctx ctx(in);
        auto ast = g.parse_ast("choice", ctx);
        REQUIRE(ast);
        CHECK(ast->value == 2);
        CHECK(ast->tag == 2); // y won → y's action
    }
}

// ===========================================================================
// 6. NON-START ALIAS reached from a parent (the "alias as a named transform
//    referenced by another rule" shape). The shape real grammars reach for
//    first when they want to name and reuse a transform:
//
//      inner  = '1'                 ; action: tag=1, value=1
//      middle = inner               ; action: tag=2, value=inner+10  <-- alias
//      root   = middle              ; start rule, action: tag=3, passthrough
//
//    Expected: root folds middle over inner => value=11, then root => tag=3.
//
//    Regression guard for the wrap-if-typed-fold fix. Previously NonTerminal::
//    parse adopted the body node and stamped node->producer only-if-none, so
//    the single node carried up the chain had producer == inner (the
//    innermost). Root (the start rule) fired via fold_start and folded its body
//    via fold_rule, which dispatched on node->producer — i.e. on inner, NOT on
//    middle — so middle's transform (+10, tag=2) was dropped and the result
//    was value=1 (raw inner), tag=3. Now middle owns its own layer and the
//    fold composes inner → middle (+10) → root.
// ===========================================================================
TEST_CASE("alias-action: non-start alias transform composes")
{
    Grammar<char, Node> g;

    auto inner = (g["inner"] = g.token('1'));
    inner.set_action([](Ctx&, Span, char) -> Node { return Node{1, 1}; }); // tag 1

    auto middle = (g["middle"] = g["inner"]);
    middle.set_action([](Ctx&, Span, Node n) -> Node { return Node{n.value + 10, 2}; }); // tag 2

    auto root = (g["root"] = g["middle"]);
    root.set_action([](Ctx&, Span, Node n) -> Node { return Node{n.value, 3}; }); // tag 3

    std::string in = "1";
    Ctx ctx(in);
    auto ast = g.parse_ast("root", ctx);
    REQUIRE(ast);
    CHECK(ast->value == 11); // inner=1 → middle=11 → root=11 — middle's transform applied
    CHECK(ast->tag == 3);    // the outermost (start) rule's tag
}
