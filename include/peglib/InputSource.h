#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>

#include "FileSource.h"

namespace peg
{

// ---------------------------------------------------------------------------
// InputSourceBase: type-erased interface to the parsed input.
//
// Context holds a unique_ptr<InputSourceBase<CharT>> so that the same
// Context<CharT, NodeType> can drive either a contiguous in-memory buffer
// (SpanSource) or a paged file reader (FileSourceSource) without the Context
// template itself depending on the storage strategy.
//
// Performance note: SpanSource also exposes a raw `data()` pointer that
// Context caches (m_fast_data). When that cache is non-null, the parser's
// per-character hot path (Context::current / Context::at) indexes into the
// raw pointer with zero virtual dispatch. FileSourceSource leaves the cache
// null, so its per-character access goes through the virtual `at()` — which
// is acceptable because FileSource is I/O-bound anyway.
// ---------------------------------------------------------------------------
template<typename CharT>
struct InputSourceBase
{
    virtual ~InputSourceBase() = default;
    virtual CharT at(std::size_t offset) const = 0;
    virtual std::size_t size() const = 0;
    virtual std::basic_string<CharT> substr(std::size_t offset, std::size_t count) const = 0;

    // Cut-driven buffer eviction. Default is a no-op (contiguous sources have
    // nothing to evict). FileSourceSource overrides to drop pages strictly
    // before `offset`, reclaiming memory once the parser commits a cut.
    virtual void release_before(std::size_t /*offset*/) {}

    // Non-virtual fast-path accessor: returns a raw pointer to contiguous
    // storage, or nullptr if the source is not contiguous. Context caches
    // this once at construction so the per-character hot path avoids a
    // virtual call entirely for span-backed inputs.
    const CharT* contiguous_data() const noexcept { return m_contiguous_data; }

protected:
    // Null for paged sources (FileSource); set by SpanSource to its internal
    // buffer pointer. Stored on the base so Context can read it without a
    // virtual call or a downcast.
    const CharT* m_contiguous_data = nullptr;
};

// ---------------------------------------------------------------------------
// SpanSource: adapter for contiguous in-memory ranges (std::string,
// std::vector, std::span). Non-owning — the caller must keep the original
// range alive for the source's lifetime, exactly as the old span-backed
// Context required.
// ---------------------------------------------------------------------------
template<typename CharT>
struct SpanSource : InputSourceBase<CharT>
{
    SpanSource(const CharT* data, std::size_t size) : m_data{data}, m_size{size}
    {
        this->m_contiguous_data = data;
    }

    CharT at(std::size_t offset) const override
    {
        // Mirrors the old span operator[]: caller (Context::current) is
        // responsible for range-checking via assert before calling.
        return m_data[offset];
    }

    std::size_t size() const override { return m_size; }

    std::basic_string<CharT> substr(std::size_t offset, std::size_t count) const override
    {
        return std::basic_string<CharT>{m_data + offset, count};
    }

private:
    const CharT* m_data;
    std::size_t m_size;
};

// ---------------------------------------------------------------------------
// FileSourceSource: adapter for FileSource. Owns the FileSource by move,
// forwarding offset-based access. Overrides release_before to drive the
// cut-eviction that FileSource supports natively.
// ---------------------------------------------------------------------------
template<typename CharT>
struct FileSourceSource : InputSourceBase<CharT>
{
    explicit FileSourceSource(FileSource<CharT>&& fs) : m_fs{std::move(fs)} {}

    CharT at(std::size_t offset) const override { return m_fs.at(offset); }

    std::size_t size() const override { return m_fs.size(); }

    std::basic_string<CharT> substr(std::size_t offset, std::size_t count) const override
    {
        std::basic_string<CharT> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(m_fs.at(offset + i));
        }
        return out;
    }

    void release_before(std::size_t offset) override { m_fs.release_before(offset); }

    // Expose the underlying FileSource for SourceMap, which walks the file
    // directly (prescanning line offsets) rather than through the parser.
    const FileSource<CharT>& file_source() const noexcept { return m_fs; }

private:
    FileSource<CharT> m_fs;
};

} // namespace peg
