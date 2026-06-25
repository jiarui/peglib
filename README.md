# peglib

Header-only Parsing Expression Grammar (PEG) library in C++20, featuring packrat
memoization, left-recursion support, a cut operator for committed choice,
structured error reporting, automatic whitespace skipping, and a post-parse
typed-fold action model for AST construction with unconditional move-safety.

## Status

Core combinators, infrastructure, Grammar API, and automatic whitespace handling
are complete. Typed semantic actions use a **pure two-phase fold model**: `parse`
builds the tree (memoized), `parse_ast` folds it once into owned, move-safe values
— a move-only `NodeType` (e.g. an AST with `unique_ptr` children) composes with
natural by-value action signatures and no `shared_ptr<AstNode>`. The roadmap now
focuses on **full tracing/profiling and parameterized rules** — see [TODO.md](TODO.md).

Application-specific work built on peglib lives in consumer projects.
[yueshi](https://github.com/jiarui/yueshi) is a Lua 5.4 frontend (lexer → typed
AST → evaluator) using peglib as its parsing engine and serves as the primary
real-world case study.

## Features

- **Header-only**, C++20, no external runtime dependencies.
- **Static C++ combinator grammars**: `>>` (sequence), `|` (choice), `*` / `+` /
  `-` / `n*` (repetition / optional), `!` / `&` (negation / lookahead), plus the
  `Grammar` member factories `g.terminal(...)`, `g.terminalSeq(...)`, `g.token(...)`,
  `g.empty()`, `g.cut()` (committed choice), `g.lexeme(...)` (no-skip wrapper).
  Every expression a `Grammar` builds carries that Grammar's `Context` (and thus
  `NodeType`), so the operators compose and assign into rules without any explicit
  Context arguments.
- **Packrat memoization** for linear-time parsing.
- **Left-recursion** support via seed-grow.
- **Cut operator** for Prolog-style committed choice. Cut-committed failures
  throw `peg::ParseError` (a hard error); regular failures are queryable via
  `Context::take_error()`.
- **Semantic actions, typed or untyped**:
  - **Typed (the primary API)**: `auto h = (g["r"] = body); h.set_action([](Context&,
    Span, /*typed child results*/...) {...});` — compile-time-checked against the
    body's derived result type, positional, no tree search. `g.terminal(x)` is
    filtered (void; structural tokens never appear as params); `g.token(x)` keeps
    the matched element (recovered from the span at fold time) so operator identity
    is visible to left-folds. Retrieve results via `g.parse_ast("r", ctx) ->
    std::optional<NodeType>`. Unconditionally move-safe — move-only `NodeType`
    composes (`vector<MoveOnly>`, `optional<MoveOnly>`) with no `shared_ptr`.
  - **Untyped (escape hatch)**: `g["r"].set_action([](Context&, ParseTreeNodePtr)
    {...})` — the action hand-reads the tree and writes `node->value` during parse.
    Retained for side-effect actions (e.g. tokenization); not the value path for
    typed rules.
  No value stack — the fold flows through owned returns.
- **Structured error reporting**: `ExpectedItem` set records what was expected
  at the furthest failure position. `Diagnostic::format()` produces
  `file:line:col: error: expected A or B` messages. A separate multi-diagnostic
  channel (`Context::diagnostics()`) accumulates one diagnostic per recovery
  point so a parser can report many errors per file.
- **Error recovery**: `Rule::set_recovery(spec)` (or `peg::recover(rule, spec)`
  sugar) attaches a sync spec to a rule — on body failure, the rule scans
  forward to the next sync token, records a diagnostic, and resumes. Cut-
  committed failures are not recovered. Helpers: `recover_set`, `recover_eol`,
  `recover_eof`, `recover_predicate`.
- **`SourceMap`**: byte offset ↔ (line, col) mapping, supports both contiguous
  in-memory sources and streaming `FileSource`.
- **Grammar validation**: `undefined_rules()` and `unreachable_rules()` helpers.
- **Automatic whitespace skipping**: `Grammar::set_skipper(rule)` makes a
  transparent rule fire automatically between adjacent sequence children
  and between repetition iterations — no more manual `>> ws >>` threading
  between every pair of terminals. `lexeme(expr)` locally disables
  auto-skip for token bodies (numbers, identifiers, string literals) whose
  characters must stay contiguous. Leading whitespace is consumed at the
  grammar boundary (pest-style); trailing whitespace is the user's choice
  via an explicit `EndOfFile` (`!.`) anchor. Works for any `CharT`
  (`char`, `char32_t`, …).
