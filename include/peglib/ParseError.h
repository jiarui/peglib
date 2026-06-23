#pragma once
#include "peglib/SourceMap.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace peg
{

// ---------------------------------------------------------------------------
// Expected set: recorded at the furthest failure position
// ---------------------------------------------------------------------------

enum class ExpectedKind
{
    RuleLabel, // explicit human-readable label via set_label()
    RuleName,  // NonTerminal name via set_name()
    Literal,   // character or string literal, printable form
    Range,     // character range 'a'..'z', printable form
};

struct ExpectedItem
{
    ExpectedKind kind;
    std::string text; // already escaped for display

    bool operator==(const ExpectedItem& rhs) const noexcept
    {
        return kind == rhs.kind && text == rhs.text;
    }
    bool operator<(const ExpectedItem& rhs) const noexcept
    {
        if (kind != rhs.kind)
            return kind < rhs.kind;
        return text < rhs.text;
    }
};

// Render a single value_type for display in expected-set messages, as a
// UTF-8 std::string (without surrounding quotes). This is the generic
// rendering hook: char passes through directly; wider character types
// (char32_t codepoints, etc.) are UTF-8 encoded.
template<typename CharT>
std::string to_display(CharT c)
{
    // Common case: byte-oriented char. Pass through; escaping is applied by
    // the caller (escape_char_for_expected).
    if constexpr (sizeof(CharT) == 1) {
        return std::string{static_cast<char>(c)};
    } else {
        // Encode a single codepoint (char32_t / wchar_t / etc.) as UTF-8.
        auto cp = static_cast<std::uint32_t>(c);
        std::string out;
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return out;
    }
}

// Escape a single character for display in expected-set messages.
//   - Printable ASCII (0x20..0x7E): the character itself, wrapped in single quotes.
//   - Tab/newline/etc: C-style escapes, wrapped in single quotes.
//   - Other (char): hex form '\xNN', wrapped in single quotes.
//   - Other (wider CharT, e.g. char32_t): UTF-8 encoded via to_display, then
//     hex-escaped if non-printable, wrapped in single quotes.
template<typename CharT>
std::string escape_char_for_expected(CharT c)
{
    std::string s;
    s += '\'';
    // Handle C-style escapes for the common control characters regardless of
    // CharT width (a char32_t '\n' is still U+000A).
    const auto v = static_cast<unsigned long>(static_cast<std::uint32_t>(c));
    switch (v) {
    case '\t':
        s += "\\t";
        break;
    case '\n':
        s += "\\n";
        break;
    case '\r':
        s += "\\r";
        break;
    case '\\':
        s += "\\\\";
        break;
    case '\'':
        s += "\\'";
        break;
    default:
        if (v >= 0x20 && v <= 0x7E) {
            s += static_cast<char>(v);
        } else if constexpr (sizeof(CharT) == 1) {
            // Single-byte non-printable: hex \xNN.
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02X", static_cast<unsigned char>(c));
            s += buf;
        } else {
            // Wider non-printable: hex \UNNNNNNNN (full codepoint width).
            char buf[16];
            std::snprintf(buf, sizeof(buf), "\\U%08X", static_cast<unsigned>(v));
            s += buf;
        }
        break;
    }
    s += '\'';
    return s;
}

// Escape a string (e.g. multi-char terminal sequence) for display.
// Uses double quotes. Control characters use C-style escapes.
// Overload for the common char case: takes std::string_view so string
// literals ("abc") bind without including their trailing NUL byte.
inline std::string escape_string_for_expected(std::string_view s)
{
    std::string out;
    out += '"';
    for (char c : s) {
        switch (c) {
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        default:
            if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) <= 0x7E) {
                out += c;
            } else {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\x%02X", static_cast<unsigned char>(c));
                out += buf;
            }
            break;
        }
    }
    out += '"';
    return out;
}

