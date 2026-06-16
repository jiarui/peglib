# peglib TODO

## Done

- Core combinators: sequence (`>>`), alternation (`|`), repetition (`*` `+` `-`
  `n*`), negation (`!`), lookahead (`&`), empty, cut
- Packrat memoization
- Left-recursion support (seed-grow algorithm)
- `FileSource` streaming input with double buffering
- Cut operator with memo release on commitment
- doctest-based test system (vendored, zero external deps)
- CMake build (C++20, INTERFACE target, compile_commands.json)
- clang-format + clang-tidy configs
- CI: GitHub Actions (Linux + Windows, GCC/Clang/MSVC matrix)

## Next Up — Phase 1 (Core Infrastructure for Lua)

- [ ] **Error reporting**: furthest-failure position + expected-rule set
- [ ] **`ParseError`** struct with source location and formatted message
      (`file:line:col: error: expected ...`)
- [ ] **`SourceMap`**: byte offset ↔ (line, col) mapping + `line_view(n)`
- [ ] **`cut` → hard error**: when a cut-committed branch fails, escalate to a
      thrown `parse_error` instead of returning false
- [ ] **Value stack**: `Context` holds a stack of user-defined AST nodes;
      semantic actions pop children and push parents
- [ ] **Concept-constrained `PegContext` trait**: library is AST-agnostic;
      users `typedef node_type` in their Context specialization
- [ ] **`FileSource::release_before`**: wire cut-driven buffer release to the
      input source (currently only memo is released)
- [ ] Per-rule `node_type` typedef (default `std::monostate`)

## Lua Milestone

### Phase 2 — Lexer (double-pass)

- [ ] `Token { TokenID, TokenValue, SourceRange }`
- [ ] Full Lua 5.4 lexing: keywords, names, numerals (hex/fp/exponent),
      short/long strings, short/long comments, all operators
- [ ] Long-bracket matching (`[[ ... ]]`, `[==[ ... ]==]`)
- [ ] Keyword vs name disambiguation via `cut`
- [ ] `TokenStream` API (`peek` / `next` / `expect`)

### Phase 3 — Parser → typed AST

- [ ] `AST.h` with `std::variant`-based strong node types
- [ ] **Explicit precedence layering** (14 priority levels, 2 associativities)
      — NOT pure left recursion (PEG left-recursion is precedence-unaware;
      see Lua 5.4 operator precedence table)
- [ ] Semantic actions build AST nodes from the value stack
- [ ] `ASTPrinter` (S-expression output) for debugging
- [ ] Parse Lua 5.4 official test suite ≥ 95%

### Phase 4 — Validation

- [ ] Error-recovery study on real-world Lua sources
- [ ] Performance benchmark: parse 1 MB Lua file (target: cut release keeps
      memory bounded)

## Future Milestones (not in current scope)

- **M2: Tree-walking evaluator** — `LuaValue` (nil/bool/number/string/table),
  closures, environments, metatables, basic operators
- **M3: Standard library subset** — `print`, `string`, `table`, `math`, `io`,
  `os`, optional `coroutine`
- **M4: Full stdlib + GC** — mark-sweep or ref-count + cycle detection,
  `pcall` error handling, complete official test suite
- **M5: Bytecode VM** — register-based compiler + VM for performance parity
  with reference Lua

## Open Design Decisions

| Decision | Choice | Rationale |
| ---      | ---    | ---       |
| AST node typing | Concept-constrained user trait | Library stays AST-agnostic; users `typedef node_type` |
| Pass model | Double-pass (char → token → AST) | Cleaner lexer/parser separation; matches reference Lua |
| Precedence | Explicit layering (14 levels) | PEG left-recursion is precedence-unaware |
| Test framework | doctest (vendored) | Zero deps, fast compile, CI-friendly |
| CI platforms | Linux + Windows only | macOS deferred (cost); platform coverage sufficient |
| Sanitizers | master-only job | Slow (2-3x); not worth blocking every PR |
