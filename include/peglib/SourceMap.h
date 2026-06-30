// SourceMap: byte offset <-> (line, column) mapping.
// Construction is O(n) prescan; locate() / offset_of() are O(log L).
// Line endings: \n terminates a line; a preceding \r is part of the
// terminator. Lone \r is not a line ending.
#pragma once
#include "peglib/FileSource.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace peg
{

struct SourceLocation
{
    std::size_t offset = 0;
    std::size_t line = 1;   // 1-based
    std::size_t column = 1; // 1-based, counts bytes
};

struct SourceRange
{
    SourceLocation start;
    SourceLocation end;
};

class SourceMap
{
public:
    // Non-owning: caller must keep `source` alive while line_view() is used.
    explicit SourceMap(std::string_view source) : m_contiguous{source} { prescan(); }

    // The FileSource must outlive this SourceMap. PageSize is deduced; the
    // source is held type-erased so SourceMap stays non-templated.
    template<std::size_t PageSize>
    explicit SourceMap(const FileSource<char, PageSize>& source)
        : m_file_source{&source}, m_file_size{&SourceMap::impl_size<FileSource<char, PageSize>>},
          m_file_at{&SourceMap::impl_at<FileSource<char, PageSize>>}
    {
        prescan_file();
    }

    SourceMap(const SourceMap&) = default;
    SourceMap(SourceMap&&) noexcept = default;
    SourceMap& operator=(const SourceMap&) = default;
    SourceMap& operator=(SourceMap&&) noexcept = default;

    // Offsets past EOF stay on the last line; column reflects the raw
    // (unclamped) offset.
    [[nodiscard]] SourceLocation locate(std::size_t offset) const noexcept
    {
        auto it = std::upper_bound(m_line_starts.begin(), m_line_starts.end(), offset);
        std::size_t line_idx = static_cast<std::size_t>(it - m_line_starts.begin()) - 1;
        std::size_t line_start = m_line_starts[line_idx];
        std::size_t col = offset - line_start + 1;
        std::size_t line = line_idx + 1;
        return SourceLocation{.offset = offset, .line = line, .column = col};
    }

    // Returns npos if line is out of range. Both args are 1-based.
    [[nodiscard]] std::size_t offset_of(std::size_t line, std::size_t column) const noexcept
    {
        if (line == 0 || line > m_line_starts.size()) {
            return npos;
        }
        std::size_t line_start = m_line_starts[line - 1];
        return line_start + (column - 1);
    }

    // Contiguous-source only.
    [[nodiscard]] std::string_view line_view(std::size_t line) const
    {
        assert(m_file_source == nullptr && "line_view requires a contiguous SourceMap");
        if (line == 0 || line > m_line_starts.size()) {
            return {};
        }
        std::size_t start = m_line_starts[line - 1];
        std::size_t end;
        if (line < m_line_starts.size()) {
            end = m_line_starts[line] - 1; // skip \n
            if (end > start && m_contiguous[end - 1] == '\r') {
                --end; // skip \r in \r\n
            }
        } else {
            end = m_contiguous.size();
            if (end > start && m_contiguous[end - 1] == '\r') {
                --end;
            }
        }
        return m_contiguous.substr(start, end - start);
    }

    // Works for both contiguous and FileSource-backed maps (latter re-reads
    // the line from disk).
    [[nodiscard]] std::string line_content(std::size_t line) const
    {
        if (line == 0 || line > m_line_starts.size()) {
            return {};
        }
        if (m_file_source == nullptr) {
            return std::string{line_view(line)};
        }
        std::size_t start = m_line_starts[line - 1];
        std::size_t end;
        std::size_t source_size = m_file_size(m_file_source);
        if (line < m_line_starts.size()) {
            end = m_line_starts[line] - 1;
            if (end > start && m_file_at(m_file_source, end - 1) == '\r') {
                --end;
            }
        } else {
            end = source_size;
            if (end > start && m_file_at(m_file_source, end - 1) == '\r') {
                --end;
            }
        }
        std::string result;
        result.reserve(end - start);
        for (std::size_t i = start; i < end; ++i) {
            result.push_back(m_file_at(m_file_source, i));
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
        m_line_starts.push_back(0);
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
        std::size_t end_pos = m_file_size(m_file_source);
        for (std::size_t i = 0; i < end_pos; ++i) {
            char c = m_file_at(m_file_source, i);
            if (c == '\n') {
                m_line_starts.push_back(i + 1);
            }
        }
    }

    // Type-erased FileSource accessors (PageSize varies; SourceMap is non-templated).
    template<typename Fs>
    static std::size_t impl_size(const void* p)
    {
        return static_cast<const Fs*>(p)->size();
    }
    template<typename Fs>
    static char impl_at(const void* p, std::size_t i)
    {
        return static_cast<const Fs*>(p)->at(i);
    }

    using size_fn_t = std::size_t (*)(const void*);
    using at_fn_t = char (*)(const void*, std::size_t);

    std::vector<std::size_t> m_line_starts;
    std::string_view m_contiguous;
    const void* m_file_source{};
    size_fn_t m_file_size{};
    at_fn_t m_file_at{};
};

} // namespace peg
