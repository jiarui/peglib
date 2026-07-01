// ---------------------------------------------------------------------------
// peglib benchmark harness.
//
// Measures end-to-end parse time on representative workloads, reported as
// ns/parse and MB/s. NOT a pass/fail test — never registered with ctest. Gated
// behind -DPEGLIB_BUILD_BENCHMARKS=ON so it never perturbs the normal test
// build. Uses std::chrono::steady_clock (no external dependency).
//
// Each workload:
//   1. builds the Grammar once (outside the timed loop — grammar construction
//      is not what we're measuring),
//   2. warms up a few iterations (stabilize caches / branch predictor),
//   3. times a batch of parses, each over a FRESH Context (so memo state and
//      tree allocation are measured per-parse, which is the hot path under
//      optimization),
//   4. asserts the parse succeeded (a failed parse would silently lower the
//      number — guard against it),
//   5. reports min ns/parse and MB/s over the batch.
//
// Output is plain columns for easy before/after diffing:
//
//   workload                            size(B)  iters   ns/parse    MB/s   ok
//   json wide (N=2000)                    18002   200      40123   448.7   1
//   ...
//
// Pass --quick for a fast smoke run (fewer iters). Default is a measurement
// run sized to keep total wall time under ~30s on a modern laptop.
// ---------------------------------------------------------------------------
#include "peglib.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "fixtures.hpp"

using namespace peg;

namespace
{

// The default Context instantiation used by every workload here (char input,
// monostate node type) — matches what Grammar<> builds. All bench code refers
// to this alias rather than the bare Context class template (which has no
// default for CharT, so Context<> is ill-formed).
using Ctx = Context<char>;

// -------------------------------------------------------------------------
// Result reporting.
// -------------------------------------------------------------------------
struct BenchResult
{
    const char* name;
    std::size_t bytes;
    int iters;
    double ns_per_parse;
    double mb_per_s;
    bool ok;
};

void print_header()
{
    std::printf(
        "%-36s %8s %6s %11s %9s %4s\n", "workload", "size(B)", "iters", "ns/parse", "MB/s", "ok");
    std::printf("%-36s %8s %6s %11s %9s %4s\n",
                "------------------------------------",
                "--------",
                "------",
                "-----------",
                "---------",
                "----");
}

void print_result(const BenchResult& r)
{
    std::printf("%-36s %8zu %6d %11.0f %9.1f %4d\n",
                r.name,
                r.bytes,
                r.iters,
                r.ns_per_parse,
                r.mb_per_s,
                r.ok ? 1 : 0);
}

// Run `body(ctx)` `iters` times, each on a fresh Context over `input`, timing
// the total. Returns the per-parse ns (mean over the batch).
template<typename ParseFn>
BenchResult run(const char* name, std::string_view input, int warmup, int iters, ParseFn body)
{
    // Warmup on a throwaway context.
    for (int i = 0; i < warmup; ++i) {
        Ctx ctx{input};
        body(ctx);
    }

    bool all_ok = true;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        Ctx ctx{input};
        if (!body(ctx))
            all_ok = false;
    }
    auto t1 = std::chrono::steady_clock::now();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    double ns_per_parse = (secs / static_cast<double>(iters)) * 1e9;
    double mb_per_s = (static_cast<double>(input.size()) / (1024.0 * 1024.0)) /
                      (secs / static_cast<double>(iters));
    return {name, input.size(), iters, ns_per_parse, mb_per_s, all_ok};
}

// -------------------------------------------------------------------------
// Grammars. Built once per program, outside the timed loop.
// -------------------------------------------------------------------------

