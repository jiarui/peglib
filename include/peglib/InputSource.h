// InputSourceBase: type-erased input interface. Context holds a
// unique_ptr<InputSourceBase<CharT>> so a single Context<CharT, NodeType>
// can drive either a contiguous in-memory buffer (SpanSource) or a paged
// file reader (FileSourceSource).
//
// SpanSource exposes a raw pointer that Context caches (m_fast_data); when
// non-null, the per-character hot path (Context::current / Context::at)
// indexes it with zero virtual dispatch. FileSourceSource leaves it null.
#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>

#include "FileSource.h"

namespace peg
{

template<typename CharT>
struct InputSourceBase
{
    virtual ~InputSourceBase() = default;
    virtual CharT at(std::size_t offset) const = 0;
    virtual std::size_t size() const = 0;

    // Default no-op (contiguous sources have nothing to evict); FileSourceSource
    // overrides to drop pages strictly before `offset` on cut commitment.
    virtual void release_before(std::size_t /*offset*/) {}

    // Slice [offset, offset+count). Constrained to integral CharT:
    // basic_string<CharT> is ill-formed for non-trivially-copyable CharT, so
    // token-level grammars have no slicing API (read payloads via ctx.at).
    std::basic_string<CharT> slice(std::size_t offset, std::size_t count) const
        requires std::is_integral_v<CharT>
    {
        if (m_contiguous_data != nullptr) {
            return std::basic_string<CharT>{m_contiguous_data + offset, count};
        }
        std::basic_string<CharT> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(this->at(offset + i));
        }
        return out;
    }

    // Raw pointer to contiguous storage, or nullptr if the source is paged.
    const CharT* contiguous_data() const noexcept { return m_contiguous_data; }

protected:
    const CharT* m_contiguous_data = nullptr;
};

// Adapter for contiguous in-memory ranges (std::string, std::vector, span).
// Non-owning: caller must keep the original range alive.
template<typename CharT>
struct SpanSource : InputSourceBase<CharT>
{
    SpanSource(const CharT* data, std::size_t size) : m_data{data}, m_size{size}
    {
        this->m_contiguous_data = data;
    }

    CharT at(std::size_t offset) const override { return m_data[offset]; }
    std::size_t size() const override { return m_size; }

private:
    const CharT* m_data;
    std::size_t m_size;
};

// Adapter for FileSource. Owns the FileSource by move; forwards cut-driven
// eviction via release_before.
template<typename CharT, std::size_t PageSize>
struct FileSourceSource : InputSourceBase<CharT>
{
    explicit FileSourceSource(FileSource<CharT, PageSize>&& fs) : m_fs{std::move(fs)} {}

    CharT at(std::size_t offset) const override { return m_fs.at(offset); }
    std::size_t size() const override { return m_fs.size(); }

    void release_before(std::size_t offset) override { m_fs.release_before(offset); }

    // Exposed for SourceMap, which walks the file directly.
    const FileSource<CharT, PageSize>& file_source() const noexcept { return m_fs; }

private:
    FileSource<CharT, PageSize> m_fs;
};

} // namespace peg
