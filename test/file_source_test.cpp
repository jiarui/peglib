#include "peglib.h"
#include "peglib/FileSource.h"

#include "doctest.h"

#include <fstream>
#include <string>
#include <string_view>

using namespace peg;

namespace
{
std::string read_all(const std::string& path)
{
    // Binary mode: do not translate CRLF on Windows. FileSource opens with
    // "rb" for byte-accurate seeking, so the reference reader must see the
    // same raw bytes or comparisons desync at every newline.
    std::ifstream fs(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(fs), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("filesource-reads-file-consistently-with-ifstream")
{
    const std::string license_path = std::string(PEGLIB_TEST_DATA_DIR) + "/../LICENSE";
    auto context = from_file<char>(license_path);
    const std::string expected = read_all(license_path);

    auto expected_it = expected.begin();
    while (!context.ended()) {
        CHECK(context.current() == *expected_it);
        ++expected_it;
        context.next();
    }
    CHECK(expected_it == expected.end());
}

TEST_CASE("filesource-begin-and-end-iterate-full-file")
{
    const std::string license_path = std::string(PEGLIB_TEST_DATA_DIR) + "/../LICENSE";
    auto fs = FileSource<char, 4096>(license_path);

    size_t count = 0;
    for (auto it = fs.begin(); it != fs.end(); ++it) {
        ++count;
    }
    CHECK(count > 0);
    CHECK(count == std::filesystem::file_size(license_path));
}

TEST_CASE("filesource-iterator-comparisons-consistent")
{
    const std::string license_path = std::string(PEGLIB_TEST_DATA_DIR) + "/../LICENSE";
    auto fs = FileSource<char, 4096>(license_path);

    auto a = fs.begin();
    auto b = fs.begin();
    auto c = fs.end();

    CHECK(a == b);
    CHECK_FALSE(a != b);
    CHECK(a < c);
    CHECK(a <= c);
    CHECK(c > a);
    CHECK(c >= a);
    CHECK_FALSE(a >= c);
}

// Force cache misses + rewinds with a small buffer. The PEG engine routinely
// rewinds (backtracking, lookahead, NotExpr/AndExpr), so the FileSource must
// re-fetch pages that have already been evicted or that predate the current
// buffer window.
TEST_CASE("filesource-tiny-buffer-exercises-cache-miss-and-rewind")
{
    const std::string license_path = std::string(PEGLIB_TEST_DATA_DIR) + "/../LICENSE";
    const std::string expected = read_all(license_path);
    REQUIRE(expected.size() > 64);

    // 8-byte buffer: the 11 KB LICENSE file spans many pages.
    auto fs = FileSource<char, 8>(license_path);
    const auto end = fs.end();

    // Walk forward to the middle of the file, reading every byte.
    auto it = fs.begin();
    for (size_t i = 0; i < expected.size() / 2; ++i) {
        REQUIRE(it != end);
        CHECK(*it == expected[i]);
        ++it;
    }

    // Rewind to byte 0 — this page was evicted long ago (cache miss path).
    auto back = fs.begin();
    CHECK(*back == expected[0]);

    // Forward jump well past the current window (forces read_to into buf[0]).
    FileSource<char, 8>::iterator jump = fs.begin();
    // Manual position increment via dereference drives the page loads.
    const size_t jump_target = expected.size() - 4;
    for (size_t i = 0; i < jump_target; ++i) {
        REQUIRE(jump != end);
        ++jump;
    }
    CHECK(*jump == expected[jump_target]);

    // Walk the remaining bytes to end.
    for (size_t i = jump_target; i < expected.size(); ++i) {
        REQUIRE(jump != end);
        CHECK(*jump == expected[i]);
        ++jump;
    }
    CHECK(jump == end);
}

// Re-reading a position after a rewind must return the same byte (i.e. the
// page re-read after eviction is consistent). This is the exact access
// pattern of AndExpr / NotExpr / SequenceExpr rollback.
TEST_CASE("filesource-reread-after-eviction-is-stable")
{
    const std::string license_path = std::string(PEGLIB_TEST_DATA_DIR) + "/../LICENSE";
    const std::string expected = read_all(license_path);

    auto fs = FileSource<char, 16>(license_path);

    // Read byte at offset 0, then walk far enough to evict page 0, then
    // re-read offset 0 — values must match.
    auto first = fs.begin();
    const char v0a = *first;
    CHECK(v0a == expected[0]);

    auto it = fs.begin();
    for (size_t i = 0; i < 64 && it != fs.end(); ++i)
        ++it;

    auto first_again = fs.begin();
    const char v0b = *first_again;
    CHECK(v0b == expected[0]);
    CHECK(v0a == v0b);
}
