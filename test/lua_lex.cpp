#include "peglib.h"

#include "doctest.h"

#include <climits>
#include <variant>
using namespace peg;

auto WS = +terminal(std::set({' ', '\f', '\t', '\v'}));
auto linebreak = terminal('\n');
auto not_linebreak = terminal<char>([](char c) { return c != '\n'; });
auto digit = terminal('0', '9');
auto xdigit = terminal<char>([](char c) { return std::isxdigit(c); });
auto fractional = -(terminal('+') | '-') >>
                  ((*digit >> terminal('.') >> +digit) | (+digit >> terminal('.') >> *digit));
auto decimal = -(terminal('+') | '-') >> +digit;
auto hexdecimal = terminal('0') >> (terminal('x') | 'X') >> +xdigit >>
                  -(terminal('.') >> +xdigit) >> -((terminal('p') | 'P') >> decimal);
auto escape_single_quote = terminal('\\') >>
                           (terminal('a') | 'b' | 'f' | 'n' | 'r' | 't' | 'v' | '\\' | '\'' |
                            ('z' >> WS) | (3 * digit) | (2 * xdigit) |
                            terminal('u') >> '{' >> *xdigit >> '}');
auto string_content =
    terminal<char>([](char c) { return c != '\'' && c != '\\' && c != '\r' && c != '\n'; });
auto string_single_quote = terminal('\'') >> *(string_content | escape_single_quote) >>
                           terminal('\'');
auto cut_ = cut<Context<std::span<const char>>>();

Grammar<> g;

[[maybe_unused]] const bool grammar_initialized = [] {
    g["names"] = terminal<char>([](char c) { return std::isalpha(c) || c == '_'; }) >>
                 *terminal<char>([](char c) { return std::isalnum(c) || c == '_'; });
    g["numeral"] =
        hexdecimal | ((fractional | decimal) >> -((terminal('e') | 'E') >> -(decimal)));
    g["comment"] = terminal('-') >> '-' >> *not_linebreak >> linebreak;
    g["string_literal"] = string_single_quote;
    g["ops"] = terminal('(') | terminal(')');
    g["token"] = (g["numeral"] >> cut_) | (g["names"] >> cut_) | (g["string_literal"] >> cut_) |
                 g["ops"] | g["comment"] | WS;
    g["lexer"] = +g["token"];
    return true;
}();

enum class TokenID : std::intmax_t
{
    TK_AND = UCHAR_MAX + 1,
    TK_BREAK,
    TK_DO,
    TK_ELSE,
    TK_ELSEIF,
    TK_END,
    TK_FALSE,
    TK_FOR,
    TK_FUNCTION,
    TK_GOTO,
    TK_IF,
    TK_IN,
    TK_LOCAL,
    TK_NIL,
    TK_NOT,
    TK_OR,
    TK_REPEAT,
    TK_RETURN,
    TK_THEN,
    TK_TRUE,
    TK_UNTIL,
    TK_WHILE,
    /* other terminal symbols */
    TK_IDIV,
    TK_CONCAT,
    TK_DOTS,
    TK_EQ,
    TK_GE,
    TK_LE,
    TK_NE,
    TK_SHL,
    TK_SHR,
    TK_DBCOLON,
    TK_EOS,
    TK_FLT,
    TK_INT,
    TK_NAME,
    TK_STRING
};

struct Token
{
    using TokenInfo = std::variant<long long, double, std::string>;
    Token() = default;
    Token(TokenID i) : id{i} {}
    Token(const std::string str) : id{TokenID::TK_NAME}, info{std::move(str)} {}
    Token(int i) : id{TokenID::TK_INT} { info.emplace<0>(i); }
    Token(double d) : id{TokenID::TK_FLT} { info.emplace<1>(d); }
    TokenID id;
    TokenInfo info;
};

struct TokenizerTest
{
    std::vector<Token> m_token_buf;
    void run(const std::string& input)
    {
        g["names"].set_action(
            [this](Context<std::span<const std::string::value_type>>& context,
                   Context<std::span<const std::string::value_type>>::ParseTreeNodePtr node)
                -> std::monostate {
                auto& input = context.get_input();
                std::string m = std::string{input.begin() + node->start_offset,
                                            input.begin() + node->end_offset};
                if (m == "if") {
                    m_token_buf.emplace_back(TokenID::TK_IF);
                } else if (node->end_offset > node->start_offset) {
                    m_token_buf.emplace_back(m);
                }
                return {};
            });
        Context context(input);
        while (!context.ended()) {
            g.parse("token", context);
        }
    }
    ~TokenizerTest() { g["names"].set_action(nullptr); }
};

TEST_CASE("lua-lex-token")
{
    {
        TokenizerTest t;
        t.run("if test");
        CHECK(t.m_token_buf.size() == 2);
        CHECK(t.m_token_buf[0].id == TokenID::TK_IF);
        CHECK(t.m_token_buf[1].id == TokenID::TK_NAME);
    }
    {
        TokenizerTest t;
        t.run("if true here");
        CHECK(t.m_token_buf.size() == 3);
        CHECK(t.m_token_buf[0].id == TokenID::TK_IF);
        CHECK(t.m_token_buf[1].id == TokenID::TK_NAME);
        CHECK(t.m_token_buf[1].id == TokenID::TK_NAME);
    }
}

