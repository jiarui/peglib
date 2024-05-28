#define BOOST_TEST_MODULE lua_test
#include <boost/test/unit_test.hpp>
#include "peglib.h"
#include <variant>
using namespace peg;

auto WS = +terminal(std::set({' ', '\f', '\t', '\v'}));
auto linebreak = terminal('\n');
auto not_linebreak = terminal<char>([](char c){return c!='\n';});
Rule<std::string::value_type> names = terminal<char>([](char c){return std::isalpha(c) || c == '_';}) 
        >> *terminal<char>([](char c){return std::isalnum(c) || c=='_';});
auto digit = terminal('0', '9');
auto xdigit = terminal<char>([](char c){return std::isxdigit(c);});
auto fractional = -(terminal('+') | '-') >> ((*digit >> terminal('.') >> +digit) | (+digit >> terminal('.') >> *digit));
auto decimal = -(terminal('+') | '-') >> +digit;

auto hexdecimal = terminal('0') >> (terminal('x') | 'X') >> +xdigit >> -(terminal('.') >> +xdigit) >> 
            -((terminal('p') | 'P') >> decimal);
Rule<std::string::value_type> numeral = hexdecimal | ((fractional | decimal) >> -((terminal('e') | 'E') >> -(decimal)));

Rule<std::string::value_type> comment = terminal('-') >> '-' >> *not_linebreak >> linebreak;

auto escape_single_quote = terminal('\\') >> (terminal('a') | 'b' | 'f' | 'n' | 'r' | 't' | 'v' | '\\' | '\'' | ('z' >> WS) | (3 * digit) | (2 * xdigit) | terminal('u') >> '{' >> *xdigit >> '}');
auto string_content = terminal<char>([](char c){return c != '\'' && c != '\\' && c != '\r' && c!='\n';});
auto string_single_quote = terminal('\'')>> *(string_content | escape_single_quote) >> terminal('\'');
Rule<std::string::value_type> string_literal = string_single_quote;

Rule<std::string::value_type> ops = terminal('(') | terminal(')');

Rule<std::string::value_type> token = numeral | names | string_literal | ops | comment | WS;
Rule<std::string::value_type> lexer = +token;


enum class TokenID : std::intmax_t {
    TK_AND = UCHAR_MAX + 1, TK_BREAK,
    TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION,
    TK_GOTO, TK_IF, TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR, TK_REPEAT,
    TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL, TK_WHILE,
    /* other terminal symbols */
    TK_IDIV, TK_CONCAT, TK_DOTS, TK_EQ, TK_GE, TK_LE, TK_NE,
    TK_SHL, TK_SHR,
    TK_DBCOLON, TK_EOS,
    TK_FLT, TK_INT, TK_NAME, TK_STRING
};

struct Token{
    using TokenInfo = std::variant<long long, double, std::string>;
    Token() = default;
    Token(TokenID i) : id{i} {}
    Token(const std::string str) : id{TokenID::TK_NAME}, info{std::move(str)} {}
    Token(int i) : id{TokenID::TK_INT}{ info.emplace<0>(i);}
    Token(double d) : id{TokenID::TK_FLT}{ info.emplace<1>(d);}
    TokenID id;
    TokenInfo info;
};

struct TokenizerTest {
    std::vector<Token> m_token_buf;
    void run(const std::string& input) {
        names.setAction(
            [this](Context<std::string::value_type>& context, Context<std::string::value_type>::MatchRange match) {
                std::string m = std::string{match.begin(), match.end()};
                if(m == "if"){
                    m_token_buf.emplace_back(TokenID::TK_IF);
                }
                else if (match.size() > 0) {
                    m_token_buf.emplace_back(m);
                }
            }
        );
        Context context(input);
        while(!context.ended()){
            token(context);
        }
    }
    ~TokenizerTest() {
        names.setAction(nullptr);
    }
};

BOOST_AUTO_TEST_CASE(test_token) {
    {
        TokenizerTest t;
        t.run("if test");
        BOOST_CHECK_EQUAL(t.m_token_buf.size(), 2);
        BOOST_CHECK(t.m_token_buf[0].id == TokenID::TK_IF);
        BOOST_CHECK(t.m_token_buf[1].id == TokenID::TK_NAME);
    }
    {
        TokenizerTest t;
        t.run("if true here");
        BOOST_CHECK_EQUAL(t.m_token_buf.size(), 2);
        BOOST_CHECK(t.m_token_buf[0].id == TokenID::TK_IF);
        BOOST_CHECK(t.m_token_buf[1].id == TokenID::TK_NAME);

    }
}


BOOST_AUTO_TEST_CASE(test_names) {
    {
        names.setAction(([](Context<char>& c, std::span<const char> range){
            BOOST_CHECK_EQUAL(std::string(range.begin(), range.end()), "print");
        }));
        std::string input = R"(   print)";
        Context context(input);
        Rule<std::string::value_type> ws = WS;
        bool ok = ws(context);
        BOOST_TEST(ok);
        auto start = context.mark();
        BOOST_TEST(names(context));
        BOOST_CHECK_EQUAL(std::string(start, context.mark()), "print");
    }
}


BOOST_AUTO_TEST_CASE(test_number) {
    std::vector<std::string> tests= {
        R"(123)",
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
        R"(0X1.921FB54442D18P+1)"
    };
    for(const auto& t : tests){
        std::string input = t;
        Context context(input);
        auto start = context.mark();
        BOOST_TEST(numeral(context));
        BOOST_CHECK_EQUAL(std::string(start, context.mark()), input);
    }
}

BOOST_AUTO_TEST_CASE(test_comment) {
    std::vector<std::string> tests= {
        R"(--adfasdb
)"
    };
    for(const auto& t : tests){
        std::string input = t;
        Context context(input);
        auto start = context.mark();
        BOOST_TEST(comment(context));
        BOOST_CHECK_EQUAL(std::string(start, context.mark()), input);
    }
}

BOOST_AUTO_TEST_CASE(test_string) {
    std::vector<std::string> tests= {
        R"('abc')",
        R"('abc\n123')"
    };
    for(const auto& t : tests){
        std::string input = t;
        Context context(input);
        auto start = context.mark();
        BOOST_TEST(string_literal(context));
        BOOST_CHECK_EQUAL(std::string(start, context.mark()), input);
    }
}

BOOST_AUTO_TEST_CASE(test_tokens) {
    std::string input = R"(print('hello world'))";
    Context context(input);
    names.setAction([](Context<char>& c, std::span<const char> range){
        });
    {
        auto start = context.mark();
        BOOST_TEST(token(context));
        BOOST_CHECK_EQUAL(std::string(start, context.mark()), "print");
    }
    {
        auto start = context.mark();
        BOOST_TEST(token(context));
        BOOST_CHECK_EQUAL(std::string(start, context.mark()), "(");

    }
    {
        auto start = context.mark();
        BOOST_TEST(token(context));
        BOOST_CHECK_EQUAL(std::string(start, context.mark()), "'hello world'");

    }
    {
        auto start = context.mark();
        BOOST_TEST(token(context));
        BOOST_CHECK_EQUAL(std::string(start, context.mark()), ")");

    }
    
}