- **Grammar visualization**: `Grammar::to_dot()` emits a Graphviz DOT
  digraph of rule dependencies (every defined rule is a node, every rule
  reference is an edge, the start rule gets a double border, undefined
  references appear as dangling edge targets for spotting typos). Pipe
  the output through `dot -Tsvg` to render.
- **Concept-constrained**: the `PegContext<C>` concept mirrors the full Context
  API that combinators depend on, and is applied as a constraint on `Grammar`'s
  template parameter — a malformed Context fails fast with a single concept
  diagnostic instead of a deep template error.
- **Pluggable input sources, type-erased**: `Context<CharT, NodeType>` drives
  either an in-memory range (`std::string`, `std::vector`) or a streaming
  `FileSource` — the storage strategy is selected at construction and invisible
  to the template signature. A single `Grammar<char>` can parse a string and a
  file. The contiguous (span) path fills a raw-pointer cache so the
  per-character hot path has zero virtual dispatch; `FileSource` goes through
  one virtual call per character (I/O-bound anyway).
- **Non-char `value_type` is first-class — including downstream tokens**:
  `Context<char32_t>` works for matching and diagnostics, and so does a
  non-trivially-copyable token type (e.g. a lexer `Token` carrying a
  `std::variant`/`std::string` payload). `Grammar<Token, MyAst>` builds rules
  with `g.terminal(tok)`, `g.terminal(pred)`, etc., and the semantic actions
  return `MyAst`. `escape_char_for_expected` / the `to_display` CPO render
  diagnostics for any element type (wider codepoints as `\UNNNNNNNN`, custom
  tokens via an ADL `to_display(const Token&)` hook). (PEG-text compilation
  still produces char-only grammars; a UTF-8 decoder and codepoint-aware `.`
  are Tier 2/3 future work.)
- **`FileSource` is embedded-friendly**: `FileSource<CharT, PageSize>` uses a
  fixed-size `std::array` per buffer page (compile-time `PageSize`, no per-page
  heap allocation) — suitable for freestanding use and with better cache
  locality than the old `std::vector` buffers.

## Quick Start

### A minimal grammar

```cpp
#include "peglib.h"
#include <iostream>

using namespace peg;

int main() {
    Grammar<> g;
    g["number"] = +g.terminal('0', '9');
    g["expr"]   = g["number"] | g["expr"] >> '+' >> g["number"];
    g.set_start("expr");

    std::string input = "1+2+3";
    Context<> ctx{input};
    if (g.parse(ctx)) {
        std::cout << "parsed successfully\n";
    } else {
        if (auto err = ctx.take_error()) {
            SourceMap map{std::string_view{input}};
            std::cerr << err->format(map, "input") << "\n";
        }
    }
}
```

`g.parse_string("1+2+3")` is a convenience that builds the `Context` for you.

### Building a typed AST (the fold model)

```cpp
struct Ast { int value = 0; };
using Ctx = Context<char, Ast>;

Grammar<char, Ast> g;
auto num = (g["num"] = g.terminal('0', '9'));
num.set_action([](Ctx& c, Span sp) -> Ast { return Ast{c.at(sp.start) - '0'}; });

auto add = (g["add"] = g["num"] >> *(g.token('+') >> g["num"]));
add.set_action([](Ctx&, Span, Ast first, std::vector<std::tuple<char, Ast>> rest) -> Ast {
    int acc = first.value;
    for (auto& [ /*op*/, rhs ] : rest) acc += rhs.value;
    return Ast{acc};
});

std::string in = "1+2+3";
Ctx ctx(in);
auto ast = g.parse_ast("add", ctx);   // -> std::optional<Ast>
if (ast) std::cout << ast->value << "\n";   // 6
```

`g.token('+')` keeps the matched element (visible to the action as `char`);
`g.terminal('+')` would be filtered out (void). `parse_ast` runs the post-parse
fold — owned, move-safe values. A move-only `NodeType` composes with no
`shared_ptr`.

### Automatic whitespace skipping

For any grammar where tokens may be separated by spaces, tabs, newlines, or
comments, declare one skipper rule and call `set_skipper` — you no longer
need to thread `>> ws >>` between every pair of terminals:

