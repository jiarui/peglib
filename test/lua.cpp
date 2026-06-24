#include "peglib.h"

#include "doctest.h"

#include <cstddef>

using namespace peg;

// ---------------------------------------------------------------------------
// Lua 5.4 grammar (subset) — example grammar for peglib.
//
// With the Grammar API, all rules live inside a Grammar object. Recursive
// and mutually-recursive rules work naturally — no forward declarations or
// static-initializer lambdas needed. Rules are auto-named from the map key.
// ---------------------------------------------------------------------------

namespace lua_grammar
{
inline Grammar<> g;

[[maybe_unused]] const bool grammar_initialized = [] {
    g["Name"] = g.terminal('a');
    g["LiteralString"] = g.terminalSeq("\"hello\"");
    g["Numeral"] = g.terminalSeq("10");
    g["unop"] = g.terminal('-') | g.terminalSeq("not") | g.terminal('#') | g.terminal('~');
    g["binop"] = g.terminalSeq("//") | g.terminal('+');
    g["fieldsep"] = g.terminal(',') | g.terminal(';');
    g["field"] = (g.terminal('[') >> g["expr"] >> g.terminal(']') >> g.terminal('=') >> g["expr"]) |
                 (g["Name"] >> g.terminal('=') >> g["expr"]) | g["expr"];
    g["fieldlist"] = g["field"] >> *(g["fieldsep"] >> g["field"]) >> -g["fieldsep"];
    g["tableconstructor"] = g.terminal('{') >> -g["fieldlist"] >> g.terminal('}');
    g["parlist"] =
        g["namelist"] >> (-(g.terminal(',') >> g.terminalSeq("...")) | g.terminalSeq("..."));
    g["funcbody"] =
        g.terminal('(') >> -(g["parlist"]) >> g.terminal(')') >> g["block"] >> g.terminalSeq("end");
    g["functiondef"] = g.terminalSeq("function") >> g["funcbody"];
    g["args"] = (g.terminal('(') >> -g["explist"] >> g.terminal(')')) | g["tableconstructor"] |
                g["LiteralString"];
    g["functioncall"] = (g["prefixexp"] >> g["args"]) |
                        (g["prefixexp"] >> g.terminal(':') >> g["Name"] >> g["args"]);
    g["varlist"] = g["var"] >> *(g.terminal(',') >> g["var"]);
    g["funcname"] = g["Name"] >> *(g.terminal('.') >> g["Name"]) >> -(g.terminal(':') >> g["Name"]);
    g["label"] = g.terminalSeq("::") >> g["Name"] >> g.terminalSeq("::");
    g["retstat"] = g.terminalSeq("return") >> -g["explist"] >> g.terminal(';');
    g["attrib"] = -(g.terminal('<') >> g["Name"] >> g.terminal('.'));
    g["attnamlist"] = g["Name"] >> g["attrib"] >> *(g.terminal(',') >> g["Name"] >> g["attrib"]);
    // Renamed from `stat` to `stat_rule` because Windows ucrt's <sys/stat.h>
    // declares a `stat` function, causing C2373 redefinition errors on MSVC.
    g["stat_rule"] =
        g.terminal(';') | (g["varlist"] >> g.terminal('=') >> g["explist"]) | g["functioncall"] |
        g["label"] | g.terminalSeq("break") | (g.terminalSeq("goto") >> g["Name"]) |
        (g.terminalSeq("do") >> g["block"] >> g.terminalSeq("end")) |
        (g.terminalSeq("while") >> g["expr"] >> g.terminalSeq("do") >> g["block"] >>
         g.terminalSeq("end")) |
        (g.terminalSeq("repeat") >> g["block"] >> g.terminalSeq("until") >> g["expr"]) |
        (g.terminalSeq("if") >> g["expr"] >> g.terminalSeq("then") >> g["block"] >>
         *(g.terminalSeq("elseif") >> g["expr"] >> g.terminalSeq("then") >> g["block"]) >>
         -(g.terminalSeq("else") >> g["block"]) >> g.terminalSeq("end")) |
        (g.terminalSeq("for") >> g["Name"] >> g.terminal('=') >> g["expr"] >> g.terminal(',') >>
         g["expr"] >> -(g.terminal(',') >> g["expr"]) >> g.terminalSeq("do") >> g["block"] >>
         g.terminalSeq("end")) |
        (g.terminalSeq("for") >> g["namelist"] >> g.terminalSeq("in") >> g["explist"] >>
         g.terminalSeq("do") >> g["block"] >> g.terminalSeq("end")) |
        (g.terminalSeq("function") >> g["funcname"] >> g["funcbody"]) |
        (g.terminalSeq("local") >> g.terminalSeq("function") >> g["Name"] >> g["funcbody"]) |
        (g.terminalSeq("local") >> g["attnamlist"] >> -(g.terminal('-') >> g["explist"]));

    g["chunk"] = g["block"];

    // Recursive and mutually-recursive rules — order doesn't matter with
    // Grammar (lazy creation + in-place assignment).
    g["expr"] = g.terminalSeq("nil") | g.terminalSeq("false") | g.terminalSeq("true") |
                g.terminalSeq("...") | g["functiondef"] | g["prefixexp"] | g["tableconstructor"] |
                (g["expr"] >> g["binop"] >> g["expr"]) | (g["unop"] >> g["expr"]) | g["Numeral"] |
                g["LiteralString"];
    g["explist"] = g["expr"] >> *(g.terminal(',') >> g["expr"]);
    g["namelist"] = g["Name"] >> *(g.terminal(',') >> g["Name"]);
    g["var"] = g["Name"] | (g["prefixexp"] >> g.terminal('[') >> g["expr"] >> g.terminal(']')) |
               (g["prefixexp"] >> g.terminal('.') >> g["Name"]);
    g["prefixexp"] =
        g["var"] | g["functioncall"] | (g.terminal('(') >> g["expr"] >> g.terminal(')'));
    g["block"] = *g["stat_rule"] >> -g["retstat"];
    g["expr1"] = (g["expr1"] >> g["binop"] >> g["expr1"]) | g["Numeral"];

    g["aaaa"] = g.terminalSeq("abc");
    g["bbbb"] = g.terminalSeq("abb") | g["aaaa"];

    g.set_start("chunk");
    return true;
}();
} // namespace lua_grammar

TEST_CASE("lua-test-start")
{
    using namespace lua_grammar;
    {
        std::vector<std::string> tests = {R"(-)", R"(not)", R"(#)", R"(~)"};
        for (const auto& t : tests) {
            Context context(t);
            auto start = context.mark();
            CHECK(g.parse("unop", context));
            CHECK(context.input().slice(start, context.mark() - start) == t);
        }
    }
    {
        std::string input = R"(10+10)";
        Context context(input);
        auto start = context.mark();
        CHECK(g.parse("expr", context));
        CHECK(context.input().slice(start, context.mark() - start) == input);
    }
    {
        std::string input = R"(a=10+10+10)";
        Context context(input);
        auto start = context.mark();
        CHECK(g.parse(context));
        CHECK(context.input().slice(start, context.mark() - start) == input);
    }
}

TEST_CASE("lua-keyword-or-name")
{
    using namespace lua_grammar;
    {
        std::string input = R"(abc)";
        Context context(input);
        auto start = context.mark();
        CHECK(g.parse("bbbb", context));
        CHECK(context.input().slice(start, context.mark() - start) == input);
    }
}