// Overload for wide-character basic_strings (std::u32string, std::wstring, ...).
// Encodes each codepoint to UTF-8 / \UNNNNNNNN as appropriate. Sized ranges
// only — string literals would include their NUL byte; pass a basic_string or
// basic_string_view instead.
template<typename CharT>
    requires(!std::is_same_v<CharT, char>)
std::string escape_string_for_expected(const std::basic_string<CharT>& s)
{
    std::string out;
    out += '"';
    for (auto e : s) {
        const auto v = static_cast<unsigned long>(static_cast<std::uint32_t>(e));
        switch (v) {
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        default:
            if (v >= 0x20 && v <= 0x7E) {
                out += static_cast<char>(v);
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "\\U%08X", static_cast<unsigned>(v));
                out += buf;
            }
            break;
        }
    }
    out += '"';
    return out;
}

// ---------------------------------------------------------------------------
// Diagnostic: value-object representation of a parse failure.
//
// A Diagnostic carries the furthest failure position and the set of
// "expected" items at that position. It is queryable, accumulable, and
// formatable via a SourceMap.
//
// For cut-committed hard failures, see ParseError (the exception type).
// ---------------------------------------------------------------------------

class Diagnostic
{
public:
    Diagnostic(std::size_t pos, std::set<ExpectedItem> expected)
        : m_pos{pos}, m_expected{std::move(expected)}
    {}

    [[nodiscard]] std::size_t position() const noexcept { return m_pos; }
    [[nodiscard]] const std::set<ExpectedItem>& expected() const noexcept { return m_expected; }

    // Format the diagnostic as: "filename:line:col: error: expected A or B or C"
    // If the expected set is empty, the message reads "error: unexpected input".
    [[nodiscard]] std::string format(const SourceMap& map, std::string_view filename) const
    {
        auto loc = map.locate(m_pos);
        std::ostringstream oss;
        oss << filename << ':' << loc.line << ':' << loc.column << ": error: ";
        if (m_expected.empty()) {
            oss << "unexpected input";
        } else {
            oss << "expected ";
            // English-list join: "a", "a or b", "a, b or c", ...
            const std::size_t n = m_expected.size();
            std::size_t i = 0;
            for (const auto& item : m_expected) {
                if (i > 0) {
                    oss << (i + 1 == n ? " or " : ", ");
                }
                oss << item.text;
                ++i;
            }
        }
        return oss.str();
    }

private:
    std::size_t m_pos;
    std::set<ExpectedItem> m_expected;
};

// ---------------------------------------------------------------------------
// ParseError: exception type thrown when a cut-committed branch fails.
//
// Data-structure compatible with Diagnostic; use to_diagnostic() to convert.
// ---------------------------------------------------------------------------

class ParseError : public std::runtime_error
{
public:
    ParseError(std::size_t pos, std::set<ExpectedItem> expected)
        : std::runtime_error{"peg::ParseError: cut-committed branch failed"}, m_pos{pos},
          m_expected{std::move(expected)}
    {
        // Pre-build the bare message (no file:line:col — that requires a SourceMap).
        std::ostringstream oss;
        oss << "parse error at offset " << m_pos;
        if (!m_expected.empty()) {
            oss << ": expected ";
            bool first = true;
            for (const auto& item : m_expected) {
                if (!first)
                    oss << " or ";
                first = false;
                oss << item.text;
            }
        }
        m_what = oss.str();
    }

    [[nodiscard]] std::size_t position() const noexcept { return m_pos; }
    [[nodiscard]] const std::set<ExpectedItem>& expected() const noexcept { return m_expected; }

    [[nodiscard]] Diagnostic to_diagnostic() const { return Diagnostic{m_pos, m_expected}; }

    [[nodiscard]] const char* what() const noexcept override { return m_what.c_str(); }

private:
    std::size_t m_pos;
    std::set<ExpectedItem> m_expected;
    std::string m_what;
};

} // namespace peg
