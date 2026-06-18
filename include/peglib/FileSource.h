#pragma once
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
namespace peg
{
// Streaming, double-buffered file-backed input source.
//
// Unit convention:
//   - Constructor parameter `buffer_byte_size` is in BYTES.
//   - All internal sizes (`m_buffer_size`, `m_filesize`) are in ITEMS
//     (i.e. number of `value_type` elements), not bytes.
//
// Iterator validity:
//   - Comparing iterators from different FileSource instances is undefined.
//   - Dereferencing the end iterator (or any iterator >= end()) is undefined.
//
// Thread safety: NOT thread-safe. The buffer cache is mutable and mutated
// by const-looking accessors (begin/end/get re-fill buffers on cache miss),
// so concurrent reads — even through a const FileSource — race on the cache.
// PEG parsing is inherently sequential; share one FileSource across parse
// steps in the same thread only.
//
// Large files: buffer positioning uses fseek with a `long` offset, which on
// platforms where `long` is 32-bit (e.g. 32-bit Linux) cannot address files
// larger than ~2 GiB. On LP64 platforms (64-bit Linux/macOS) `long` is
// 64-bit and this is not a concern.
template<typename value_type_>
struct FileSource
{
    // `buffer_byte_size` is the desired buffer size in BYTES. It is rounded
    // up to a multiple of sizeof(value_type) and converted to item count.
    FileSource(size_t buffer_byte_size, const std::string& path) : m_current_buf{0}
    {
        const size_t item_size = sizeof(value_type);
        // Round byte size up to a whole number of items, then convert to item count.
        size_t buffer_items = (buffer_byte_size + item_size - 1) / item_size;
        if (buffer_items == 0)
            buffer_items = 1;
        m_buffer_size = buffer_items;

        // Binary mode is mandatory on Windows: text mode ("r") translates
        // CRLF -> LF on read, so fread() returns fewer bytes than
        // file_size() reports and buffer offsets become inconsistent.
        m_fp = std::fopen(path.c_str(), "rb");
        if (m_fp == nullptr) {
            throw std::runtime_error("FileSource: failed to open '" + path + "'");
        }
        // From here on, ensure m_fp is closed if any subsequent step throws
        // (the destructor won't run on a partially-constructed object).
        try {
            const size_t file_bytes = std::filesystem::file_size(path);
            m_filesize = file_bytes / item_size;

            const size_t n0 = m_bufs[0].read(m_fp, m_buffer_size);
            if (n0 == m_buffer_size) {
                m_bufs[1].read(m_fp, m_buffer_size);
            }
        } catch (...) {
            std::fclose(m_fp);
            m_fp = nullptr;
            throw;
        }
    }

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    FileSource(FileSource&& rhs) noexcept
        : m_filesize{rhs.m_filesize}, m_buffer_size{rhs.m_buffer_size},
          m_current_buf{rhs.m_current_buf}, m_fp{rhs.m_fp}
    {
        m_bufs[0] = std::move(rhs.m_bufs[0]);
        m_bufs[1] = std::move(rhs.m_bufs[1]);
        rhs.m_fp = nullptr;
        rhs.m_filesize = 0;
        rhs.m_buffer_size = 0;
        rhs.m_current_buf = 0;
    }

    FileSource& operator=(FileSource&&) = delete;

    ~FileSource()
    {
        if (m_fp) {
            std::fclose(m_fp);
        }
    }

    using value_type = value_type_;

    struct iterator;

    // Returns the item at position `i`. Calling with `i >= end()` is undefined.
    value_type get(iterator i) const
    {
        assert(i.m_pos < m_filesize && "FileSource::get: position out of range");
        if (m_bufs[m_current_buf].in(i)) {
            return m_bufs[m_current_buf].get_unchecked(i);
        }
        const int other = m_current_buf ^ 1;
        if (m_bufs[other].in(i)) {
            m_current_buf = other;
            return m_bufs[m_current_buf].get_unchecked(i);
        }
        // Cache miss: read the page containing i into m_bufs[0].
        if (!read_to(i.m_pos, 0)) {
            assert(false && "FileSource::get: read_to failed for in-range position");
        }
        m_current_buf = 0;
        return m_bufs[m_current_buf].get_unchecked(i);
    }

