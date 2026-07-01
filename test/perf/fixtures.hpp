// ---------------------------------------------------------------------------
// Programmatic input fixtures for the peglib benchmark suite.
//
// No .json/.lua files are committed — inputs are generated on the fly so size
// and shape are parametric and the repo stays lean. Each generator returns a
// std::string that is kept alive by the caller for the Context's lifetime
// (Context's SpanSource is non-owning).
//
// Workload coverage:
//   - deeply_nested_json : heavy memo + ParseTreeNode allocation, low
//                          backtracking. Scales with nesting depth N.
//   - wide_json_array    : heavy repetition + node allocation. Scales with
//                          element count N.
//   - dense_arithmetic   : heavy ordered-choice backtracking + per-failure
//                          diagnostic churn. Scales with expression count N.
//   - left_recursive     : exercises the Warth seed-grow loop and the LR-stack
//                          scan. Scales with operand count N.
//   - lua_like_chunk     : a synthetic Lua-like source exercising the
//                          lua_grammar (statements, expressions, function
//                          defs). Scales with statement count N.
// ---------------------------------------------------------------------------
#ifndef PEGLIB_PERF_FIXTURES_HPP
#define PEGLIB_PERF_FIXTURES_HPP

#include <string>
#include <string_view>

namespace peglib_bench::fixtures
{

// `{"k":0}` repeated N times, wrapped once: scales allocation + memo work
// without exploding nesting depth. Approx 9*N bytes.
inline std::string wide_json_array(std::size_t n)
{
    std::string s;
    s.reserve(9 * n + 2);
    s += '[';
    for (std::size_t i = 0; i < n; ++i) {
        if (i != 0)
            s += ',';
        s += "{\"k\":0}";
    }
    s += ']';
    return s;
}

// Left-nested arrays: `[[[...[42]...]]]` with depth N. Each level is a fresh
// (array, position) memo key + a new ParseTreeNode. Approx 2*N bytes.
inline std::string deeply_nested_json(std::size_t depth)
{
    std::string s;
    s.reserve(2 * depth + 2);
    for (std::size_t i = 0; i < depth; ++i)
        s += '[';
    s += "42";
    for (std::size_t i = 0; i < depth; ++i)
        s += ']';
    return s;
}

// `(((a+b)*c)-d)+...` — N operands chained. Heavy ordered-choice /
// backtracking churn and per-failure diagnostic construction under a typical
// expression grammar. Operands cycle a..z. Approx 4*N bytes.
inline std::string dense_arithmetic(std::size_t n_operands)
{
    std::string s;
    s.reserve(4 * n_operands);
    for (std::size_t i = 0; i < n_operands; ++i) {
        if (i != 0) {
            const char ops[] = {'+', '-', '*', '/'};
            s += ops[i % 4];
        }
        s += static_cast<char>('a' + (i % 26));
    }
    return s;
}

// Left-recursive-friendly input: `1+2+3+...+N`. For a grammar `expr = expr
// "+" num / num` this drives the seed-grow loop and the LR-stack scan once
// per operand. Approx 2*N bytes.
inline std::string left_recursive_chain(std::size_t n_operands)
{
    std::string s;
    s.reserve(2 * n_operands);
    for (std::size_t i = 0; i < n_operands; ++i) {
        if (i != 0)
            s += '+';
        s += static_cast<char>('0' + (i % 10));
    }
    return s;
}

// A synthetic Lua-like chunk accepted by the (degenerate) lua_grammar test
// fixture. That grammar has NO skipper and defines Name = 'a', Numeral = "10",
// so input must contain no spaces and use those exact terminals. We emit
// `a=10+10+...;` statements: each is a `varlist '=' explist` stat (exercising
// stat_rule's ordered-choice dispatch down to the assignment branch) followed
// by a `;` stat. The `+10` chain exercises the left-recursive
// `expr = expr binop expr / ...` rule. Approx ~(terms*3+4)*N bytes.
inline std::string lua_like_chunk(std::size_t n_statements, std::size_t terms_per_expr = 4)
{
    std::string s;
    s.reserve((terms_per_expr * 3 + 4) * n_statements + 1);
    for (std::size_t i = 0; i < n_statements; ++i) {
        s += "a=";
        for (std::size_t j = 0; j < terms_per_expr; ++j) {
            if (j != 0)
                s += '+';
            s += "10";
        }
        s += ';';
    }
    return s;
}

} // namespace peglib_bench::fixtures

#endif // PEGLIB_PERF_FIXTURES_HPP
