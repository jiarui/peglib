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

## Phase 1 — Core Infrastructure for Lua (DONE)

- [x] **Error reporting**: furthest-failure position + expected-rule set
- [x] **`ParseError`** (exception type) and **`Diagnostic`** (value object) with
      source location and formatted message (`file:line:col: error: expected ...`)
- [x] **`SourceMap`**: byte offset ↔ (line, col) mapping + `line_view(n)` /
      `line_content(n)` (works for both contiguous and `FileSource`-backed maps)
- [x] **`cut` → hard error**: when a cut-committed branch fails, escalate to a
      thrown `peg::ParseError` instead of returning false
- [x] **Value stack**: `Context` holds a stack of user-defined AST nodes;
      semantic actions push return values (reduce semantics deferred to Phase 3)
- [x] **Concept-constrained `PegContext` trait**: library is AST-agnostic;
      users pass `NodeType` as a template parameter (default `std::monostate`)
- [x] **`FileSource::release_before`**: wired cut-driven buffer release to the
      input source via `is_context_releasable_v` trait
- [x] Per-rule `node_type` typedef via `Context<InputSource, NodeType>` template
- [x] **`Parser.h` split**: `ParserFwd.h` / `Terminals.h` / `Combinators.h` /
      `NonTerminal.h` (umbrella `Parser.h` preserved for backward compatibility)
- [x] **`Macros.h`**: `PEG_RULE` and `PEG_RULE_LABELED` macros for named rules

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
| AST node typing | `Context<InputSource, NodeType>` template | Library stays AST-agnostic; default `std::monostate` keeps backward compatibility |
| Error model | Mixed (cut throws, normal returns false) | Cut-committed failures are programmer errors; regular parse failures are queryable |
| `ParseError` vs `Diagnostic` | `ParseError` (exception, PascalCase) + `Diagnostic` (value object) | Exception type for throws; value object for queries/format |
| SourceMap | O(n) prescan + lazy line re-read for FileSource | Phase 4 1 MB benchmark needs streaming; full load unacceptable |
| Expected set | Hybrid: rule name / label / printable literal | Best error messages with fallback for unnamed rules |
| Pass model | Double-pass (char → token → AST) | Cleaner lexer/parser separation; matches reference Lua |
| Precedence | Explicit layering (14 levels) | PEG left-recursion is precedence-unaware |
| Test framework | doctest (vendored) | Zero deps, fast compile, CI-friendly |
| CI platforms | Linux + Windows only | macOS deferred (cost); platform coverage sufficient |
| Sanitizers | master-only job | Slow (2-3x); not worth blocking every PR |
| Parser.h structure | Split into ParserFwd/Terminals/Combinators/NonTerminal | Reduce compile times; umbrella preserved for backward compat |
| Value stack reduce | Phase 1 only pushes; reduce deferred to Phase 3 | Reduce semantics need AST.h to be designed first to avoid rework |
| `DiagnosticConsumer` | Not in Phase 1 (YAGNI) | Only one diagnostic kind today; revisit when multiple levels needed |
