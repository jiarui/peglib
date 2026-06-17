# peglib

Header-only Parsing Expression Grammar (PEG) library in C++20, featuring packrat
memoization, left-recursion support, a cut operator for committed choice,
structured error reporting, and a pluggable value stack for AST construction.

## Status

Core combinators and infrastructure are stable and tested. Phase 1 (error
reporting, SourceMap, value stack, concept-constrained Context) is complete.
The roadmap now focuses on **generic PEG library features**: a textual grammar
format, automatic whitespace skipping, error recovery, tracing/profiling, and
parameterized rules — see [TODO.md](TODO.md).

Application-specific work built on peglib lives in consumer projects.
[yueshi](https://github.com/jiarui/yueshi) is a Lua 5.4 frontend (lexer → typed
AST → evaluator) using peglib as its parsing engine and serves as the primary
real-world case study.

## Features

- **Header-only**, C++20, no external runtime dependencies.
- **Natural DSL** via operator overloading: `>>` (sequence), `|` (choice),
  `*` / `+` / `-` / `n*` (repetition / optional), `!` / `&` (negation /
  lookahead), `cut()` (committed choice).
- **Packrat memoization** for linear-time parsing.
- **Left-recursion** support via seed-grow.
- **Cut operator** for Prolog-style committed choice. Cut-committed failures
  throw `peg::ParseError` (a hard error); regular failures are queryable via
  `Context::take_error()`.
- **Structured error reporting**: `ExpectedItem` set records what was expected
  at the furthest failure position. `Diagnostic::format()` produces
  `file:line:col: error: expected A or B` messages.
- **`SourceMap`**: byte offset ↔ (line, col) mapping, supports both contiguous
  in-memory sources and streaming `FileSource`.
- **Value stack**: `Context<InputSource, NodeType>` holds a stack of
  user-defined AST nodes. Semantic actions push return values; reduction is
  deferred to a later phase.
- **Concept-constrained**: `PegContext<C>` concept validates the Context API at
  compile time.
- **Pluggable input sources**: in-memory (`std::string`, `std::vector`) and
  streaming file I/O (`FileSource` with double buffering + cut-driven eviction).

## Quick Start

```cpp
#include "peglib.h"
#include <iostream>
#include <string>

using namespace peg;

int main() {
    // A grammar for comma-separated letters: 'a','b','c'
    const Rule<> letter = terminal('a') | 'b' | 'c';
    const Rule<> list   = letter >> *(',' >> letter);

    std::string input = "a,b,c";
    Context context(input);
    if (list(context) && context.ended()) {
        std::cout << "parsed successfully\n";
    } else {
        // On normal failure: query the diagnostic
        if (auto err = context.take_error()) {
            SourceMap map{std::string_view{input}};
            std::cerr << err->format(map, "input") << "\n";
        }
    }
}
```

### Error reporting with cut

```cpp
// `('a' >> cut >> 'x') | 'a'` on input "ab":
// first alt matches 'a', sets cut, fails on 'x' — cut-committed
// failure throws peg::ParseError instead of falling through to alt 2.
Rule<> g = (terminal('a') >> cut() >> terminal('x')) | terminal('a');
std::string input = "ab";
Context context(input);

try {
    g(context);
} catch (const ParseError& e) {
    SourceMap map{std::string_view{input}};
    std::cerr << e.to_diagnostic().format(map, "file.txt") << "\n";
    // Prints: file.txt:1:2: error: expected 'x'
}
```

### Custom AST node type

```cpp
struct IntNode { int value; };

using MyContext = Context<std::span<const char>, IntNode>;
using MyRule = MyContext::Rule;

MyRule digit = ...; // construct a TerminalExpr<MyContext, ...>
digit.setAction([](MyContext&, MyContext::match_range r) {
    return IntNode{*r.begin() - '0'};
});

std::string input = "42";
MyContext context(input);
digit(context);
digit(context);
// context.node_count() == 2
// context.peek_node().value == 2 (last pushed)
```

### Named rules with `PEG_RULE` macro

```cpp
PEG_RULE(MyContext, numeral, '0'-'9' >> +('0'-'9'));
// equivalent to:
//   MyContext::Rule numeral = ('0'-'9' >> +('0'-'9'));
//   numeral.set_name("numeral");
// "numeral" appears in error messages when the rule fails.
```

### Recursive rules

Self-referential or mutually-recursive rules cannot use copy-initialization
(`Rule<> r = r >> ...`) because the expression is evaluated before `r` is
constructed. Use default-construct + assign instead:

```cpp
// Function scope — direct assignment works:
Rule<> expr;
expr = expr >> '+' >> expr | number;

// Namespace scope — wrap assignments in a static initializer lambda:
Rule<> expr;
Rule<> number = +terminal('0', '9');   // non-recursive: copy-init is fine

const bool init = [] {
    expr = expr >> '+' >> expr | number;  // modifies the existing NonTerminal
    return true;
}();
```

For convenience, `PEG_RULE_DEF` combines forward-declare + assign + name:

```cpp
PEG_RULE_DEF(MyContext, expr, expr >> '+' >> expr | number);
```

Why not `Rule<> r = r >> ...`? C++ evaluates the initializer expression before
constructing `r`. With shared ownership, copying an uninitialized `shared_ptr`
causes a crash. This is a fundamental C++ language constraint — see
[Spirit X3's `BOOST_SPIRIT_DEFINE](https://www.boost.org/doc/libs/release/libs/spirit/doc/x3/html/spirit_x3/tutorials/semantic_actions.html)
for the same pattern in another C++ parser library.

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
  Context.h          parsing context (state, memo, cut, error tracking, value stack)
  ParserFwd.h        ScopeGuard, ParsingExprInterface, ParsingExpr, symbolConsumable
  Terminals.h        TerminalExpr, TerminalSeqExpr, EmptyExpr
  Combinators.h      SequenceExpr, AlternationExpr, Repetition, NotExpr, AndExpr, CutExpr
  NonTerminal.h      NonTerminal (internal node), Rule (shared_ptr handle)
  Parser.h           umbrella for the 4 parser headers above
  Rule.h             operator DSL (>>, |, *, +, !, &, ...) + factories
  FileSource.h       streaming file-backed input with double buffering
  SourceMap.h        byte offset <-> (line, col) mapping
  ParseError.h       Diagnostic, ParseError, ExpectedItem, escape helpers
  Concepts.h         PegContext concept
  Macros.h           PEG_RULE, PEG_RULE_LABELED
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
