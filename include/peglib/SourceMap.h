#pragma once
#include "peglib/FileSource.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace peg
{

// ---------------------------------------------------------------------------
// Source location / range types
// ---------------------------------------------------------------------------

struct SourceLocation
{
    std::size_t offset = 0; // byte offset from start of file, 0-based
    std::size_t line = 1;   // 1-based
    std::size_t column = 1; // 1-based, counts bytes (not codepoints)
};

struct SourceRange
{
    SourceLocation start;
    SourceLocation end;
};

// ---------------------------------------------------------------------------
// SourceMap: byte offset <-> (line, column) mapping
//
// Construction is O(n): a single pass scans the source to record the byte
// offset at the start of each line. After construction, locate() and
// offset_of() are O(log L) where L is the number of lines (binary search).
//
// Two constructors are provided:
//   - string_view / span-backed: O(1) line_view via slicing the view.
//   - FileSource-backed: line_content re-reads the line from disk on demand.
//
// Line-ending convention: \n terminates a line. A preceding \r (i.e. \r\n)
// is treated as part of the terminator and is NOT included in the line
// content returned by line_view / line_content. A lone \r (old Mac) is
// NOT treated as a line ending — only \n starts a new line.
//
// Edge cases:
//   - Empty source: num_lines() == 1 (one empty line).
//   - Source ending in \n: the trailing empty line counts (num_lines
//     includes it), consistent with POSIX wc -l semantics for unterminated
//     final lines.
// ---------------------------------------------------------------------------

class SourceMap
{
public:
    // Construct from a contiguous source (string_view, std::span<const char>, etc.)
    // Prescan is O(n). The SourceMap does NOT take ownership; caller must keep
    // `source` alive while line_view() is used.
    explicit SourceMap(std::string_view source) : m_contiguous{source}, m_file_source{nullptr}
    {
        prescan();
    }

    // Construct from a FileSource. Reads through the FileSource once to
    // populate line_starts; the FileSource must outlive this SourceMap.
    explicit SourceMap(const FileSource<char>& source) : m_file_source{&source} { prescan_file(); }

    SourceMap(const SourceMap&) = default;
    SourceMap(SourceMap&&) noexcept = default;
    SourceMap& operator=(const SourceMap&) = default;
    SourceMap& operator=(SourceMap&&) noexcept = default;

    // Byte offset -> SourceLocation. Offsets beyond EOF clamp to the last
    // valid location (end of file).
    [[nodiscard]] SourceLocation locate(std::size_t offset) const noexcept
    {
        // Binary-search for the line containing `offset`.
        // m_line_starts[i] is the byte offset of the start of line (i+1).
        auto it = std::upper_bound(m_line_starts.begin(), m_line_starts.end(), offset);
        std::size_t line_idx = static_cast<std::size_t>(it - m_line_starts.begin()) - 1;
        std::size_t line_start = m_line_starts[line_idx];
        std::size_t col = offset - line_start + 1; // 1-based
        std::size_t line = line_idx + 1;           // 1-based
        return SourceLocation{.offset = offset, .line = line, .column = col};
    }

    // (line, column) -> byte offset. Returns npos if line is out of range.
    // `line` is 1-based, `column` is 1-based.
    [[nodiscard]] std::size_t offset_of(std::size_t line, std::size_t column) const noexcept
    {
        if (line == 0 || line > m_line_starts.size()) {
            return npos;
        }
        std::size_t line_start = m_line_starts[line - 1];
        return line_start + (column - 1);
    }

    // 1-based line number -> line content WITHOUT the line terminator.
    // Only valid for contiguous (string_view-backed) SourceMaps.
    [[nodiscard]] std::string_view line_view(std::size_t line) const
    {
        assert(m_file_source == nullptr && "line_view requires a contiguous SourceMap");
        if (line == 0 || line > m_line_starts.size()) {
            return {};
        }
        std::size_t start = m_line_starts[line - 1];
        // End is the start of the next line (or end of source for last line),
        // minus any trailing \r\n or \n.
        std::size_t end;
        if (line < m_line_starts.size()) {
            end = m_line_starts[line] - 1; // skip the \n
            if (end > start && m_contiguous[end - 1] == '\r') {
                --end; // skip the \r in \r\n
            }
        } else {
            end = m_contiguous.size();
            // Trim trailing \r if present (lone \r at EOF without \n — unusual but handled)
            if (end > start && m_contiguous[end - 1] == '\r') {
                --end;
            }
        }
        return m_contiguous.substr(start, end - start);
    }

    // 1-based line number -> line content WITHOUT the line terminator.
    // Works for both contiguous and FileSource-backed maps. For FileSource,
    // this re-reads the line from disk (O(line_length)).
    [[nodiscard]] std::string line_content(std::size_t line) const
    {
        if (line == 0 || line > m_line_starts.size()) {
            return {};
        }
        if (m_file_source == nullptr) {
            return std::string{line_view(line)};
        }
        // FileSource path: compute byte range, re-read from disk.
        std::size_t start = m_line_starts[line - 1];
        std::size_t end;
        std::size_t source_size = m_file_source->size();
        if (line < m_line_starts.size()) {
            end = m_line_starts[line] - 1; // skip \n
            if (end > start) {
                // Peek at the byte before \n to check for \r.
                if (m_file_source->at(end - 1) == '\r') {
                    --end;
                }
            }
        } else {
            end = source_size;
            if (end > start) {
                if (m_file_source->at(end - 1) == '\r') {
                    --end;
                }
            }
        }
        std::string result;
        result.reserve(end - start);
        for (std::size_t i = start; i < end; ++i) {
            result.push_back(m_file_source->at(i));
        }
        return result;
    }

    [[nodiscard]] std::size_t num_lines() const noexcept { return m_line_starts.size(); }

    [[nodiscard]] std::string_view source_view() const noexcept { return m_contiguous; }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    void prescan()
    {
        m_line_starts.clear();
        m_line_starts.push_back(0); // line 1 starts at offset 0
        for (std::size_t i = 0; i < m_contiguous.size(); ++i) {
            if (m_contiguous[i] == '\n') {
                m_line_starts.push_back(i + 1);
            }
        }
    }

    void prescan_file()
    {
        m_line_starts.clear();
        m_line_starts.push_back(0);
        std::size_t end_pos = m_file_source->size();
        for (std::size_t i = 0; i < end_pos; ++i) {
            char c = m_file_source->at(i);
            if (c == '\n') {
                m_line_starts.push_back(i + 1);
            }
        }
    }

    std::vector<std::size_t> m_line_starts;
    std::string_view m_contiguous;           // empty for FileSource-backed maps
    const FileSource<char>* m_file_source{}; // nullptr for contiguous maps
};

} // namespace peg
