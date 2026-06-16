# peglib TODO

peglib is a **generic** header-only C++20 PEG library. Application-specific
work (e.g. the Lua 5.4 frontend) lives in consumer projects — see
[yueshi](https://github.com/jiarui/yueshi) for a real-world grammar built on
top of peglib.

## Done

- Core combinators: sequence (`>>`), alternation (`|`), repetition (`*` `+` `-`
  `n*`), negation (`!`), lookahead (`&`), empty, cut
- Packrat memoization (always on for `NonTerminal`, evicted by cut)
- Left-recursion support (seed-grow algorithm, direct/indirect/mutual)
- `FileSource` streaming input with double buffering + cut-driven eviction
- Cut operator with memo release on commitment
- doctest-based test system (vendored, zero external deps)
- CMake build (C++20, INTERFACE target, compile_commands.json)
- clang-format + clang-tidy configs
- CI: GitHub Actions (Linux + Windows, GCC/Clang/MSVC matrix)

## Phase 1 — Core Infrastructure (DONE)

- [x] **Error reporting**: furthest-failure position + expected-rule set
- [x] **`ParseError`** (exception type) and **`Diagnostic`** (value object) with
      source location and formatted message (`file:line:col: error: expected ...`)
- [x] **`SourceMap`**: byte offset ↔ (line, col) mapping + `line_view(n)` /
      `line_content(n)` (works for both contiguous and `FileSource`-backed maps)
- [x] **`cut` → hard error**: when a cut-committed branch fails, escalate to a
      thrown `peg::ParseError` instead of returning false
- [x] **Value stack**: `Context` holds a stack of user-defined AST nodes;
      semantic actions push return values (reduce semantics deferred to a later
      phase / consumer code)
- [x] **Concept-constrained `PegContext` trait**: library is AST-agnostic;
      users pass `NodeType` as a template parameter (default `std::monostate`)
- [x] **`FileSource::release_before`**: wired cut-driven buffer release to the
      input source via `is_context_releasable_v` trait
- [x] Per-rule `node_type` typedef via `Context<InputSource, NodeType>` template
- [x] **`Parser.h` split**: `ParserFwd.h` / `Terminals.h` / `Combinators.h` /
      `NonTerminal.h` (umbrella `Parser.h` preserved for backward compatibility)
- [x] **`Macros.h`**: `PEG_RULE` and `PEG_RULE_LABELED` macros for named rules

## Phase 2.0 — Rule Lifetime Redesign (BLOCKER)

**This is the highest-priority design fix. It blocks all other Phase 2 work.**

### Problem

`Rule<>` is currently a type alias for `NonTerminal<Context>` — users hold the
NonTerminal *entity* directly, not a handle. NonTerminal has two conflicting
roles:

1. **Grammar tree node** — needs a stable `this` pointer for:
   - Packrat memo key (`m_mem[pos][const Rule*]`)
   - Left-recursion seed identity (seed-grow writes to a specific `this`)
   - Semantic action storage (`m_action` lives on the entity)
2. **User-facing value type** — users copy, assign, and pass `Rule<>` by value

Copying a NonTerminal creates a new `this`, breaking memo caching, left-recursion,
and action sharing. To avoid this, `self()` wraps NonTerminal references in
`NonTerminalRef` (a bare `const NonTerminal&`). But `NonTerminalRef` doesn't
extend lifetime — when the referenced NonTerminal goes out of scope (e.g. a
local `Rule<>` in a grammar-construction lambda), the reference dangles,
causing "pure virtual method called" crashes.

### Root Cause

Conflating two concepts into one type:
```
Rule        = user handle (should be cheap to copy, pass-by-value)
NonTerminal = internal node (needs stable identity, shared ownership)
```

### Fix

Make `Rule` a **handle** wrapping `shared_ptr<NonTerminal>`:

- [ ] `Rule` holds `shared_ptr<NonTerminalImpl>`; copy is shallow (shared ownership)
- [ ] Delete `NonTerminalRef` entirely — `Rule` itself IS the reference
- [ ] Delete `self()` — `operator>>`/`|`/etc. handle all operands uniformly
- [ ] Memo key changes from `const Rule*` to `NonTerminalImpl*` (via `get()`)
- [ ] Left-recursion: seed-grow uses the shared NonTerminalImpl, not the Rule copy
- [ ] `Rule::operator=` modifies the underlying NonTerminalImpl (not rebind)
- [ ] `Rule::setAction` / `set_name` / `set_label` delegate to NonTerminalImpl
- [ ] Default-constructed `Rule` creates a new NonTerminalImpl (forward-declared rule)
- [ ] Update all operator overloads in `Rule.h` (no more `self()` dispatch)
- [ ] Update `PEG_RULE` / `PEG_RULE_LABELED` macros
- [ ] All existing tests pass (72 core + 8 Lua + JSON)
- [ ] Local `Rule<>` variables in lambda/function scope work without dangling

### Non-Goals (deferred)

- `Context` ownership of input data (separate issue, deferred)
- Textual grammar format (Phase 2, after this is stable)

## Phase 2 — Textual Grammar Format

The biggest gap separating peglib from feature-complete PEG libraries (yhirose
cpp-peglib, pest, peggy). Once shipped, users can author grammars declaratively
without writing C++ combinators.

- [ ] **PEG-in-PEG self-hosting grammar**: a `peg::grammar` rule that parses PEG
      text into `Rule` objects
- [ ] `Grammar::from_string("Expr <- Term ('+' Term)*")` — compile text into a
      working `Rule` tree at runtime
- [ ] Support the canonical PEG syntax:
      - `Identifier <- Expression` rule definitions
      - Sequence (juxtaposition), `/` ordered choice
      - `*` `+` `?` repetition, `!` `&` predicates
      - `.` any-char, `[a-z]` char class, `'lit'` / `"lit"` literals
      - `(...)` grouping
      - Comments (`# ...` to end of line)
- [ ] Named rules auto-registered into the `Grammar` (queryable, surface in
      error messages)
- [ ] **Grammar validation**: undefined rule references, unreachable rules,
      left-recursion detection (warning, not error — we support it)
- [ ] Hooks for semantic actions per named rule (lambdas attached after
      `from_string`)
- [ ] Round-trip: `Grammar::to_dot()` for visualization

## Phase 3 — Auto Whitespace & Token Boundaries

Eliminate the boilerplate of manually threading `WS` rules through every
production — a feature every comparable PEG library offers.

- [ ] `Context::set_skipper(rule)` — auto-skip whitespace (or any skip rule)
      between adjacent terminals in the grammar
- [ ] `lexeme(rule)` combinator — disable auto-skip within a sub-expression
      (useful for tokens, strings, numbers)
- [ ] **Atomic rules** — opt-in "no skip + no inner backtracking" mode for
      lexical rules (matches pest's `@{}` and yhirose's `< ... >`)
- [ ] Per-context opt-out: `Context::set_skipper(empty)` to disable globally

## Phase 4 — Error Recovery

Production parsers (IDEs, linters, formatters) need to report *many* errors per
file, not just the first one.

- [ ] **Recovery combinator**: `recover(rule, sync_set)` — on `rule` failure,
      skip input until a token in `sync_set` is reached, then continue parsing
- [ ] **Multi-diagnostic reporting**: `Context::diagnostics()` returns a vector
      of `Diagnostic` accumulated across recovery points (currently only the
      furthest-failure is queryable)
- [ ] **Labeled recovery expressions**: `rule^label` — when `rule` fails,
      consult a labeled recovery grammar (matches yhirose's `%recovery` syntax)
- [ ] **Sync token selection helpers**: common patterns (`;`, `}`, EOL, statement
      boundaries)

## Phase 5 — Tracing & Profiling

Grammar bugs are subtle; a tracer is essential during development.

- [ ] **Rule trace callbacks**: `Context::on_rule_enter / on_rule_leave /
      on_rule_fail` — user-supplied callbacks fired per `NonTerminal::parse`
- [ ] **Hit counter**: per-rule invocation count + cache-hit ratio
- [ ] **Time-per-rule profiling**: cumulative wall time per rule, sorted
      descending (helps identify grammar hotspots)
- [ ] **Grammar visualization**: `Grammar::to_dot()` exports a Graphviz DOT
      digraph of rule dependencies (already sketched in Phase 2, finalized here)
- [ ] Optional `PEGLIB_TRACE` macro for verbose stdout logging when building
      test grammars

## Phase 6 — Parameterized Rules & Composition

Reduce grammar duplication.

- [ ] **Rule templates**: `List(Item, Sep)` macro-style — a single definition
      instantiated with different item/separator rules
- [ ] **Grammar imports**: compose a `Grammar` from multiple sources / files
      (`import { list_rule } from "lists.peg"`)
- [ ] **Rule override**: allow downstream grammars to override a rule from an
      imported grammar (inheritance-style composition)
- [ ] **Capture/backreference**: `$name<...>` captures a sub-match, `$name`
      references it later in the same production (regex-style, like yhirose)

## Future (not currently scheduled)

- **Unicode support**: codepoint-aware `.`, `\p{...}` properties, case-folding.
  Currently all matching is byte-oriented (UTF-8 friendly but not semantic).
- **`peglint` CLI**: standalone tool to validate a grammar, lint a source file,
  dump the AST, trace a parse, and emit a profile report.
- **Precedence-climbing operator**: in-grammar `{ precedence L + - }` block
  (yhirose-style) so users don't have to write explicit 14-level layering.
- **Captures / substitution parsing**: `Cs`-style capture (LPEG) for
  transformational parsing (find-and-replace, source-to-source rewrites).

## Open Design Decisions

| Decision | Choice | Rationale |
| ---      | ---    | ---       |
| AST node typing | `Context<InputSource, NodeType>` template | Library stays AST-agnostic; default `std::monostate` keeps backward compatibility |
| Error model | Mixed (cut throws, normal returns false) | Cut-committed failures are programmer errors; regular parse failures are queryable |
| `ParseError` vs `Diagnostic` | `ParseError` (exception, PascalCase) + `Diagnostic` (value object) | Exception type for throws; value object for queries/format |
| SourceMap | O(n) prescan + lazy line re-read for FileSource | Streaming 1 MB+ inputs need lazy evaluation; full load unacceptable |
| Expected set | Hybrid: rule name / label / printable literal | Best error messages with fallback for unnamed rules |
| Memoization | Always on for NonTerminal; evicted by cut | Linear-time guarantee is the main selling point of PEG; cut keeps memory bounded |
| Left-recursion | Always on via seed-grow | No toggle needed; PEG left-recursion is precedence-unaware (documented limitation) |
| Test framework | doctest (vendored) | Zero deps, fast compile, CI-friendly |
| CI platforms | Linux + Windows only | macOS deferred (cost); platform coverage sufficient |
| Sanitizers | master-only job | Slow (2-3x); not worth blocking every PR |
| Parser.h structure | Split into ParserFwd/Terminals/Combinators/NonTerminal | Reduce compile times; umbrella preserved for backward compat |
| Value stack reduce | Phase 1 only pushes; reduce deferred to consumer code | Reduce semantics are application-specific; library should stay generic |
| `DiagnosticConsumer` | Not yet introduced (YAGNI) | Only one diagnostic kind today; revisit when multiple levels / recovery arrive |
| Textual grammar format | Canonical PEG syntax (Ford 2004) + yhirose-style extensions | Maximally familiar to existing PEG users; deviations only where they add clear value |
| Whitespace model | Opt-in `set_skipper` + `lexeme()` escape hatch | pest/yhirose show that auto-skip is the right default; but library users must be able to disable |
| Rule ownership | `Rule` as `shared_ptr` handle (Phase 2.0) | NonTerminal identity must be stable for memo/seed-grow; Rule must be copyable for ergonomics. Shared ownership reconciles both. |
