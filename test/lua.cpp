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
    g["Name"] = terminal('a');
    g["LiteralString"] = terminalSeq("\"hello\"");
    g["Numeral"] = terminalSeq("10");
    g["unop"] = terminal('-') | terminalSeq("not") | terminal('#') | terminal('~');
    g["binop"] = terminalSeq("//") | terminal('+');
    g["fieldsep"] = terminal(',') | terminal(';');
    g["field"] = (terminal('[') >> g["expr"] >> terminal(']') >> terminal('=') >> g["expr"]) |
                 (g["Name"] >> terminal('=') >> g["expr"]) | g["expr"];
    g["fieldlist"] = g["field"] >> *(g["fieldsep"] >> g["field"]) >> -g["fieldsep"];
    g["tableconstructor"] = terminal('{') >> -g["fieldlist"] >> terminal('}');
    g["parlist"] = g["namelist"] >> (-(terminal(',') >> terminalSeq("...")) | terminalSeq("..."));
    g["funcbody"] =
        terminal('(') >> -(g["parlist"]) >> terminal(')') >> g["block"] >> terminalSeq("end");
    g["functiondef"] = terminalSeq("function") >> g["funcbody"];
    g["args"] = (terminal('(') >> -g["explist"] >> terminal(')')) | g["tableconstructor"] |
                g["LiteralString"];
    g["functioncall"] =
        (g["prefixexp"] >> g["args"]) | (g["prefixexp"] >> terminal(':') >> g["Name"] >> g["args"]);
    g["varlist"] = g["var"] >> *(terminal(',') >> g["var"]);
    g["funcname"] = g["Name"] >> *(terminal('.') >> g["Name"]) >> -(terminal(':') >> g["Name"]);
    g["label"] = terminalSeq("::") >> g["Name"] >> terminalSeq("::");
    g["retstat"] = terminalSeq("return") >> -g["explist"] >> terminal(';');
    g["attrib"] = -(terminal('<') >> g["Name"] >> terminal('.'));
    g["attnamlist"] = g["Name"] >> g["attrib"] >> *(terminal(',') >> g["Name"] >> g["attrib"]);
    // Renamed from `stat` to `stat_rule` because Windows ucrt's <sys/stat.h>
    // declares a `stat` function, causing C2373 redefinition errors on MSVC.
    g["stat_rule"] =
        terminal(';') | (g["varlist"] >> terminal('=') >> g["explist"]) | g["functioncall"] |
        g["label"] | terminalSeq("break") | (terminalSeq("goto") >> g["Name"]) |
        (terminalSeq("do") >> g["block"] >> terminalSeq("end")) |
        (terminalSeq("while") >> g["expr"] >> terminalSeq("do") >> g["block"] >>
         terminalSeq("end")) |
        (terminalSeq("repeat") >> g["block"] >> terminalSeq("until") >> g["expr"]) |
        (terminalSeq("if") >> g["expr"] >> terminalSeq("then") >> g["block"] >>
         *(terminalSeq("elseif") >> g["expr"] >> terminalSeq("then") >> g["block"]) >>
         -(terminalSeq("else") >> g["block"]) >> terminalSeq("end")) |
        (terminalSeq("for") >> g["Name"] >> terminal('=') >> g["expr"] >> terminal(',') >>
         g["expr"] >> -(terminal(',') >> g["expr"]) >> terminalSeq("do") >> g["block"] >>
         terminalSeq("end")) |
        (terminalSeq("for") >> g["namelist"] >> terminalSeq("in") >> g["explist"] >>
         terminalSeq("do") >> g["block"] >> terminalSeq("end")) |
        (terminalSeq("function") >> g["funcname"] >> g["funcbody"]) |
        (terminalSeq("local") >> terminalSeq("function") >> g["Name"] >> g["funcbody"]) |
        (terminalSeq("local") >> g["attnamlist"] >> -(terminal('-') >> g["explist"]));

    g["chunk"] = g["block"];

    // Recursive and mutually-recursive rules — order doesn't matter with
    // Grammar (lazy creation + in-place assignment).
    g["expr"] = terminalSeq("nil") | terminalSeq("false") | terminalSeq("true") |
                terminalSeq("...") | g["functiondef"] | g["prefixexp"] | g["tableconstructor"] |
                (g["expr"] >> g["binop"] >> g["expr"]) | (g["unop"] >> g["expr"]) | g["Numeral"] |
                g["LiteralString"];
    g["explist"] = g["expr"] >> *(terminal(',') >> g["expr"]);
    g["namelist"] = g["Name"] >> *(terminal(',') >> g["Name"]);
    g["var"] = g["Name"] | (g["prefixexp"] >> terminal('[') >> g["expr"] >> terminal(']')) |
               (g["prefixexp"] >> terminal('.') >> g["Name"]);
    g["prefixexp"] = g["var"] | g["functioncall"] | (terminal('(') >> g["expr"] >> terminal(')'));
    g["block"] = *g["stat_rule"] >> -g["retstat"];
    g["expr1"] = (g["expr1"] >> g["binop"] >> g["expr1"]) | g["Numeral"];

    g["aaaa"] = terminalSeq("abc");
    g["bbbb"] = terminalSeq("abb") | g["aaaa"];

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
            CHECK(context.substr(start, context.mark() - start) == t);
        }
    }
    {
        std::string input = R"(10+10)";
        Context context(input);
        auto start = context.mark();
        CHECK(g.parse("expr", context));
        CHECK(context.substr(start, context.mark() - start) == input);
    }
    {
        std::string input = R"(a=10+10+10)";
        Context context(input);
        auto start = context.mark();
        CHECK(g.parse(context));
        CHECK(context.substr(start, context.mark() - start) == input);
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
        CHECK(context.substr(start, context.mark() - start) == input);
    }
}
