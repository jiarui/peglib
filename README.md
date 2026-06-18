# peglib

Header-only Parsing Expression Grammar (PEG) library in C++20, featuring packrat
memoization, left-recursion support, a cut operator for committed choice,
structured error reporting, runtime text-grammar compilation, and a post-parse
tree-based action model for AST construction.

## Status

Core combinators, infrastructure, Grammar API, and textual grammar format are
complete. Phase 2 delivers `GrammarCompiler::from_string()` — compile PEG text
at runtime into a working `Grammar<>`. The roadmap now focuses on **generic PEG
library features**: automatic whitespace skipping, error recovery,
tracing/profiling, and parameterized rules — see [TODO.md](TODO.md).

Application-specific work built on peglib lives in consumer projects.
[yueshi](https://github.com/jiarui/yueshi) is a Lua 5.4 frontend (lexer → typed
AST → evaluator) using peglib as its parsing engine and serves as the primary
real-world case study.

## Features

- **Header-only**, C++20, no external runtime dependencies.
- **Two ways to define grammars**:
  - **C++ combinators**: `>>` (sequence), `|` (choice), `*` / `+` / `-` /
    `n*` (repetition / optional), `!` / `&` (negation / lookahead),
    `cut()` (committed choice).
  - **Textual PEG format**: `GrammarCompiler::from_string("Expr <- Term ('+' Term)*")`
    compiles PEG text at runtime into a `Grammar<>`.
- **Packrat memoization** for linear-time parsing.
- **Left-recursion** support via seed-grow.
- **Cut operator** for Prolog-style committed choice. Cut-committed failures
  throw `peg::ParseError` (a hard error); regular failures are queryable via
  `Context::take_error()`.
- **Post-parse action model**: `parse()` returns `ParseResult {success, tree}`.
  Actions receive a `ParseTreeNodePtr` and read `children[i]->value` to access
  sub-rule results. No value stack — the tree flows through return values.
- **Structured error reporting**: `ExpectedItem` set records what was expected
  at the furthest failure position. `Diagnostic::format()` produces
  `file:line:col: error: expected A or B` messages.
- **`SourceMap`**: byte offset ↔ (line, col) mapping, supports both contiguous
  in-memory sources and streaming `FileSource`.
- **Grammar validation**: `undefined_rules()` and `unreachable_rules()` helpers.
- **Concept-constrained**: the `PegContext<C>` concept mirrors the full Context
  API that combinators depend on, and is applied as a constraint on `Grammar`'s
  template parameter — a custom Context type that's missing a method fails
  fast with a single concept diagnostic instead of a deep template error.
- **Pluggable input sources**: in-memory (`std::string`, `std::vector`) and
  streaming file I/O (`FileSource` with double buffering + cut-driven eviction).
- **Parses its own grammar spec**: the C++ meta-grammar can parse `meta/peg.peg`,
  and `GrammarCompiler` can compile that spec into a working grammar. (A full
  bootstrap-equivalence test — compiled grammar produces the same AST as the
  C++ meta-grammar — is a planned follow-up; today both paths are validated
  independently.)

## Quick Start

### Textual grammar (recommended)

```cpp
#include "peglib.h"
#include <iostream>

using namespace peg;

int main() {
    auto g = GrammarCompiler::from_string(R"(
        Expr   <- Term ('+' Term)*
        Term   <- Factor ('*' Factor)*
        Factor <- [0-9]+ / '(' Expr ')'
    )");

    std::string input = "1+2*3";
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

### C++ combinator grammar

```cpp
Grammar<> g;
g["number"] = +terminal('0', '9');
g["expr"]   = g["number"] | g["expr"] >> '+' >> g["number"];
g.set_start("expr");
g.parse_string("1+2+3");  // convenience: creates a Context internally
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
  ParserFwd.h        ScopeGuard, ParsingExprInterface, ParsingExpr, symbolConsumable
  Terminals.h        TerminalExpr, TerminalSeqExpr, EmptyExpr
  Combinators.h      SequenceExpr, AlternationExpr, Repetition, NotExpr, AndExpr, CutExpr
  NonTerminal.h      NonTerminal (internal entity), Rule (non-owning handle)
  Grammar.h          Grammar (rule container), the primary user-facing API
  Parser.h           umbrella for the 4 parser headers above
  Rule.h             operator DSL (>>, |, *, +, !, &, ...) + factories
  FileSource.h       streaming file-backed input with double buffering
  SourceMap.h        byte offset <-> (line, col) mapping
  ParseError.h       Diagnostic, ParseError, ExpectedItem, escape helpers
  Concepts.h         PegContext concept
  DynExpr.h          type-erased expressions for runtime grammar compilation
  PegAst.h           AST node types for the PEG meta-grammar
  MetaGrammar.h      C++ reference PEG-in-PEG parser (drives GrammarCompiler)
  GrammarCompiler.h  from_string / try_from_string — compile PEG text
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
- `value_stack_test.cpp` — value stack, PegContext concept
- `error_test.cpp` — error reporting, expected set, Diagnostic format, ParseError
- `json_test.cpp` — JSON grammar (real-world example of building a complete
  language grammar with the operator DSL)
- `lua_lex.cpp`, `lua.cpp` — Lua 5.4 lexer + grammar (another real-world
  example; full Lua interpreter lives in
  [yueshi](https://github.com/jiarui/yueshi))

## Roadmap

The library targets generic PEG-authoring features: a textual grammar format
(Phase 2), automatic whitespace handling (Phase 3), error recovery
(Phase 4), tracing/profiling (Phase 5), and parameterized rules (Phase 6).
See [TODO.md](TODO.md) for the full roadmap.

For a real-world grammar built on peglib, see
[yueshi](https://github.com/jiarui/yueshi) — a Lua 5.4 interpreter.

## License

[Apache License 2.0](LICENSE).
