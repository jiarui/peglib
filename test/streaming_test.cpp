// ---------------------------------------------------------------------------
// Streaming-parse + cut-driven eviction end-to-end tests.
//
// Coverage gap flagged in negative_path_test.cpp: no test drives a real parser
// with cut over a FileSource-backed Context (the existing FileSource tests only
// walk iterators; context_test drives release_before by hand). These tests
// close that gap and verify the "eviction transparency" contract: a parse over
// a paged FileSource must produce a tree (and typed AST) identical to the same
// parse over a contiguous SpanSource, even when cut-driven release_before
// evicts the pages holding earlier offsets.
//
// Strategy: every FileSource case is paired with a SpanSource reference parse
// of the same input, and the two are compared for exact equality. Child-
// filtering rules (terminal = void, token = kept) are source-independent, so
// the comparison needs no a-priori knowledge of the tree shape. Any corruption
// from eviction (stale memo, offset desync, dangling read) surfaces as a
// mismatch against the reference.
//
// cut semantics reminder (see Combinators.h / Context.h):
//   - cut commits to the nearest enclosing Alternation/Repetition scope only;
//     a cut with no such scope (e.g. directly in a Sequence at the top of a
//     rule body) is a no-op (Context.h:316-318).
//   - eviction fires in remove_cut (scope exit), at the position where cut was
//     set (m_cut.top().pos), NOT at the position where the parse eventually
//     finishes. release_before(pos) drops pages whose m_buf_to <= pos
//     (FileSource.h:103). To actually evict the first page, the cut must be
//     reached AFTER consuming at least PageSize elements.
//   - cut does NOT mean "syntactically complete" or "never rewindable"; it is
//     a choice-commitment, weaker than what a codegen commit point would need.
// ---------------------------------------------------------------------------

#include "peglib.h"
#include "peglib/FileSource.h"

#include "doctest.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

using namespace peg;

namespace
{
// RAII temp file: writes content on construction, removes on destruction.
// Mirrors the pattern in negative_path_test.cpp so leftover files are cleaned
// even when a REQUIRE aborts the case.
struct TmpFile
{
    std::string path;
    explicit TmpFile(std::string_view name, std::string_view content)
        : path{std::string(PEGLIB_TEST_DATA_DIR) + "/" + std::string{name}}
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    ~TmpFile()
    {
        if (!path.empty()) {
            std::remove(path.c_str());
        }
    }
    TmpFile(const TmpFile&) = delete;
    TmpFile& operator=(const TmpFile&) = delete;
};

// Recursive structural equality of two parse trees. Compares name, offsets,
// alt-branch index, and (recursively) children. Producer pointers are identical
// across two parses of the same Grammar, so they are not compared.
template<typename NodePtr>
bool trees_equal(const NodePtr& a, const NodePtr& b)
{
    if (!a && !b)
        return true;
    if (!a || !b)
        return false;
    if (a->name != b->name)
        return false;
    if (a->start_offset != b->start_offset)
        return false;
    if (a->end_offset != b->end_offset)
        return false;
    if (a->alt_winner != b->alt_winner)
        return false;
    if (a->children.size() != b->children.size())
        return false;
    for (std::size_t i = 0; i < a->children.size(); ++i) {
        if (!trees_equal(a->children[i], b->children[i]))
            return false;
    }
    return true;
}

// NodeType for the typed-action cases.
struct Node
{
    char ch = 0;
};
using Ctx = Context<char, Node>;
using NodePtr = Ctx::ParseTreeNodePtr;

// Shared cut-driven grammar for the structural cases (1, 2, 5). Two
// alternatives, each consuming a discriminator token, a run of padding (which
// pushes the cut position past the first page so release_before actually
// evicts page 0), a cut, then a tail.
//
// Input shape for branch 0:  'a' + pppp... + xxxx...
//   token('a')@0  +'p'@1..N  cut@N+1  +'x'@N+1..
// On a PageSize-P source, release_before(N+1) evicts page [0,P) when N+1 >= P.
struct CutGrammar
{
    Grammar<> g;
    CutGrammar()
    {
        g["line"] = (g.token('a') >> +g.terminal('p') >> g.cut() >> +g.terminal('x')) |
                    (g.token('b') >> +g.terminal('q') >> g.cut() >> +g.terminal('y'));
        g.set_start("line");
    }
};
} // namespace