```cpp
Grammar<> g;

// Whitespace + line comments, both transparent.
g["ws"] = *g.terminal([](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
});
g["number"] = g.lexeme(+g.terminal('0', '9'));   // contiguous digits, no inner ws
g["ident"]  = g.lexeme(g.terminal('a','z') >> *g.terminal('a','z'));
g["expr"]   = g["term"] >> *(g.terminal('+') >> g["term"]);
g["term"]   = g["factor"] >> *(g.terminal('*') >> g["factor"]);
g["factor"] = g["number"] | g["ident"]
            | g.terminal('(') >> g["expr"] >> g.terminal(')');

g.set_start("expr");
g.set_skipper(g["ws"]);                       // one line replaces all the threading

g.parse_string("1 + 2 * ( 3 + 4 )");          // true
g.parse_string("  1+2*3  ");                  // true (pest-style leading ws)
```

`g.lexeme(...)` is the escape hatch: inside it, auto-skip is disabled so a
token's characters stay contiguous (`"12 34"` is two numbers, not `"1234"`).
To disable auto-skip globally, call `clear_skipper()` (or never call
`set_skipper` — that is the default).

### Cut, lexeme, and recovery (C++ API)

Three peglib-specific features beyond the PEG baseline:

**Cut `g.cut()`** — commits the current alternative/repetition scope. After a
cut, failure in the same scope throws `peg::ParseError` (hard failure). Used
inside an ordered choice to express "once we've matched this prefix, we're
committed":

```cpp
g["stmt"] = (g.token('i') >> g.cut() >> cond >> g.token(':') >> g["stmt"])
          | g["expr"];
// Once 'i' matches, this must be an if-statement — don't backtrack into expr.
```

A standalone cut outside any Alternation/Repetition scope is a no-op.

**Lexeme `g.lexeme(...)`** — disables auto-skip for the inner expression, so a
token's characters stay contiguous (`"12 34"` is two numbers, not `"1234"`).

**Recovery `Rule::set_recovery(spec)`** (or `peg::recover(rule, spec)` sugar) —
attaches a sync spec to a rule. On body failure, the rule scans forward to the
next sync token, records a diagnostic at the original failure position, consumes
the sync token, and reports recovered success:

```cpp
g["stmt"].set_recovery(peg::recover_set<char>({';', '}'}), "stmt");
g["block"].set_recovery(peg::recover_eof(), "block");
```

`recover_predicate(fn, label)` accepts an arbitrary sync predicate. Cut-committed
failures are **not** recovered — cut is an explicit commitment that overrides
recovery. Diagnostics accumulate across recovery points via
`Context::diagnostics()`, so a single parse can report many errors:

```cpp
Context<> ctx{input};
if (g.parse(ctx)) {
    for (const auto& d : ctx.diagnostics()) {
        std::cerr << d.format(map, "input") << "\n";   // one per resync
    }
}
```

### Grammar visualization

```cpp
std::cout << g.to_dot();
// then render with:  ./my_parser | dot -Tsvg > grammar.svg
```

## Lifetime & Recursive Rules

peglib's `Grammar<>` is the **sole owner** of all rule entities (`NonTerminal`).
The handle returned by `g["name"]` is a non-owning `Rule` — it stores a bare
pointer to the underlying `NonTerminal` plus the rule's name.

- **Recursive rules work out of the box.** Self-recursion
  (`g["expr"] = g["expr"] >> '+' >> g["number"]`) and mutual recursion
  (`g["A"] = g["B"] | ...; g["B"] = g["A"] | ...`) work without forward
  declarations or macro gymnastics.
- **No shared_ptr cycles.** Because `Rule` is non-owning, recursive grammar
  trees never form reference cycles. `~Grammar()` destroys everything in one
  pass — no runtime cycle-breaking patches, no `weak_ptr`, no manual cleanup.
- **Design constraint: a `Rule` cannot outlive its `Grammar`.** This is
  intentional — it prevents dangling references in recursive grammars and is
  the reason cycles can be eliminated at the source. Don't store a `Rule`
  extracted from one `Grammar` beyond the `Grammar`'s lifetime; build a new
  `Grammar` instead.

If you only need to parse input, `Grammar::parse_string(input)` and
`Grammar::parse(ctx)` are all you need — you never have to name the `Rule`
type yourself.