// Verbatim copy of the JSON grammar from test/json_test.cpp (manual ws
// threading variant — exercises the core parse path without the skipper).
struct JsonWorkload
{
    Grammar<> g;
    JsonWorkload()
    {
        auto cut_ = g.cut();
        g["ws"] =
            *g.terminal([](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
        auto escape_seq = g.terminal('\\') >> g.terminal([](char c) {
            return c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' || c == 'n' ||
                   c == 'r' || c == 't' || c == 'u';
        });
        auto string_char = g.terminal(
            [](char c) { return c != '"' && c != '\\' && static_cast<unsigned char>(c) >= 0x20; });
        g["string"] = g.terminal('"') >> *(escape_seq | string_char) >> g.terminal('"');
        auto digit = g.terminal('0', '9');
        auto nonzero_digit = g.terminal('1', '9');
        auto sign = g.terminal('-') | g.terminal('+');
        auto integer = (-g.terminal('-')) >> (g.terminal('0') | (nonzero_digit >> *digit));
        auto frac = g.terminal('.') >> +digit;
        auto exp = (g.terminal('e') | 'E') >> -sign >> +digit;
        g["number"] = integer >> -frac >> -exp;
        g["keyword"] = g.terminalSeq("true") | g.terminalSeq("false") | g.terminalSeq("null");
        auto comma_sep = g["ws"] >> g.terminal(',') >> g["ws"];
        g["value_list"] = g["value"] >> *(comma_sep >> g["value"]);
        g["array"] = g.terminal('[') >> g["ws"] >> -g["value_list"] >> g["ws"] >> g.terminal(']');
        g["key_value"] = g["string"] >> g["ws"] >> g.terminal(':') >> g["ws"] >> g["value"];
        g["member_list"] = g["key_value"] >> *(comma_sep >> g["key_value"]);
        g["object"] = g.terminal('{') >> g["ws"] >> -g["member_list"] >> g["ws"] >> g.terminal('}');
        g["value"] = (g["keyword"] >> cut_) | (g["number"] >> cut_) | (g["object"] >> cut_) |
                     (g["array"] >> cut_) | (g["string"] >> cut_);
        g["json"] = g["ws"] >> g["value"] >> g["ws"];
        g.set_start("json");
    }
};

// A compact left-recursive expression grammar for the LR workloads:
//   expr   = expr "+" num / num
//   num    = [0-9]+
// `expr` is direct-left-recursive, exercising the Warth seed-grow loop and
// the lr_in_progress stack scan. Built fresh so it stays independent of the
// lua grammar's expr1.
struct ExprLRWorkload
{
    Grammar<> g;
    ExprLRWorkload()
    {
        auto digit = g.terminal('0', '9');
        g["num"] = +digit;
        g["expr"] = (g["expr"] >> g.terminal('+') >> g["num"]) | g["num"];
        g.set_start("expr");
    }
};

// An arithmetic grammar in the more typical non-left-recursive (PEG) form:
//   expr   = term (("+"|"-") term)*
//   term   = factor (("*"|"/") factor)*
//   factor = [a-z] | "(" expr ")"
// Backtracking here is what stresses the ordered-choice failure path.
struct ArithWorkload
{
    Grammar<> g;
    ArithWorkload()
    {
        g["factor"] = g.terminal([](char c) { return c >= 'a' && c <= 'z'; }) |
                      (g.terminal('(') >> g["expr"] >> g.terminal(')'));
        g["term"] = g["factor"] >> *((g.terminal('*') | g.terminal('/')) >> g["factor"]);
        g["expr"] = g["term"] >> *((g.terminal('+') | g.terminal('-')) >> g["term"]);
        g.set_start("expr");
    }
};

// Verbatim copy of the Lua 5.4 (subset) grammar from test/lua.cpp.
struct LuaWorkload
{
    Grammar<> g;
    LuaWorkload()
    {
        g["Name"] = g.terminal('a');
        g["LiteralString"] = g.terminalSeq("\"hello\"");
        g["Numeral"] = g.terminalSeq("10");
        g["unop"] = g.terminal('-') | g.terminalSeq("not") | g.terminal('#') | g.terminal('~');
        g["binop"] = g.terminalSeq("//") | g.terminal('+');
        g["fieldsep"] = g.terminal(',') | g.terminal(';');
        g["field"] =
            (g.terminal('[') >> g["expr"] >> g.terminal(']') >> g.terminal('=') >> g["expr"]) |
            (g["Name"] >> g.terminal('=') >> g["expr"]) | g["expr"];
        g["fieldlist"] = g["field"] >> *(g["fieldsep"] >> g["field"]) >> -g["fieldsep"];
        g["tableconstructor"] = g.terminal('{') >> -g["fieldlist"] >> g.terminal('}');
        g["parlist"] =
            g["namelist"] >> (-(g.terminal(',') >> g.terminalSeq("...")) | g.terminalSeq("..."));
        g["funcbody"] = g.terminal('(') >> -(g["parlist"]) >> g.terminal(')') >> g["block"] >>
                        g.terminalSeq("end");
        g["functiondef"] = g.terminalSeq("function") >> g["funcbody"];
        g["args"] = (g.terminal('(') >> -g["explist"] >> g.terminal(')')) | g["tableconstructor"] |
                    g["LiteralString"];
        g["functioncall"] = (g["prefixexp"] >> g["args"]) |
                            (g["prefixexp"] >> g.terminal(':') >> g["Name"] >> g["args"]);
        g["varlist"] = g["var"] >> *(g.terminal(',') >> g["var"]);
        g["funcname"] =
            g["Name"] >> *(g.terminal('.') >> g["Name"]) >> -(g.terminal(':') >> g["Name"]);
        g["label"] = g.terminalSeq("::") >> g["Name"] >> g.terminalSeq("::");
        g["retstat"] = g.terminalSeq("return") >> -g["explist"] >> g.terminal(';');
        g["attrib"] = -(g.terminal('<') >> g["Name"] >> g.terminal('.'));
        g["attnamlist"] =
            g["Name"] >> g["attrib"] >> *(g.terminal(',') >> g["Name"] >> g["attrib"]);
        g["stat_rule"] =
            g.terminal(';') | (g["varlist"] >> g.terminal('=') >> g["explist"]) |
            g["functioncall"] | g["label"] | g.terminalSeq("break") |
            (g.terminalSeq("goto") >> g["Name"]) |
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
        g["expr"] = g.terminalSeq("nil") | g.terminalSeq("false") | g.terminalSeq("true") |
                    g.terminalSeq("...") | g["functiondef"] | g["prefixexp"] |
                    g["tableconstructor"] | (g["expr"] >> g["binop"] >> g["expr"]) |
                    (g["unop"] >> g["expr"]) | g["Numeral"] | g["LiteralString"];
        g["explist"] = g["expr"] >> *(g.terminal(',') >> g["expr"]);
        g["namelist"] = g["Name"] >> *(g.terminal(',') >> g["Name"]);
        g["var"] = g["Name"] | (g["prefixexp"] >> g.terminal('[') >> g["expr"] >> g.terminal(']')) |
                   (g["prefixexp"] >> g.terminal('.') >> g["Name"]);
        g["prefixexp"] =
            g["var"] | g["functioncall"] | (g.terminal('(') >> g["expr"] >> g.terminal(')'));
        g["block"] = *g["stat_rule"] >> -g["retstat"];
        g.set_start("chunk");
    }
};

} // namespace

// -------------------------------------------------------------------------
// main.
// -------------------------------------------------------------------------
int main(int argc, char** argv)
{
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0)
            quick = true;
    }

    // Iteration counts. Sized so a default run stays around a few seconds:
    // parse time per iteration dominates the steady_clock granularity, and the
    // total batch keeps the process short. --quick divides by 10 for a smoke
    // check that everything links and parses.
    //
    // json_deep_n is capped well below the recursion ceiling: peglib is a
    // recursive-descent engine and deeply-nested input drives one C++ stack
    // frame per nesting level (array → value → array → …), each level spanning
    // several frames (NonTerminal::parseImpl → parse → Rule::parse →
    // SequenceExpr::parse …). Depth ~2000 parses cleanly under the default 8MB
    // stack; 4000 overflows it (SIGSEGV). 1500 keeps comfortable headroom while
    // still producing enough node/allocation work to measure.
    const int json_wide_n = quick ? 400 : 4000;
    const int json_deep_n = quick ? 300 : 1500;
    const int arith_n = quick ? 500 : 5000;
    const int lr_n = quick ? 500 : 5000;
    const int lua_n = quick ? 200 : 2000;

    const int iters_small = quick ? 10 : 100; // for the larger-input workloads
    const int iters_large = quick ? 30 : 300; // for the smaller-input workloads
    const int warmup = 3;

    print_header();

    // --- JSON: wide array (allocation + memo pressure) ---
    {
        JsonWorkload w;
        auto input = peglib_bench::fixtures::wide_json_array(json_wide_n);
        auto r = run("json wide array", input, warmup, iters_small, [&](Ctx& ctx) {
            return w.g.parse(ctx) && ctx.ended();
        });
        print_result(r);
    }

    // --- JSON: deep nesting (recursion + per-level node) ---
    {
        JsonWorkload w;
        auto input = peglib_bench::fixtures::deeply_nested_json(json_deep_n);
        auto r = run("json deep nest", input, warmup, iters_small, [&](Ctx& ctx) {
            return w.g.parse(ctx) && ctx.ended();
        });
        print_result(r);
    }

    // --- Arithmetic (ordered-choice backtracking / failure churn) ---
    {
        ArithWorkload w;
        auto input = peglib_bench::fixtures::dense_arithmetic(arith_n);
        auto r = run("arith dense (backtrack)", input, warmup, iters_large, [&](Ctx& ctx) {
            return w.g.parse(ctx) && ctx.ended();
        });
        print_result(r);
    }

    // --- Left-recursive (seed-grow loop + LR scan) ---
    {
        ExprLRWorkload w;
        auto input = peglib_bench::fixtures::left_recursive_chain(lr_n);
        auto r = run("expr left-recursive", input, warmup, iters_large, [&](Ctx& ctx) {
            return w.g.parse(ctx) && ctx.ended();
        });
        print_result(r);
    }

    // --- Lua-like chunk (real-world grammar breadth) ---
    {
        LuaWorkload w;
        auto input = peglib_bench::fixtures::lua_like_chunk(lua_n);
        auto r = run("lua chunk", input, warmup, iters_small, [&](Ctx& ctx) {
            return w.g.parse(ctx) && ctx.ended();
        });
        print_result(r);
    }

    return 0;
}