// ---------------------------------------------------------------------------
// Case 1: end-to-end cut-driven parse over FileSource matches the SpanSource
// reference tree. PageSize=4, input spans 3 pages; cut at pos 5 evicts page 0.
// ---------------------------------------------------------------------------
TEST_CASE("streaming: cut-driven parse over FileSource matches span reference")
{
    CutGrammar cg;
    const std::string input_a = "appppxxxxx"; // 10 chars, cut@5
    const std::string input_b = "bqqqqyyyyy";

    for (const auto& input : {input_a, input_b}) {
        // Span reference.
        Context ref_ctx(input);
        auto ref = cg.g.parse_tree("line", ref_ctx);
        REQUIRE(ref);
        CHECK(ref_ctx.ended());

        // FileSource with PageSize=4: cut@5 -> release_before(5) evicts [0,4).
        TmpFile tmp{"streaming_case1.tmp", input};
        auto fctx = from_file<char, 4>(tmp.path);
        auto tree = cg.g.parse_tree("line", fctx);
        REQUIRE(tree);
        CHECK(fctx.ended());
        CHECK(trees_equal(ref, tree));
    }
}

// ---------------------------------------------------------------------------
// Case 2: small-page stress. PageSize=8 over an 80-char input amplifies the
// disk-re-read path; cut at pos 40 evicts pages [0,8), [8,16), ... [32,40).
// Tree must still equal the span reference and input must be fully consumed.
// ---------------------------------------------------------------------------
TEST_CASE("streaming: small-page stress triggers eviction, still equivalent")
{
    CutGrammar cg;

    const std::string padding(39, 'p');
    const std::string tail(40, 'x');
    const std::string input = "a" + padding + tail; // 80 chars, cut@40

    Context ref_ctx(input);
    auto ref = cg.g.parse_tree("line", ref_ctx);
    REQUIRE(ref);
    CHECK(ref_ctx.ended());

    TmpFile tmp{"streaming_case2.tmp", input};
    auto fctx = from_file<char, 8>(tmp.path);
    auto tree = cg.g.parse_tree("line", fctx);
    REQUIRE(tree);
    CHECK(fctx.ended());
    CHECK(fctx.mark() == input.size());
    CHECK(trees_equal(ref, tree));
}

// ---------------------------------------------------------------------------
// Case 3: TokenExpr capture read after eviction.
// The typed fold for TokenExpr calls ctx.at(node->start_offset) (ResultType.h:344)
// to recover the matched element. The root node starts at pos 0, whose page has
// been evicted by the time the fold runs (eviction happened during parse, in
// remove_cut at scope exit; the fold runs after parse in parse_ast). ctx.at(0)
// must re-read from disk and return the original byte.
// ---------------------------------------------------------------------------
TEST_CASE("streaming: TokenExpr capture read after eviction")
{
    Grammar<char, Node> g;
    auto h =
        (g["g"] = (g.token('a') >> +g.terminal('p') >> g.cut() >> +g.terminal('x')) | g.token('z'));
    // token('a') is the only non-void child of the winning sequence branch, so
    // the action receives the captured char. Branch 1 (token('z')) shares the
    // same result type, keeping the alternation well-formed.
    h.set_action([](Ctx& ctx, Span /*sp*/, char ch) -> Node {
        // Touch the evicted offset too, to force the re-read path explicitly.
        (void)ctx.at(0);
        return Node{.ch = ch};
    });

    const std::string input = "appppxxxxx"; // cut@5, PageSize=4 -> [0,4) evicted

    // Reference (SpanSource): Ctx is Context<char, Node>, matching the Grammar.
    Ctx ref_ctx(input);
    auto ref = g.parse_ast("g", ref_ctx);
    REQUIRE(ref);
    CHECK(ref->ch == 'a');

    // FileSource-backed Ctx: must be Context<char, Node> too — from_file returns
    // Context<char, monostate>, so build the Context directly from the FileSource.
    TmpFile tmp{"streaming_case3.tmp", input};
    FileSource<char, 4> fs(tmp.path);
    Ctx fctx(std::move(fs));
    auto ast = g.parse_ast("g", fctx);
    REQUIRE(ast);
    CHECK(fctx.ended());
    CHECK(ast->ch == 'a');
    CHECK(ast->ch == ref->ch);
}

