# peglib

Header-only Parsing Expression Grammar (PEG) library in C++20, featuring packrat
memoization, left-recursion support, a cut operator for committed choice,
structured error reporting, runtime text-grammar compilation, and a post-parse
tree-based action model for AST construction.

## Status

Core combinators, infrastructure, Grammar API, textual grammar format, and
automatic whitespace handling are complete. Phase 2 delivers
`GrammarCompiler::from_string()` — compile PEG text at runtime into a working
`Grammar<>`. Phase 3 delivers `set_skipper` + `lexeme` (eliminates manual
whitespace threading) plus `Grammar::to_dot()` for Graphviz visualization.
The roadmap now focuses on **error recovery, full tracing/profiling, and
parameterized rules** — see [TODO.md](TODO.md).

Application-specific work built on peglib lives in consumer projects.
[yueshi](https://github.com/jiarui/yueshi) is a Lua 5.4 frontend (lexer → typed
AST → evaluator) using peglib as its parsing engine and serves as the primary
real-world case study.

## Features

- **Header-only**, C++20, no external runtime dependencies.
- **Two ways to define grammars**:
  - **C++ combinators**: `>>` (sequence), `|` (choice), `*` / `+` / `-` /
    `n*` (repetition / optional), `!` / `&` (negation / lookahead),
    `cut()` (committed choice), `lexeme()` (no-skip wrapper).
  - **Textual PEG format**: `GrammarCompiler::from_string("Expr <- Term ('+' Term)*")`
    compiles PEG text at runtime into a `Grammar<>`. Supports the Ford 2004
    baseline plus peglib extensions: `~` (cut), `< e >` (lexeme),
    `%recover({';'})` / `%recover(eof)` / `%recover(eol)` (error recovery).
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
- **Non-char `value_type` is first-class**: `Context<char32_t>` works for
  matching and diagnostics. `escape_char_for_expected` / `escape_string_for_expected`
  are templated; wider codepoints render as `\UNNNNNNNN` instead of being
  truncated to `\xNN`. (PEG-text compilation still produces char-only grammars;
  a UTF-8 decoder and codepoint-aware `.` are Tier 2/3 future work.)
- **`FileSource` is embedded-friendly**: `FileSource<CharT, PageSize>` uses a
  fixed-size `std::array` per buffer page (compile-time `PageSize`, no per-page
  heap allocation) — suitable for freestanding use and with better cache
  locality than the old `std::vector` buffers.
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

### Automatic whitespace skipping

For any grammar where tokens may be separated by spaces, tabs, newlines, or
comments, declare one skipper rule and call `set_skipper` — you no longer
need to thread `>> ws >>` between every pair of terminals:

```cpp
Grammar<> g;

// Whitespace + line comments, both transparent.
g["ws"] = *terminal<char>([](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
});
g["number"] = lexeme(+terminal('0', '9'));   // contiguous digits, no inner ws
g["ident"]  = lexeme(terminal('a','z') >> *terminal('a','z'));
g["expr"]   = g["term"] >> *(terminal('+') >> g["term"]);
g["term"]   = g["factor"] >> *(terminal('*') >> g["factor"]);
g["factor"] = g["number"] | g["ident"]
            | terminal('(') >> g["expr"] >> terminal(')');

g.set_start("expr");
g.set_skipper(g["ws"]);                       // one line replaces all the threading

g.parse_string("1 + 2 * ( 3 + 4 )");          // true
g.parse_string("  1+2*3  ");                  // true (pest-style leading ws)
```

`lexeme(...)` is the escape hatch: inside it, auto-skip is disabled so a
token's characters stay contiguous (`"12 34"` is two numbers, not `"1234"`).
To disable auto-skip globally, call `clear_skipper()` (or never call
`set_skipper` — that is the default).

### Text-grammar extensions: cut, lexeme, recovery

The textual PEG format supports three peglib-specific constructs beyond the
Ford 2004 baseline. They mirror the C++ combinator API (`cut()`, `lexeme()`,
`Rule::set_recovery`) so the two surfaces have identical power for these
features.

**Cut `~`** — commits the current alternative/repetition scope. After a cut,
failure in the same scope throws `peg::ParseError` (hard failure). Used inside
an ordered choice to express "once we've matched this prefix, we're committed":