    // Release buffer content strictly before `pos`. Called by Context on cut.
    // After release, any iterator with position < pos must not be dereferenced.
    void release_before(iterator pos)
    {
        for (int idx = 0; idx < 2; ++idx) {
            if (m_bufs[idx].m_buf_to <= pos.m_pos) {
                m_bufs[idx].clear();
            }
        }
    }

    struct iterator
    {
        bool operator<(const iterator& rhs) const
        {
            assert(m_freader == rhs.m_freader && "comparing iterators from different FileSource");
            return m_pos < rhs.m_pos;
        }
        bool operator>(const iterator& rhs) const
        {
            assert(m_freader == rhs.m_freader && "comparing iterators from different FileSource");
            return m_pos > rhs.m_pos;
        }
        bool operator<=(const iterator& rhs) const
        {
            assert(m_freader == rhs.m_freader && "comparing iterators from different FileSource");
            return m_pos <= rhs.m_pos;
        }
        bool operator>=(const iterator& rhs) const
        {
            assert(m_freader == rhs.m_freader && "comparing iterators from different FileSource");
            return m_pos >= rhs.m_pos;
        }
        bool operator==(const iterator& rhs) const
        {
            assert(m_freader == rhs.m_freader && "comparing iterators from different FileSource");
            return m_pos == rhs.m_pos;
        }
        bool operator!=(const iterator& rhs) const
        {
            assert(m_freader == rhs.m_freader && "comparing iterators from different FileSource");
            return m_pos != rhs.m_pos;
        }

        iterator& operator++()
        {
            ++m_pos;
            return *this;
        }
        iterator operator++(int)
        {
            iterator tmp = *this;
            ++m_pos;
            return tmp;
        }

        typename FileSource::value_type operator*() const { return m_freader->get(*this); }

        // Byte position (item index for value_type == char).
        std::size_t position() const { return m_pos; }

        friend FileSource;

    protected:
        iterator(const FileSource* f, size_t pos) : m_pos{pos}, m_freader{f} {}
        size_t m_pos; // in item index
        const FileSource* m_freader;
    };

    iterator end() const { return {this, m_filesize}; }
    iterator begin() const { return {this, 0}; }

    // Public factory: construct an iterator at byte position `pos`.
    // Equivalent to begin() + pos, but O(1) (no increment chain).
    iterator seek(size_t pos) const { return {this, pos}; }

    // Value at byte position `pos` (range-checked via assert).
    value_type at(size_t pos) const { return get(seek(pos)); }

    // Total number of items (== byte count for value_type == char).
    size_t size() const { return m_filesize; }

protected:
    struct buffer
    {
        buffer() = default;
        buffer(const buffer&) = delete;
        buffer(buffer&&) = default;
        buffer& operator=(const buffer&) = delete;
        buffer& operator=(buffer&&) = default;

        // Reads up to `n_items` items from `fp` into this buffer.
        // Returns the number of items actually read.
        size_t read(FILE* fp, size_t n_items)
        {
            m_buf.resize(n_items);
            const size_t from_bytes = std::ftell(fp);
            m_buf_from = from_bytes / sizeof(value_type);
            const size_t read_items = std::fread(m_buf.data(), sizeof(value_type), n_items, fp);
            m_buf.resize(read_items);
            m_buf_to = m_buf_from + read_items;
            return read_items;
        }

        void clear()
        {
            m_buf.clear();
            m_buf_from = 0;
            m_buf_to = 0;
        }

        bool in(iterator i) const { return i.m_pos >= m_buf_from && i.m_pos < m_buf_to; }

        value_type get_unchecked(iterator i) const { return m_buf[i.m_pos - m_buf_from]; }

        std::vector<value_type> m_buf;
        size_t m_buf_from = 0;
        size_t m_buf_to = 0;
    };

    size_t m_filesize = 0;    // in items
    size_t m_buffer_size = 0; // in items per buffer
    mutable buffer m_bufs[2];
    mutable int m_current_buf = 0;
    FILE* m_fp = nullptr;

    bool read_to(size_t item_pos, int index) const
    {
        if (item_pos >= m_filesize) {
            return false;
        }
        const size_t buf_pos = item_pos - (item_pos % m_buffer_size);
        if (std::fseek(m_fp, static_cast<long>(buf_pos * sizeof(value_type)), SEEK_SET) != 0) {
            return false;
        }
        return m_bufs[index].read(m_fp, m_buffer_size) > 0;
    }
};
} // namespace peg
