#include "peglib.h"
#include "peglib/FileSource.h"

#include "doctest.h"

#include <fstream>
#include <string>
#include <string_view>

using namespace peg;

// ---------------------------------------------------------------------------
// SourceMap.h unit tests: byte offset <-> (line, col) mapping, line content.
// ---------------------------------------------------------------------------

TEST_CASE("sourcemap-empty-source")
{
    SourceMap map{std::string_view{""}};
    CHECK(map.num_lines() == 1);
    CHECK(map.locate(0).line == 1);
    CHECK(map.locate(0).column == 1);
    CHECK(map.locate(0).offset == 0);
}

TEST_CASE("sourcemap-single-line")
{
    SourceMap map{std::string_view{"hello"}};
    CHECK(map.num_lines() == 1);

    auto loc = map.locate(0);
    CHECK(loc.line == 1);
    CHECK(loc.column == 1);

    loc = map.locate(4);
    CHECK(loc.line == 1);
    CHECK(loc.column == 5);

    CHECK(map.line_view(1) == "hello");
    CHECK(map.line_content(1) == "hello");
}

TEST_CASE("sourcemap-multi-line-unix")
{
    std::string_view src = "abc\ndef\nghi";
    SourceMap map{src};
    CHECK(map.num_lines() == 3);

    // Line 1: "abc" at offsets 0-2, \n at 3
    CHECK(map.locate(0).line == 1);
    CHECK(map.locate(0).column == 1);
    CHECK(map.locate(2).column == 3);

    // Line 2: "def" at offsets 4-6
    auto loc4 = map.locate(4);
    CHECK(loc4.line == 2);
    CHECK(loc4.column == 1);

    // Line 3: "ghi" at offsets 8-10
    auto loc8 = map.locate(8);
    CHECK(loc8.line == 3);
    CHECK(loc8.column == 1);
    CHECK(map.locate(10).column == 3);

    CHECK(map.line_view(1) == "abc");
    CHECK(map.line_view(2) == "def");
    CHECK(map.line_view(3) == "ghi");
}

TEST_CASE("sourcemap-crlf-line-endings")
{
    std::string_view src = "ab\r\ncd\r\nef";
    SourceMap map{src};
    CHECK(map.num_lines() == 3);

    // Line 1: "ab" then \r\n at offsets 2-3
    CHECK(map.locate(0).line == 1);
    CHECK(map.locate(1).column == 2);

    // \r at offset 2 is still on line 1 (column 3)
    CHECK(map.locate(2).line == 1);
    CHECK(map.locate(2).column == 3);

    // Line 2 starts after \n at offset 3, so offset 4 = line 2, col 1
    auto loc4 = map.locate(4);
    CHECK(loc4.line == 2);
    CHECK(loc4.column == 1);

    // line_view must strip both \r and \n
    CHECK(map.line_view(1) == "ab");
    CHECK(map.line_view(2) == "cd");
    CHECK(map.line_view(3) == "ef");

    CHECK(map.line_content(1) == "ab");
    CHECK(map.line_content(2) == "cd");
}

TEST_CASE("sourcemap-trailing-newline")
{
    std::string_view src = "abc\n";
    SourceMap map{src};
    CHECK(map.num_lines() == 2); // line 1: "abc", line 2: "" (empty trailing line)

    CHECK(map.line_view(1) == "abc");
    CHECK(map.line_view(2) == "");

    // Offset 3 is the \n, still on line 1
    CHECK(map.locate(3).line == 1);
    // Offset 4 is past end, clamps to last valid location
    auto eof = map.locate(4);
    CHECK(eof.line == 2);
    CHECK(eof.column == 1);
}

TEST_CASE("sourcemap-offset-roundtrip")
{
    std::string_view src = "line one\nline two\nline three";
    SourceMap map{src};

    for (std::size_t off = 0; off < src.size(); ++off) {
        auto loc = map.locate(off);
        std::size_t back = map.offset_of(loc.line, loc.column);
        CHECK(back == off);
    }
}

TEST_CASE("sourcemap-offset-of-out-of-range")
{
    SourceMap map{std::string_view{"hi\nthere"}};
    CHECK(map.offset_of(0, 1) == SourceMap::npos);  // line 0 doesn't exist
    CHECK(map.offset_of(99, 1) == SourceMap::npos); // line 99 doesn't exist
    // Column past end of line is allowed (returns offset, just past content)
    CHECK(map.offset_of(1, 100) == 99); // clamped by arithmetic
}

TEST_CASE("sourcemap-filesource-equivalent-to-contiguous")
{
    const std::string license_path = std::string(PEGLIB_TEST_DATA_DIR) + "/../LICENSE";

    // Read entire file into string for contiguous map.
    std::ifstream fs(license_path, std::ios::binary);
    std::string full{std::istreambuf_iterator<char>(fs), std::istreambuf_iterator<char>()};

    SourceMap contiguous{std::string_view{full}};

    // SourceMap needs the raw FileSource for its line-prescan. Construct it
    // directly rather than extracting it from a Context (Context type-erases
    // the source behind InputSourceBase now).
    FileSource<char> fsource{4096, license_path};
    SourceMap filed{fsource};

    CHECK(filed.num_lines() == contiguous.num_lines());

    // Check a few offsets match
    for (std::size_t off :
         {std::size_t{0}, std::size_t{10}, std::size_t{100}, full.size() / 2, full.size() - 1}) {
        auto lc = contiguous.locate(off);
        auto lf = filed.locate(off);
        CHECK(lc.line == lf.line);
        CHECK(lc.column == lf.column);
    }

    // line_content should match for all lines
    for (std::size_t line = 1; line <= contiguous.num_lines(); ++line) {
        CHECK(filed.line_content(line) == contiguous.line_content(line));
    }
}
