#include "peglib.h"

#include "doctest.h"

#include <cstddef>

using namespace peg;
using LuaRule = Rule<>;

// ---------------------------------------------------------------------------
// Lua 5.4 grammar (subset) — example grammar for peglib.
//
// Rules referenced before their definition (mutually / self-recursive) are
// default-constructed first; their shared_ptr is valid (points to an empty
// NonTerminal). Other rules copy that shared_ptr. The recursive rules are
// assigned later in a static initializer lambda, which modifies the existing
// NonTerminal in-place (*m_impl = rhs) — all copies see the update.
// ---------------------------------------------------------------------------

LuaRule expr;       // self-referential + forward-referenced
LuaRule namelist;   // forward-referenced by parlist
LuaRule block;      // forward-referenced by funcbody, stat_rule
LuaRule explist;    // forward-referenced by args, retstat
LuaRule prefixexp;  // forward-referenced by functioncall
LuaRule var;        // forward-referenced by prefixexp
LuaRule expr1;      // self-referential

LuaRule Name = terminal('a');
LuaRule LiteralString = terminalSeq("\"hello\"");
LuaRule Numeral = terminalSeq("10");
LuaRule unop = terminal('-') | terminalSeq("not") | terminal('#') | terminal('~');
LuaRule binop = terminalSeq("//") | terminal('+');
LuaRule fieldsep = terminal(',') | terminal(';');
LuaRule field = (terminal('[') >> expr >> terminal(']') >> terminal('=') >> expr) |
                (Name >> terminal('=') >> expr) | expr;
LuaRule fieldlist = field >> *(fieldsep >> field) >> -fieldsep;
LuaRule tableconstructor = terminal('{') >> -fieldlist >> terminal('}');
LuaRule parlist = namelist >> (-(terminal(',') >> terminalSeq("...")) | terminalSeq("..."));
LuaRule funcbody = terminal('(') >> -(parlist) >> terminal(')') >> block >> terminalSeq("end");
LuaRule functiondef = terminalSeq("function") >> funcbody;
LuaRule args = (terminal('(') >> -explist >> terminal(')')) | tableconstructor | LiteralString;
LuaRule functioncall = (prefixexp >> args) | (prefixexp >> terminal(':') >> Name >> args);
LuaRule varlist = var >> *(terminal(',') >> var);
LuaRule funcname = Name >> *(terminal('.') >> Name) >> -(terminal(':') >> Name);
LuaRule label = terminalSeq("::") >> Name >> terminalSeq("::");
LuaRule retstat = terminalSeq("return") >> -explist >> terminal(';');
LuaRule attrib = -(terminal('<') >> Name >> terminal('.'));
LuaRule attnamlist = Name >> attrib >> *(terminal(',') >> Name >> attrib);
// Renamed from `stat` to `stat_rule` because Windows ucrt's <sys/stat.h>
// declares a `stat` function, causing C2373 redefinition errors on MSVC.
LuaRule stat_rule =
    terminal(';') | (varlist >> terminal('=') >> explist) | functioncall | label |
    terminalSeq("break") | (terminalSeq("goto") >> Name) |
    (terminalSeq("do") >> block >> terminalSeq("end")) |
    (terminalSeq("while") >> expr >> terminalSeq("do") >> block >> terminalSeq("end")) |
    (terminalSeq("repeat") >> block >> terminalSeq("until") >> expr) |
    (terminalSeq("if") >> expr >> terminalSeq("then") >> block >>
     *(terminalSeq("elseif") >> expr >> terminalSeq("then") >> block) >>
     -(terminalSeq("else") >> block) >> terminalSeq("end")) |
    (terminalSeq("for") >> Name >> terminal('=') >> expr >> terminal(',') >> expr >>
     -(terminal(',') >> expr) >> terminalSeq("do") >> block >> terminalSeq("end")) |
    (terminalSeq("for") >> namelist >> terminalSeq("in") >> explist >> terminalSeq("do") >> block >>
     terminalSeq("end")) |
    (terminalSeq("function") >> funcname >> funcbody) |
    (terminalSeq("local") >> terminalSeq("function") >> Name >> funcbody) |
    (terminalSeq("local") >> attnamlist >> -(terminal('-') >> explist));

LuaRule chunk = block;

// Assign the forward-declared recursive rules. Order does not matter —
// each assignment modifies the NonTerminal in-place, so copies already
// stored in other rules' expression trees resolve correctly at parse time.
[[maybe_unused]] const bool lua_grammar_init = [] {
    expr = terminalSeq("nil") | terminalSeq("false") | terminalSeq("true") |
           terminalSeq("...") | functiondef | prefixexp | tableconstructor |
           (expr >> binop >> expr) | (unop >> expr) | Numeral | LiteralString;
    explist = expr >> *(terminal(',') >> expr);
    namelist = Name >> *(terminal(',') >> Name);
    var = Name | (prefixexp >> terminal('[') >> expr >> terminal(']')) |
          (prefixexp >> terminal('.') >> Name);
    prefixexp = var | functioncall | (terminal('(') >> expr >> terminal(')'));
    block = *stat_rule >> -retstat;
    expr1 = (expr1 >> binop >> expr1) | Numeral;
    return true;
}();

TEST_CASE("lua-test-start")
{
    {
        std::vector<std::string> tests = {R"(-)", R"(not)", R"(#)", R"(~)"};
        for (const auto& t : tests) {
            Context context(t);
            auto start = context.mark();
            CHECK(unop(context));
            CHECK(std::string(start, context.mark()) == t);
        }
    }
    {
        std::string input = R"(10+10)";
        Context context(input);
        auto start = context.mark();
        CHECK(expr(context));
        CHECK(std::string(start, context.mark()) == input);
    }
    {
        std::string input = R"(a=10+10+10)";
        Context context(input);
        auto start = context.mark();
        CHECK(chunk(context));
        CHECK(std::string(start, context.mark()) == input);
    }
}

LuaRule aaaa = terminalSeq("abc");
LuaRule bbbb = terminalSeq("abb") | aaaa;

TEST_CASE("lua-keyword-or-name")
{
    {
        std::string input = R"(abc)";
        Context context(input);
        auto start = context.mark();
        CHECK(bbbb(context));
        CHECK(std::string(start, context.mark()) == input);
    }
}