```peg
# Once 'if' matches, this must be an if-statement — don't backtrack into Stmt
Stmt <- ('if' ~ Cond 'then' Stmt) / Expr / ...
```

A standalone `~` outside any Alternation/Repetition scope is a no-op (the cut
flag is dropped on an empty scope stack).

**Lexeme `< e >`** — disables auto-skip for the inner expression. With no
skipper configured (the `GrammarCompiler` default), this is a no-op; the
plumbing exists so a future `%whitespace` directive can install a skipper
without changing existing grammars. `<` is disambiguated from `<-`
(LEFTARROW) by a `!'-'` lookahead.

```peg
Number <- < [0-9]+ >     # contiguous digits (no-op until %whitespace exists)
```

**Recovery `%recover(spec)`** — a definition-level suffix that attaches a
sync spec to the rule. On body failure, the rule scans forward to the next
sync token, records a diagnostic at the original failure position, consumes
the sync token, and reports recovered success. Three spec forms:

```peg
Stmt   <- Expr ';'  %recover({';', '}'})    # sync on ';' or '}'
Block  <- '{' Stmt* '}' %recover(eof)        # last-ditch: consume to EOF
Line   <- Expr '\n' %recover(eol)            # sync on newline
```

Cut-committed failures are **not** recovered — cut is an explicit programmer
commitment that overrides recovery. Diagnostics accumulate across recovery
points via `Context::diagnostics()`, so a single parse can report many errors:

```cpp
Context<> ctx{input};
if (g.parse(ctx)) {
    for (const auto& d : ctx.diagnostics()) {
        std::cerr << d.format(map, "input") << "\n";   // one per resync
    }
}
```

The C++ API retains strictly more power for recovery: `recover_predicate(fn,
label)` (arbitrary sync predicate) has no textual form, since user-defined
predicates aren't expressible in PEG text.

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

`Grammar<CharT, NodeType>` (default `NodeType = std::monostate`) determines
the type of `ParseTreeNode::value`, which semantic actions populate. The
library does **not** force a storage policy — you pick whichever fits:

| `NodeType`                          | Use case                        | Storage cost                |
| ---                                 | ---                             | ---                         |
| `std::monostate` (default)          | pure recognizer (match / no match) | zero (1-byte placeholder) |
| a value type (e.g. `struct IntNode`)| lightweight product             | inline, stack-allocated     |
| `std::shared_ptr<T>` (e.g. `PegAstNodePtr`) | polymorphic / shared AST | heap + refcount, nullable   |

Passing `std::shared_ptr<PegAstNode>` as the template argument (rather than
`PegAstNode` with the library wrapping it in `shared_ptr` internally) is
deliberate: it lets `std::monostate` and value-type products stay zero-cost,
and lets you choose `unique_ptr`, `boost::intrusive_ptr`, or a custom handle
if you prefer. A `nullptr` action return marks a transparent rule (its node
is kept on the tree for positional stability, but parent actions skip it
when building the user-facing AST). Storage-policy generalization (making
the pointer wrapper itself a template policy parameter) is a tracked future
refactor; today's design trades a slightly verbose `shared_ptr<T>` argument
for full control over the recognizer/value/polymorphic spectrum.

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
- `skipper_test.cpp` — auto-skip (`set_skipper` + `lexeme`), all CharT
- `to_dot_test.cpp` — Graphviz DOT output, edge cases, escaping
- `json_test.cpp` — JSON grammar (real-world example of building a complete
  language grammar with the operator DSL)
- `json_skipper_test.cpp` — the same JSON grammar built with `set_skipper`
  instead of manual `>> ws >>` threading (Phase 3 "after" picture)
- `lua_lex.cpp`, `lua.cpp` — Lua 5.4 lexer + grammar (another real-world
  example; full Lua interpreter lives in
  [yueshi](https://github.com/jiarui/yueshi))

## Roadmap

The library targets generic PEG-authoring features: a textual grammar format
(Phase 2, done), automatic whitespace handling (Phase 3, done), error
recovery (Phase 4, done — including the cut/lexeme/recovery text-grammar
extensions), tracing/profiling (Phase 5, to_dot() slice done), and
parameterized rules (Phase 6). See [TODO.md](TODO.md) for the full roadmap.

For a real-world grammar built on peglib, see
[yueshi](https://github.com/jiarui/yueshi) — a Lua 5.4 interpreter.

## License

[Apache License 2.0](LICENSE).
