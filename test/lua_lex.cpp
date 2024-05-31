#define BOOST_TEST_MODULE lua_test
#include <boost/test/unit_test.hpp>
#include "peglib.h"
#include <variant>
using namespace peg;

auto WS = +terminal(std::set({' ', '\f', '\t', '\v'}));
auto linebreak = terminal('\n');
auto not_linebreak = terminal<char>([](char c){return c!='\n';});
Rule<> names = terminal<char>([](char c){return std::isalpha(c) || c == '_';}) 
        >> *terminal<char>([](char c){return std::isalnum(c) || c=='_';});
auto digit = terminal('0', '9');
auto xdigit = terminal<char>([](char c){return std::isxdigit(c);});
auto fractional = -(terminal('+') | '-') >> ((*digit >> terminal('.') >> +digit) | (+digit >> terminal('.') >> *digit));
auto decimal = -(terminal('+') | '-') >> +digit;

auto hexdecimal = terminal('0') >> (terminal('x') | 'X') >> +xdigit >> -(terminal('.') >> +xdigit) >> 
            -((terminal('p') | 'P') >> decimal);
Rule<> numeral = hexdecimal | ((fractional | decimal) >> -((terminal('e') | 'E') >> -(decimal)));

Rule<> comment = terminal('-') >> '-' >> *not_linebreak >> linebreak;

auto escape_single_quote = terminal('\\') >> (terminal('a') | 'b' | 'f' | 'n' | 'r' | 't' | 'v' | '\\' | '\'' | ('z' >> WS) | (3 * digit) | (2 * xdigit) | terminal('u') >> '{' >> *xdigit >> '}');
auto string_content = terminal<char>([](char c){return c != '\'' && c != '\\' && c != '\r' && c!='\n';});
auto string_single_quote = terminal('\'')>> *(string_content | escape_single_quote) >> terminal('\'');
Rule<> string_literal = string_single_quote;

Rule<> ops = terminal('(') | terminal(')');

auto cut_ = cut<Context<std::span<const char>>>();

Rule<> token = (numeral >> cut_) | (names >> cut_) | (string_literal >> cut_) | ops | comment | WS;
Rule<> lexer = +token;


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
            [this](Context<std::span<const std::string::value_type>>& context, Context<std::span<const std::string::value_type>>::match_range match) {
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
        BOOST_CHECK_EQUAL(t.m_token_buf.size(), 3);
        BOOST_CHECK(t.m_token_buf[0].id == TokenID::TK_IF);
        BOOST_CHECK(t.m_token_buf[1].id == TokenID::TK_NAME);
        BOOST_CHECK(t.m_token_buf[1].id == TokenID::TK_NAME);

    }
}


BOOST_AUTO_TEST_CASE(test_names) {
    {
        std::string input = R"(   print)";
        Context context(input);
        names.setAction(([](decltype(context)& c, decltype(context)::match_range range){
            BOOST_CHECK_EQUAL(std::string(range.begin(), range.end()), "print");
        }));
        
        Rule<> ws = WS;
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
    names.setAction([](decltype(context)& c, decltype(context)::match_range range){
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

namespace lexconv{
    auto WS = +terminal(std::set({' ', '\f', '\t', '\v'}));
    auto not_linebreak = terminal<char>([](char c){return c!='\n';});
    auto name_start = terminal<char>([](char c){return std::isalpha(c) || c == '_';});
    auto name_cont = terminal<char>([](char c){return std::isalnum(c) || c=='_';});
    Rule<> name =  name_start >> *name_cont;
    auto linebreak = terminalSeq<char>("\r\n") | terminal('\n');
    auto digit = terminal('0', '9');
    auto xdigit = terminal<char>([](char c){return std::isxdigit(c);});
    auto pos_or_neg = (terminal('+') | '-');
    auto fractional = -pos_or_neg >> ((*digit >> '.' >> +digit) | (+digit >> '.' >> *digit));
    auto decimal = -pos_or_neg >> +digit;
    auto hexdecimal = terminal('0') >> (terminal('x') | 'X') >> +xdigit >> -('.' >> +xdigit) >> -((terminal('p') | 'P') >> +decimal);
    auto expotent = -pos_or_neg >> +digit;
    Rule<> numeral = hexdecimal | ((fractional | decimal) >> (terminal('e') | 'E') >> -(decimal));

    auto common_escape_code = terminal('a') | 'b' | 'f' | 'n' | 'r' | 't' | 'v' | (terminal('\\') >>'\\'>>'n')| ('z' >> WS) | (3 * digit) | (2 * xdigit) | (terminal('u') >> '{' >> *xdigit >> '}') ;
    Rule<> single_escape_code = terminal('\\') >> ( common_escape_code | '\'' );
    Rule<> double_escape_code = terminal('\\') >> ( common_escape_code | '\'' );
    auto single_no_escape_code = terminal<char>([](char c){return c != '\'';});
    auto double_no_escape_code = terminal<char>([](char c){return c != '"';});
    auto string_single_quote = '\'' >> *(single_escape_code | single_no_escape_code) >> '\'';
    auto string_double_quote = '"' >> *(double_escape_code | double_no_escape_code) >> '"';
    Rule<> long_bracket_start = '[' >> *terminal('=') >> '[';
    Rule<> string_literal = string_single_quote | string_double_quote | long_bracket_start;
    
    Rule<> comment = terminal('-') >> '-' >> (long_bracket_start | (*not_linebreak >> linebreak));
    Rule<> token = numeral | name | string_literal | comment | WS;
};