TEST_CASE("lua-lex-names")
{
    {
        std::string input = R"(   print)";
        Context context(input);
        g["names"].set_action(([](decltype(context)& c, decltype(context)::ParseTreeNodePtr node)
                                   -> std::monostate {
            auto& input = c.get_input();
            CHECK(std::string(input.begin() + node->start_offset,
                              input.begin() + node->end_offset) == "print");
            return {};
        }));

        g["ws"] = WS;
        bool ok = g.parse("ws", context);
        CHECK(ok);
        auto start = context.mark();
        CHECK(g.parse("names", context));
        CHECK(std::string(start, context.mark()) == "print");
    }
}

TEST_CASE("lua-lex-number")
{
    std::vector<std::string> tests = {R"(123)",
                                      R"(3)",
                                      R"(345)",
                                      R"(0xff)",
                                      R"(0xBEBADA)",
                                      R"(3.0)",
                                      R"(3.1416)",
                                      R"(314.16e-2)",
                                      R"(0.31416E1)",
                                      R"(34e1)",
                                      R"(0x0.1E)",
                                      R"(0xA23p-4)",
                                      R"(0X1.921FB54442D18P+1)"};
    for (const auto& t : tests) {
        std::string input = t;
        Context context(input);
        auto start = context.mark();
        CHECK(g.parse("numeral", context));
        CHECK(std::string(start, context.mark()) == input);
    }
}

TEST_CASE("lua-lex-comment")
{
    std::vector<std::string> tests = {
        R"(--adfasdb
)"};
    for (const auto& t : tests) {
        std::string input = t;
        Context context(input);
        auto start = context.mark();
        CHECK(g.parse("comment", context));
        CHECK(std::string(start, context.mark()) == input);
    }
}

TEST_CASE("lua-lex-string")
{
    std::vector<std::string> tests = {R"('abc')", R"('abc\n123')"};
    for (const auto& t : tests) {
        std::string input = t;
        Context context(input);
        auto start = context.mark();
        CHECK(g.parse("string_literal", context));
        CHECK(std::string(start, context.mark()) == input);
    }
}

TEST_CASE("lua-lex-tokens")
{
    std::string input = R"(print('hello world'))";
    Context context(input);
    g["names"].set_action([](decltype(context)& c,
                             decltype(context)::ParseTreeNodePtr node) -> std::monostate {
        return {};
    });
    {
        auto start = context.mark();
        CHECK(g.parse("token", context));
        CHECK(std::string(start, context.mark()) == "print");
    }
    {
        auto start = context.mark();
        CHECK(g.parse("token", context));
        CHECK(std::string(start, context.mark()) == "(");
    }
    {
        auto start = context.mark();
        CHECK(g.parse("token", context));
        CHECK(std::string(start, context.mark()) == "'hello world'");
    }
    {
        auto start = context.mark();
        CHECK(g.parse("token", context));
        CHECK(std::string(start, context.mark()) == ")");
    }
}

namespace lexconv
{
auto WS = +terminal(std::set({' ', '\f', '\t', '\v'}));
auto not_linebreak = terminal<char>([](char c) { return c != '\n'; });
auto name_start = terminal<char>([](char c) { return std::isalpha(c) || c == '_'; });
auto name_cont = terminal<char>([](char c) { return std::isalnum(c) || c == '_'; });
auto linebreak = terminalSeq<char>("\r\n") | terminal('\n');
auto digit = terminal('0', '9');
auto xdigit = terminal<char>([](char c) { return std::isxdigit(c); });
auto pos_or_neg = (terminal('+') | '-');
auto fractional = -pos_or_neg >> ((*digit >> '.' >> +digit) | (+digit >> '.' >> *digit));
auto decimal = -pos_or_neg >> +digit;
auto hexdecimal = terminal('0') >> (terminal('x') | 'X') >> +xdigit >> -('.' >> +xdigit) >>
                  -((terminal('p') | 'P') >> +decimal);
auto expotent = -pos_or_neg >> +digit;
auto common_escape_code = terminal('a') | 'b' | 'f' | 'n' | 'r' | 't' | 'v' |
                          (terminal('\\') >> '\\' >> 'n') | ('z' >> WS) | (3 * digit) |
                          (2 * xdigit) | (terminal('u') >> '{' >> *xdigit >> '}');
auto single_no_escape_code = terminal<char>([](char c) { return c != '\''; });
auto double_no_escape_code = terminal<char>([](char c) { return c != '"'; });

Grammar<> g;

[[maybe_unused]] const bool grammar_initialized = [] {
    g["name"] = name_start >> *name_cont;
    g["numeral"] =
        hexdecimal | ((fractional | decimal) >> (terminal('e') | 'E') >> -(decimal));
    g["single_escape_code"] = terminal('\\') >> (common_escape_code | '\'');
    g["double_escape_code"] = terminal('\\') >> (common_escape_code | '\'');
    auto string_single_quote = '\'' >> *(g["single_escape_code"] | single_no_escape_code) >> '\'';
    auto string_double_quote = '"' >> *(g["double_escape_code"] | double_no_escape_code) >> '"';
    g["long_bracket_start"] = '[' >> *terminal('=') >> '[';
    g["string_literal"] = string_single_quote | string_double_quote | g["long_bracket_start"];
    g["comment"] =
        terminal('-') >> '-' >> (g["long_bracket_start"] | (*not_linebreak >> linebreak));
    g["token"] = g["numeral"] | g["name"] | g["string_literal"] | g["comment"] | WS;
    return true;
}();
} // namespace lexconv