// ---------------------------------------------------------------------------
// Case 4: on_match hook reads via ctx.at() after eviction.
// fire_on_match walks the committed tree pre-order and fires each producer's
// hook (ResultType.h:557). The root node's start_offset is 0 (evicted page);
// the hook's ctx.at(0) must re-read and return the original byte.
// ---------------------------------------------------------------------------
TEST_CASE("streaming: on_match hook reads via ctx.at() after eviction")
{
    Grammar<char, Node> g;
    g["g"] = (g.token('a') >> +g.terminal('p') >> g.cut() >> +g.terminal('x')) | g.token('z');

    char captured = 0;
    g["g"].on_match([&captured](Ctx& ctx, const NodePtr& n) {
        captured = ctx.at(n->start_offset); // root starts at pos 0 -> evicted
    });

    const std::string input = "appppxxxxx"; // cut@5, PageSize=4 -> [0,4) evicted

    // Span reference: hook fires, reads ctx.at(0) from the contiguous buffer.
    Ctx ref_ctx(input);
    auto ref = g.parse_ast("g", ref_ctx);
    REQUIRE(ref);
    CHECK(ref_ctx.ended());
    const char ref_captured = captured;
    CHECK(ref_captured == 'a');

    // FileSource pass: same hook, but ctx.at(0) now re-reads from disk.
    captured = 0;
    TmpFile tmp{"streaming_case4.tmp", input};
    FileSource<char, 4> fs(tmp.path);
    Ctx fctx(std::move(fs));
    auto ast = g.parse_ast("g", fctx);
    REQUIRE(ast);
    CHECK(fctx.ended());
    CHECK(captured == 'a');
    CHECK(captured == ref_captured);
}

// ---------------------------------------------------------------------------
// Case 5: negative control. Same grammar shape with NO cut -> no init_cut /
// remove_cut -> no release_before calls. Tree must still match the span
// reference. Isolates "FileSource path is broken independently of cut" from
// "cut eviction is broken": if cases 1-4 fail but this passes, the bug lives
// in the eviction path.
// ---------------------------------------------------------------------------
TEST_CASE("streaming: no-cut grammar never evicts; equivalent to span")
{
    Grammar<> g;
    g["line"] = (g.token('a') >> +g.terminal('p') >> +g.terminal('x')) |
                (g.token('b') >> +g.terminal('q') >> +g.terminal('y'));
    g.set_start("line");

    const std::string input = "appppxxxxx"; // 10 chars, PageSize=4

    Context ref_ctx(input);
    auto ref = g.parse_tree("line", ref_ctx);
    REQUIRE(ref);
    CHECK(ref_ctx.ended());

    TmpFile tmp{"streaming_case5.tmp", input};
    auto fctx = from_file<char, 4>(tmp.path);
    auto tree = g.parse_tree("line", fctx);
    REQUIRE(tree);
    CHECK(fctx.ended());
    CHECK(trees_equal(ref, tree));
}

// ---------------------------------------------------------------------------
// Case 6: memo table pruned across cut on FileSource.
// The first alternative matches "word" (greedily), commits via cut, then
// matches the terminator. At remove_cut, memo entries at positions < cut are
// erased (Context.h:335-337). The first alternative succeeds outright, so the
// pruned memo is not re-read here; the observable guarantee is that the
// memo+eviction interaction on a FileSource does not corrupt the result. This
// is the FileSource analogue of negative_path_test's span-only case.
// ---------------------------------------------------------------------------
TEST_CASE("streaming: memo pruned across cut on FileSource; re-parse equivalent")
{
    Grammar<> g;
    g["word"] = +g.terminal('a');
    g["g"] = (g["word"] >> g.cut() >> g.terminal(';')) | (g["word"] >> g.terminal(','));
    g.set_start("g");

    const std::string input = "aaaaaaaa;"; // 8 'a's + ';', cut@8

    Context ref_ctx(input);
    auto ref = g.parse_tree("g", ref_ctx);
    REQUIRE(ref);
    CHECK(ref_ctx.ended());

    TmpFile tmp{"streaming_case6.tmp", input};
    auto fctx = from_file<char, 4>(tmp.path);
    auto tree = g.parse_tree("g", fctx);
    REQUIRE(tree);
    CHECK(fctx.ended());
    CHECK(trees_equal(ref, tree));
}
