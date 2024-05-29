#include <boost/test/unit_test.hpp>
#include "peglib.h"
#include <cstddef>

using namespace peg;
using LuaRule = Rule<>;

extern LuaRule expr;
extern LuaRule namelist;
extern LuaRule block;
extern LuaRule explist;
extern LuaRule prefixexp;
extern LuaRule var;
LuaRule Name = terminal('a');
LuaRule LiteralString = terminalSeq("\"hello\"");
LuaRule Numeral = terminalSeq("10");
LuaRule unop = terminal('-') | terminalSeq("not") | terminal('#') | terminal('~');
LuaRule binop = terminalSeq("//") | terminal('+');
LuaRule fieldsep = terminal(',') | terminal(';');
LuaRule field = (terminal('[') >> expr >> terminal(']') >> terminal('=') >> expr) | (Name >> terminal('=') >> expr) | expr;
LuaRule fieldlist = field >> *(fieldsep >> field) >> -fieldsep;
LuaRule tableconstructor = terminal('{') >> -fieldlist >> terminal('}');
LuaRule parlist = namelist >> (-(terminal(',')>>terminalSeq("...")) | terminalSeq("..."));
LuaRule funcbody = terminal('(') >> -(parlist) >> terminal(')') >> block >> terminalSeq("end");
LuaRule functiondef = terminalSeq("function") >> funcbody;
LuaRule args = (terminal('(') >> -explist >> terminal(')')) | tableconstructor | LiteralString;
LuaRule functioncall = (prefixexp >> args) | (prefixexp >> terminal(':') >> Name >> args);
LuaRule prefixexp = var |
                    functioncall |
                    (terminal('(') >> expr >> terminal(')'));
LuaRule expr = terminalSeq("nil") |
              terminalSeq("false") | 
              terminalSeq("true") |
              terminalSeq("...") |
              functiondef |
              prefixexp |
              tableconstructor |
              (expr >> binop >> expr) |
              (unop >> expr) |
              Numeral |
              LiteralString;
LuaRule explist = expr >> *(terminal(',') >> expr);
LuaRule namelist = Name >> *(terminal(',') >> Name);
LuaRule var = Name | (prefixexp >> terminal('[') >> expr >> terminal(']')) | (prefixexp >> terminal('.') >> Name);
LuaRule varlist = var >> *(terminal(',') >> var);
LuaRule funcname = Name >> *(terminal('.') >> Name) >> -(terminal(':') >> Name);
LuaRule label = terminalSeq("::") >> Name >> terminalSeq("::");
LuaRule retstat = terminalSeq("return") >> -explist >> terminal(';');
LuaRule attrib = -(terminal('<') >> Name >> terminal('.'));
LuaRule attnamlist = Name >> attrib >> *(terminal(',') >> Name >> attrib);
LuaRule stat = terminal(';') |
               (varlist >> terminal('=') >> explist) |
               functioncall |
               label |
               terminalSeq("break") |
               (terminalSeq("goto") >> Name) |
               (terminalSeq("do") >> block >> terminalSeq("end")) |
               (terminalSeq("while") >> expr >> terminalSeq("do") >> block >> terminalSeq("end")) |
               (terminalSeq("repeat") >> block >> terminalSeq("until") >> expr) |
               (terminalSeq("if") >> expr >> terminalSeq("then") >> block >> *(terminalSeq("elseif") >> expr >> terminalSeq("then") >> block) >> -(terminalSeq("else") >> block) >> terminalSeq("end")) | 
               (terminalSeq("for") >> Name >> terminal('=') >> expr >> terminal(',') >> expr >> -(terminal(',') >> expr) >> terminalSeq("do") >> block >> terminalSeq("end")) |
               (terminalSeq("for") >> namelist >> terminalSeq("in") >> explist >> terminalSeq("do") >> block >> terminalSeq("end")) |
               (terminalSeq("function") >> funcname >> funcbody) |
               (terminalSeq("local") >> terminalSeq("function") >> Name >> funcbody) | 
               (terminalSeq("local") >> attnamlist >> -(terminal('-') >> explist));

LuaRule block = *stat >> -retstat;
LuaRule chunk = block;


LuaRule expr1 = (expr1 >> binop >> expr1) | Numeral;

BOOST_AUTO_TEST_CASE(test_start) {
    {
        std::vector<std::string> tests= {
            R"(-)",
            R"(not)",
            R"(#)",
            R"(~)"
        };
        for(const auto& t : tests){
            Context context(t);
            auto start = context.mark();
            BOOST_TEST(unop(context));
            BOOST_CHECK_EQUAL(std::string(start, context.mark()), t);
        }
    }
    {
        std::string input = R"(10+10)";
        Context context(input);
        auto start = context.mark();
        BOOST_TEST(expr(context));
        BOOST_CHECK_EQUAL(std::string(start, context.mark()), input);
    }
    {
        std::string input = R"(a=10+10+10)";
        Context context(input);
        auto start = context.mark();
        BOOST_TEST(chunk(context));
        BOOST_CHECK_EQUAL(std::string(start, context.mark()), input);
    }
}

LuaRule aaaa = terminalSeq("abc");
LuaRule bbbb = terminalSeq("abb") | aaaa;

BOOST_AUTO_TEST_CASE(test_keyword_or_name) {
    {
        std::string input = R"(abc)";
        Context context(input);
        auto start = context.mark();
        BOOST_TEST(bbbb(context));
        BOOST_CHECK_EQUAL(std::string(start, context.mark()), input);
    }
}