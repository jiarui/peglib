# peglib

Header-only Parsing Expression Grammar (PEG) library in C++20, featuring packrat
memoization, left-recursion support, and a cut operator for committed choice.

## Status

Early stage. Core combinators are stable and tested. Error reporting, typed AST
infrastructure, and the Lua reference frontend are under active development —
see [TODO.md](TODO.md) for the roadmap.

## Features

- **Header-only**, C++20, no external runtime dependencies.
- **Natural DSL** via operator overloading: `>>` (sequence), `|` (choice),
  `*` / `+` / `-` / `n*` (repetition / optional), `!` / `&` (negation /
  lookahead), `cut()` (committed choice).
- **Packrat memoization** for linear-time parsing.
- **Left-recursion** support via seed-grow.
- **Cut operator** for Prolog-style committed choice with automatic memo
  release.
- **Pluggable input sources**: in-memory (`std::string`, `std::vector`) and
  streaming file I/O (`FileSource` with double buffering).

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
        std::cout << "parse failed\n";
    }
}
```

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
  Context.h          parsing context (state, memo, cut)
  Parser.h           expression types and parsing engine
  Rule.h             operator DSL (>>, |, *, +, !, &, ...)
  FileSource.h       streaming file-backed input
test/                unit tests (doctest)
  *_test.cpp         per-header test cases
  lua_lex.cpp        Lua 5.4 lexer smoke tests
  lua.cpp            Lua 5.4 grammar smoke tests
third_party/         vendored doctest (single header)
```

## Testing

Tests use [doctest](https://github.com/doctest/doctest) (vendored in
`third_party/`, no network access required). Test cases are organized
per-header:

- `rule_test.cpp` — operator DSL, recursion, left-recursion
- `parser_test.cpp` — low-level expression and cut semantics
- `context_test.cpp` — context state, position tracking, cut lifecycle
- `file_source_test.cpp` — streaming file I/O
- `error_test.cpp` — error reporting (placeholder for Phase 1)

## Roadmap

The long-term goal is a complete Lua 5.4 frontend built on peglib. See
[TODO.md](TODO.md) for the phased roadmap (lexer → typed AST → evaluator).

## License

[Apache License 2.0](LICENSE).
