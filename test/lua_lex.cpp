#include "peglib.h"

#include "doctest.h"

#include <climits>
#include <variant>
using namespace peg;

Grammar<> g;

[[maybe_unused]] const bool grammar_initialized = [] {
    // Terminal building blocks. These are Grammar member factories now (the
    // free peg::terminal helpers were removed), so they must be built from `g`
    // — they are local to this initialiser where `g` is in scope.
    auto WS = +g.terminal(std::set<char>{' ', '\f', '\t', '\v'});
    auto linebreak = g.terminal('\n');
    auto not_linebreak = g.terminal([](char c) { return c != '\n'; });
    auto digit = g.terminal('0', '9');
    auto xdigit = g.terminal([](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
    auto fractional = -(g.terminal('+') | '-') >> ((*digit >> g.terminal('.') >> +digit) |
                                                   (+digit >> g.terminal('.') >> *digit));
    auto decimal = -(g.terminal('+') | '-') >> +digit;
    auto hexdecimal = g.terminal('0') >> (g.terminal('x') | 'X') >> +xdigit >>
                      -(g.terminal('.') >> +xdigit) >> -((g.terminal('p') | 'P') >> decimal);
    auto escape_single_quote =
        g.terminal('\\') >>
        (g.terminal('a') | 'b' | 'f' | 'n' | 'r' | 't' | 'v' | '\\' | '\'' | ('z' >> WS) |
         (3 * digit) | (2 * xdigit) | g.terminal('u') >> '{' >> *xdigit >> '}');
    auto string_content =
        g.terminal([](char c) { return c != '\'' && c != '\\' && c != '\r' && c != '\n'; });
    auto string_single_quote =
        g.terminal('\'') >> *(string_content | escape_single_quote) >> g.terminal('\'');
    auto cut_ = g.cut();

    g["names"] = g.terminal([](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }) >> *g.terminal([](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    });
    g["numeral"] =
        hexdecimal | ((fractional | decimal) >> -((g.terminal('e') | 'E') >> -(decimal)));
    g["comment"] = g.terminal('-') >> '-' >> *not_linebreak >> linebreak;
    g["string_literal"] = string_single_quote;
    g["ws"] = WS;
    g["ops"] = g.terminal('(') | g.terminal(')');
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
    Token(std::string str) : id{TokenID::TK_NAME}, info{std::move(str)} {}
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
            [this](Context<char>& context,
                   const Context<char>::ParseTreeNodePtr& node) -> std::monostate {
                std::string m = context.input().slice(node->start_offset,
                                                      node->end_offset - node->start_offset);
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
        g["names"].set_action(
            ([](decltype(context)& c,
                const decltype(context)::ParseTreeNodePtr& node) -> std::monostate {
                CHECK(c.input().slice(node->start_offset, node->end_offset - node->start_offset) ==
                      "print");
                return {};
            }));

        g["ws"] = +g.terminal(std::set<char>{' ', '\f', '\t', '\v'});
        bool ok = g.parse("ws", context);
        CHECK(ok);
        auto start = context.mark();
        CHECK(g.parse("names", context));
        CHECK(context.input().slice(start, context.mark() - start) == "print");
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
        CHECK(context.input().slice(start, context.mark() - start) == input);
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
        CHECK(context.input().slice(start, context.mark() - start) == input);
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
        CHECK(context.input().slice(start, context.mark() - start) == input);
    }
}

TEST_CASE("lua-lex-tokens")
{
    std::string input = R"(print('hello world'))";
    Context context(input);
    g["names"].set_action(
        [](decltype(context)& /*c*/,
           const decltype(context)::ParseTreeNodePtr& /*node*/) -> std::monostate { return {}; });
    {
        auto start = context.mark();
        CHECK(g.parse("token", context));
        CHECK(context.input().slice(start, context.mark() - start) == "print");
    }
    {
        auto start = context.mark();
        CHECK(g.parse("token", context));
        CHECK(context.input().slice(start, context.mark() - start) == "(");
    }
    {
        auto start = context.mark();
        CHECK(g.parse("token", context));
        CHECK(context.input().slice(start, context.mark() - start) == "'hello world'");
    }
    {
        auto start = context.mark();
        CHECK(g.parse("token", context));
        CHECK(context.input().slice(start, context.mark() - start) == ")");
    }
}
