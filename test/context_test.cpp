#include "peglib.h"
#include "peglib/FileSource.h"

#include "doctest.h"

#include <string>

using namespace peg;

// ---------------------------------------------------------------------------
// Context.h unit tests: position tracking, state save/restore, cut lifecycle.
// ---------------------------------------------------------------------------

TEST_CASE("context-basic-position")
{
    std::string input = "hello";
    Context context(input);

    CHECK_FALSE(context.ended());
    CHECK(*context.mark() == 'h');

    context.next();
    CHECK(*context.mark() == 'e');

    for (int i = 0; i < 10; ++i)
        context.next();
    CHECK(context.ended());
}

TEST_CASE("context-state-save-restore")
{
    std::string input = "abcdef";
    Context context(input);

    context.next();
    context.next();
    CHECK(*context.mark() == 'c');

    auto s = context.state();
    context.next();
    context.next();
    CHECK(*context.mark() == 'e');

    context.state(s);
    CHECK(*context.mark() == 'c');
}

TEST_CASE("context-reset-allows-rewind-past-last-cut")
{
    // reset()'s lower bound is intentionally NOT enforced: after a cut,
    // memo/buffer data for earlier positions has been released, but it is
    // still valid to rewind there and re-parse from scratch. This test
    // pins that contract — a future change that clamps reset() to
    // m_last_cut would break legitimate backtracking-after-cut grammars.
    std::string input = "abcdef";
    Context context(input);

    // Walk to 'd' (offset 3), commit a cut there (advances m_last_cut to 3),
    // then rewind to the start — must succeed even though it's before the cut.
    context.next();
    context.next();
    context.next();
    CHECK(*context.mark() == 'd');
    context.init_cut();
    context.cut(true);       // commits; m_last_cut advances on remove_cut
    context.remove_cut();

    // Rewind to the beginning — strictly before m_last_cut. Legal per contract.
    auto begin = context.get_input().begin();
    context.reset(begin);
    CHECK(*context.mark() == 'a');
    // And we can walk forward again from scratch (memo was erased, re-parse OK).
    context.next();
    CHECK(*context.mark() == 'b');
}

TEST_CASE("context-cut-stack-lifecycle")
{
    std::string input = "abc";
    Context context(input);

    // Default: no cut record has been pushed yet, but the parser expressions
    // push one on alternation/repetition. Here we only exercise the public
    // API surface to make sure init/remove are balanced.
    context.init_cut();
    CHECK_FALSE(context.cut());
    context.cut(true);
    CHECK(context.cut());
    context.remove_cut();
}

TEST_CASE("context-get-input-returns-stable-reference")
{
    std::string input = "xyz";
    Context context(input);

    const auto& in1 = context.get_input();
    const auto& in2 = context.get_input();
    CHECK(in1.data() == in2.data());
    CHECK(in1.size() == 3);
}

// ---------------------------------------------------------------------------
// release_before integration: when a cut-committed scope exits, the Context
// should call release_before on FileSource-backed inputs (and silently skip
// the call on span-backed inputs).
// ---------------------------------------------------------------------------

TEST_CASE("context-release-before-skipped-for-span-source")
{
    // Span-backed Context: is_context_releasable_v is false, so
    // remove_cut must not attempt to call release_before.
    std::string input = "abcdef";
    Context context(input);

    context.init_cut();
    context.cut(true);
    context.remove_cut(); // should compile and run without error
}

TEST_CASE("context-release-before-invoked-for-filesource")
{
    // FileSource-backed Context: cut + remove_cut should release the
    // buffer content before the cut position. After release, re-reading
    // from earlier positions must still work (FileSource re-reads from disk).
    //
    // We use a tiny 64-byte buffer so that advancing 100 positions puts
    // us past the first buffer window. release_before(pos=100) then
    // actually evicts buffer 0 (whose m_buf_to <= 64 <= 100).
    const std::string license_path = std::string(PEGLIB_TEST_DATA_DIR) + "/../LICENSE";
    constexpr size_t tiny_buffer = 64;
    auto context = from_file<char>(license_path, tiny_buffer);

    // Capture start character before any eviction.
    auto start = context.mark();
    char start_char = *start;

    // Walk forward past the first buffer window so release_before will
    // actually evict buffer 0 (m_buf_to <= 64 < 100).
    for (int i = 0; i < 100; ++i) {
        context.next();
    }
    CHECK_FALSE(context.ended());

    // Simulate a parser cut at the current position.
    context.init_cut();
    context.cut(true);
    context.remove_cut();

    // After release_before, the buffer for the early part of the file
    // has been cleared. Re-reading from start must still produce the
    // same character (FileSource re-reads from disk on cache miss).
    context.reset(start);
    char c = *context.mark();
    CHECK(c == start_char);
}