## `NodeType` — what your actions return

`Grammar<CharT, NodeType>` (default `NodeType = std::monostate`) determines the
type semantic actions return. The library does **not** force a storage policy —
you pick whichever fits:

| `NodeType`                          | Use case                        | Storage cost                |
| ---                                 | ---                             | ---                         |
| `std::monostate` (default)          | pure recognizer (match / no match) | zero (1-byte placeholder) |
| a value type (e.g. `struct IntNode`)| lightweight product             | inline, stack-allocated     |
| a move-only type (e.g. an AST with `unique_ptr` children) | recursive AST | inline, exclusive ownership |
| `std::shared_ptr<T>`                | polymorphic / shared AST        | heap + refcount, nullable   |

The post-parse typed fold owns action results transitively and moves them up
once, so a **move-only `NodeType`** composes naturally — `vector<MoveOnly>`,
`optional<MoveOnly>`, `tuple<char, MoveNode>` all work with by-value action
parameters and no `shared_ptr`. This is the recommended form for a recursive
AST (e.g. yueshi's). `std::shared_ptr<T>` remains available for genuinely
polymorphic or shared-ownership ASTs.

`CharT` and `NodeType` are independent axes: `Grammar<char32_t>` matches UTF-32
codepoints with `NodeType = monostate`; `Grammar<Token, MyAst>` matches a
stream of lexer tokens and produces a custom AST. A token `CharT` may be a
non-trivially-copyable struct (e.g. carrying a `std::string` lexeme):

```cpp
struct Token { int id; std::variant<long long, std::string> lex; /* ... */ };
struct Ast   { int kind; std::string text; };

Grammar<Token, Ast> g;
g["open"]  = g.terminal(Token{1, {}});
g["close"] = g.terminal(Token{2, {}});
auto group = (g["group"] = g["open"] >> *(!g["close"] >> g.terminal([](const Token&) { return true; }))
                            >> g["close"]);
group.set_action([](Context<Token, Ast>& ctx, Span sp, Ast /*open*/, std::vector<Ast> /*body*/, Ast /*close*/) {
    Ast a;
    a.kind = ctx.at(sp.start).id;   // read token payload by offset
    return a;
});
```

## Typed semantic actions

`g["r"] = body` returns a `RuleHandle` carrying the body's static type. Its
`set_action<F>` is **compile-time-checked**: the action receives the body's
sub-results as already-typed arguments, positionally — no hand-searching the
parse tree for children by name.

```cpp
Grammar<char, Ast> g;

// terminal(x)  → void   (filtered; '(' ')' never appear as params)
// token(x)     → char   (kept; the operator identity is visible)
auto paren = (g["paren"] = g.terminal('(') >> g["expr"] >> g.terminal(')'));
paren.set_action([](Context& c, Span sp, Ast inner) -> Ast { return inner; });

// left-fold: the operator is a typed arg
auto mul = (g["mul"] = g["unop"] >> *((g.token('*') >> g["unop"])
                                   | (g.token('/') >> g["unop"])));
mul.set_action([](Context&, Span, Ast first,
                  std::vector<std::tuple<char, Ast>> rest) -> Ast {
    Ast acc = std::move(first);
    for (auto& [op, rhs] : rest) acc = make_binop(op, std::move(acc), std::move(rhs));
    return acc;
});
```

- `Span{start, end}` mirrors the match offsets (char-level → bytes; token-level
  → token indices — read the token via `ctx.at(sp.start)`).
- The action signature follows the body's filtered result type positionally
  (no projection): wrong arity/type is a readable `static_assert` error.
- Retrieve the result via `g.parse_ast("r", ctx)` → `std::optional<NodeType>`.
  `parse_tree` returns structure for introspection (no typed-action values on
  the tree); `parse` returns boolean success.
- `g["r"].set_action(...)` (the untyped `Rule` returned by `g["r"]`) is retained
  as an escape hatch for side-effect actions (it writes `node->value` during
  parse). To get the compile-time-checked typed form, capture the assignment:
  `auto h = (g["r"] = body); h.set_action(...);`

See `CHANGELOG.md` ("Typed semantic actions (pure two-phase fold model)") for
the full design, including why a rule referenced via `g["name"]` is always
`NodeType`-typed and why the fold is unconditionally move-safe.

## Requirements

| Toolchain          | Minimum version |
| ---                | ---             |
| GCC                | 11              |
| Clang              | 14              |
| MSVC               | 19.30 (VS 2022) |
| CMake              | 3.22            |

C++20 is required (`std::span`, `concepts`, `ranges`).

## Build & Test

```sh
git clone <repo-url> peglib
cd peglib
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

### CMake options

| Option                        | Default | Description                              |
| ---                           | ---     | ---                                      |
| `PEGLIB_BUILD_TESTS`          | `ON`    | Build unit tests                         |
| `PEGLIB_COVERAGE`             | `OFF`   | Enable coverage instrumentation (GCC/Clang) |
| `PEGLIB_ENABLE_CLANG_TIDY`    | `OFF`   | Run clang-tidy during build              |
| `PEGLIB_ENABLE_SANITIZERS`    | `OFF`   | Enable ASan/UBSan (GCC/Clang)            |

## Project Layout

```
include/peglib/      header-only library
  peglib.h           umbrella (includes everything)
  Context.h          parsing context (state, memo, cut, error tracking)
  InputSource.h      InputSourceBase polymorphic interface + SpanSource/FileSourceSource adapters
  ParserFwd.h        ScopeGuard, ParsingExprInterface, ParsingExpr, symbolConsumable
  Terminals.h        TerminalExpr, TerminalSeqExpr, TokenExpr, EmptyExpr
  Combinators.h      SequenceExpr, AlternationExpr, Repetition, NotExpr, AndExpr, CutExpr
  NonTerminal.h      NonTerminal (internal entity), Rule (non-owning handle)
  Grammar.h          Grammar (rule container), the primary user-facing API
  Parser.h           umbrella for the 4 parser headers above
  Rule.h             operator DSL (>>, |, *, +, !, &, ...) — factories live on Grammar
  ResultType.h       typed-action model: result_of, the post-parse fold, action_matches
  FileSource.h       streaming file-backed input with double buffering
  SourceMap.h        byte offset <-> (line, col) mapping
  ParseError.h       Diagnostic, ParseError, ExpectedItem, escape helpers
  Concepts.h         PegContext concept
test/                unit tests (doctest)
  *_test.cpp         per-header test cases
  json_test.cpp      JSON grammar example (real-world PEG use case)
  lua.cpp            Lua 5.4 grammar example (real-world PEG use case)
  lua_lex.cpp        Lua 5.4 lexer example
third_party/         vendored doctest (single header)
```

## Testing

Tests use [doctest](https://github.com/doctest/doctest) (vendored in
`third_party/`, no network access required). Test cases are organized
per-header:

- `rule_test.cpp` — operator DSL, recursion, left-recursion
- `parser_test.cpp` — low-level expression and cut-throw semantics
- `context_test.cpp` — context state, position tracking, cut lifecycle,
  release_before integration
- `file_source_test.cpp` — streaming file I/O
- `sourcemap_test.cpp` — byte offset ↔ (line, col) mapping
- `parse_tree_test.cpp` — parse tree structure, rollback on failure
- `error_test.cpp` — error reporting, expected set, Diagnostic format, ParseError
- `typed_action_test.cpp` — the typed two-phase fold model, including the
  move-only-NodeType and alternation-of-tokens regression cases
- `skipper_test.cpp` — auto-skip (`set_skipper` + `lexeme`), all CharT
- `to_dot_test.cpp` — Graphviz DOT output, edge cases, escaping
- `json_test.cpp` — JSON grammar (real-world example of building a complete
  language grammar with the operator DSL)
- `json_skipper_test.cpp` — the same JSON grammar built with `set_skipper`
  instead of manual `>> ws >>` threading
- `lua_lex.cpp`, `lua.cpp` — Lua 5.4 lexer + grammar (another real-world
  example; full Lua interpreter lives in
  [yueshi](https://github.com/jiarui/yueshi))

## Roadmap

The library's PEG-authoring features — automatic whitespace handling (done),
error recovery with cut/lexeme/recovery (done), tracing/profiling (`to_dot()`
slice done) — are in place. The roadmap now focuses on **full
tracing/profiling and parameterized rules**. See [TODO.md](TODO.md) for the
full roadmap.

For a real-world grammar built on peglib, see
[yueshi](https://github.com/jiarui/yueshi) — a Lua 5.4 interpreter.

## License

[Apache License 2.0](LICENSE).
