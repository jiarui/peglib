#define BOOST_TEST_MODULE lua_test
#include <boost/test/unit_test.hpp>
#include "peglib.h"
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

Rule<std::string::value_type> token = numeral | names | string_literal | ops | comment;
Rule<std::string::value_type> lexer = +token;

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