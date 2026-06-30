// Streaming, double-buffered file-backed input source.
//
// PageSize is a compile-time constant: each of the two buffers is a
// fixed-size std::array<value_type, PageSize> (no per-page heap allocation),
// making FileSource suitable for embedded/freestanding use.
//
// All internal sizes are in ITEMS (number of value_type elements), not bytes.
//
// Thread safety: NOT thread-safe — buffer cache is mutable and mutated by
// const-looking accessors (re-fill on cache miss); concurrent reads race.
//
// Large files: fseek uses a `long` offset, which on platforms where `long`
// is 32-bit cannot address files larger than ~2 GiB. Not a concern on LP64.
#pragma once
#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
namespace peg
{

template<typename value_type_, std::size_t PageSize>
struct FileSource
{
    static_assert(PageSize > 0, "FileSource PageSize must be positive");

    // Binary mode is mandatory on Windows (text mode translates CRLF -> LF,
    // breaking buffer offset accounting).
    explicit FileSource(const std::string& path) : m_current_buf{0}
    {
        m_fp = std::fopen(path.c_str(), "rb");
        if (m_fp == nullptr) {
            throw std::runtime_error("FileSource: failed to open '" + path + "'");
        }
        try {
            const size_t file_bytes = std::filesystem::file_size(path);
            m_filesize = file_bytes / sizeof(value_type);

            const size_t n0 = m_bufs[0].read(m_fp, PageSize);
            if (n0 == PageSize) {
                m_bufs[1].read(m_fp, PageSize);
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
        : m_filesize{rhs.m_filesize}, m_current_buf{rhs.m_current_buf}, m_fp{rhs.m_fp}
    {
        m_bufs[0] = std::move(rhs.m_bufs[0]);
        m_bufs[1] = std::move(rhs.m_bufs[1]);
        rhs.m_fp = nullptr;
        rhs.m_filesize = 0;
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

    // Item at position `i`. Calling with `i >= end()` is undefined.
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
        if (!read_to(i.m_pos, 0)) {
            assert(false && "FileSource::get: read_to failed for in-range position");
        }
        m_current_buf = 0;
        return m_bufs[m_current_buf].get_unchecked(i);
    }

    // Release buffer content strictly before `pos`. After release, any
    // iterator with position < pos must not be dereferenced.
    void release_before(iterator pos) { release_before(pos.m_pos); }

    void release_before(std::size_t pos)
    {
        for (auto& buf : m_bufs) {
            if (buf.m_buf_to <= pos) {
                buf.clear();
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

        std::size_t position() const { return m_pos; }

        friend FileSource;

    protected:
        iterator(const FileSource* f, size_t pos) : m_pos{pos}, m_freader{f} {}
        size_t m_pos;
        const FileSource* m_freader;
    };

    iterator end() const { return {this, m_filesize}; }
    iterator begin() const { return {this, 0}; }
    iterator seek(size_t pos) const { return {this, pos}; }

    value_type at(size_t pos) const { return get(seek(pos)); }
    size_t size() const { return m_filesize; }

protected:
    struct buffer
    {
        buffer() = default;
        buffer(const buffer&) = delete;
        buffer(buffer&&) = default;
        buffer& operator=(const buffer&) = delete;
        buffer& operator=(buffer&&) = default;

        size_t read(FILE* fp, size_t n_items)
        {
            assert(n_items <= PageSize && "buffer::read: n_items exceeds PageSize");
            const size_t from_bytes = std::ftell(fp);
            m_buf_from = from_bytes / sizeof(value_type);
            const size_t read_items = std::fread(m_buf.data(), sizeof(value_type), n_items, fp);
            m_valid_count = read_items;
            m_buf_to = m_buf_from + read_items;
            return read_items;
        }

        void clear()
        {
            m_valid_count = 0;
            m_buf_from = 0;
            m_buf_to = 0;
        }

        bool in(iterator i) const { return i.m_pos >= m_buf_from && i.m_pos < m_buf_to; }

        value_type get_unchecked(iterator i) const { return m_buf[i.m_pos - m_buf_from]; }

        std::array<value_type, PageSize> m_buf;
        size_t m_valid_count = 0;
        size_t m_buf_from = 0;
        size_t m_buf_to = 0;
    };

    size_t m_filesize = 0;
    mutable buffer m_bufs[2];
    mutable int m_current_buf = 0;
    FILE* m_fp = nullptr;

    bool read_to(size_t item_pos, int index) const
    {
        if (item_pos >= m_filesize) {
            return false;
        }
        const size_t buf_pos = item_pos - (item_pos % PageSize);
        if (std::fseek(m_fp, static_cast<long>(buf_pos * sizeof(value_type)), SEEK_SET) != 0) {
            return false;
        }
        return m_bufs[index].read(m_fp, PageSize) > 0;
    }
};
} // namespace peg
