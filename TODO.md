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

## Phase 2.0 — Rule Lifetime Redesign (DONE)

**This was the highest-priority design fix. It blocked all other Phase 2 work.**

### Problem

`Rule<>` was a type alias for `NonTerminal<Context>` — users held the
NonTerminal *entity* directly, not a handle. NonTerminal had two conflicting
roles:

1. **Grammar tree node** — needs a stable `this` pointer for:
   - Packrat memo key (`m_mem[pos][const NonTerminal*]`)
   - Left-recursion seed identity (seed-grow writes to a specific `this`)
   - Semantic action storage (`m_action` lives on the entity)
2. **User-facing value type** — users copy, assign, and pass `Rule<>` by value

Copying a NonTerminal created a new `this`, breaking memo caching, left-recursion,
and action sharing. To avoid this, `self()` wrapped NonTerminal references in
`NonTerminalRef` (a bare `const NonTerminal&`). But `NonTerminalRef` didn't
extend lifetime — when the referenced NonTerminal went out of scope (e.g. a
local `Rule<>` in a grammar-construction lambda), the reference dangled,
causing "pure virtual method called" crashes.

### Root Cause

Conflating two concepts into one type:
```
Rule        = user handle (should be cheap to copy, pass-by-value)
NonTerminal = internal node (needs stable identity, shared ownership)
```

### Fix

Made `Rule` a **handle** wrapping `shared_ptr<NonTerminal>`:

- [x] `Rule` holds `shared_ptr<NonTerminal>`; copy is shallow (shared ownership)
- [x] Delete `NonTerminalRef` entirely — `Rule` itself IS the reference
- [x] Delete `self()` — `operator>>`/`|`/etc. handle all operands uniformly
- [x] Memo key changes from `const Rule*` to `const NonTerminalType*`
- [x] Left-recursion: seed-grow uses the shared NonTerminal, not the Rule copy
- [x] `Rule::operator=` modifies the underlying NonTerminal in-place (not rebind)
- [x] `Rule::set_action` / `set_name` / `set_label` delegate to NonTerminal
- [x] Default-constructed `Rule` creates a new NonTerminal (forward-declared rule)
- [x] Update all operator overloads in `Rule.h` (no more `self()` dispatch)
- [x] Add `PEG_RULE_DEF` / `PEG_RULE_DEF_LABELED` / `PEG_RULE_RECURSIVE` macros
- [x] All existing tests pass (74 core + 8 Lua + 12 JSON)
- [x] Local `Rule<>` variables in lambda/function scope work without dangling

### Constraint: Self-Referential Copy-Init

`Rule<> r = r >> 'b' | 'a'` (self-referential copy-init) is not supported.
C++ evaluates the initializer expression before constructing `r`; copying
an uninitialized `shared_ptr` is undefined behavior. Use forward-declare +
assign instead:

```cpp
Rule<> r;
r = r >> 'b' | 'a';
// or: PEG_RULE_DEF(Context, r, r >> 'b' | 'a');
```

For namespace-scope recursive rules, wrap assignments in a static initializer
lambda. Non-recursive rules continue to work with copy-init as before.

### Non-Goals (deferred)

- `Context` ownership of input data (separate issue, deferred)
- Textual grammar format (Phase 2, after this is stable)

## Done — shared_ptr cycle leak in recursive grammars (X4 redesign)

**Discovered**: Phase 2 W3 (GrammarCompiler). ASan confirmed.
**Fixed**: Phase 2 W7 via the X4 redesign (non-owning `Rule` handle).

### Problem

Recursive grammars created `shared_ptr` cycles that prevented destruction:

```
NonTerminal → body (shared_ptr<DynExpr>)
            → impl (shared_ptr<RuleRefWrapper>)
            → proxy (RuleProxy)
            → rule (shared_ptr<NonTerminal>)   ← cycle back to start
```

Every `RuleRef` in the AST embedded a `RuleProxy` copy (containing
`shared_ptr<NonTerminal>`) inside the expression tree. For self-referential
or mutually-recursive rules, this formed a cycle. ASan measurement:
100 recursive grammars (`A <- A 'x' / 'y'`) + a static JSON grammar
leaked ~7720 bytes in 20 allocations at process exit.

### Root Cause

The original `Rule` class (Phase 2.0) was a shared_ptr owner wrapper around
`NonTerminal`. Every rule reference inside an expression tree carried an
owning `shared_ptr<NonTerminal>`, so any recursive edge completed a cycle.

### Fix — X4 Redesign (eliminate the cycle at the source)

Replaced three overlapping types (`Rule` as owning handle, `RuleProxy` as
transient handle, `RuleRefWrapper` as DynExpr adapter) with a single
non-owning `Rule` handle:

- `Grammar` is the sole owner of all `NonTerminal`s, held directly as
  `std::map<string, std::shared_ptr<NonTerminal>>`.
- `Rule` (formerly `RuleProxy`) stores a **bare `NonTerminal*`** plus the
  rule name. It is returned by `Grammar::operator[]` and embedded by value
  in expression trees (static `SequenceExpr`/`AlternationExpr` tuples and
  dynamic `DynSequenceExpr`/`DynAlternationExpr` vectors alike).
- `Rule` is itself a `ParsingExpr`, so `GrammarCompiler` can type-erase it
  directly via `make_shared<Rule>(...)` — no `RuleRefWrapper` adapter.
- The design constraint is that **a `Rule` cannot outlive its `Grammar`**.
  This is intentional — it is exactly what eliminates the cycle.

### Verification

- `~Grammar()` is `= default` — no runtime cycle-breaking patches, no
  `weak_ptr`, no manual `clear_body()`.
- `recursive_leak_test.cpp` compiles + destroys 100 recursive grammars
  per subcase (textual-grammar path, static-API path, mutual recursion).
  Under ASan: **0 leaks** (down from ~7720 bytes in the baseline).

- [x] Break the cycle by making `Rule` non-owning (bare `NonTerminal*`).
- [x] Consolidate `Rule` + `RuleProxy` + `RuleRefWrapper` into a single
      `Rule` type.
- [x] Remove standalone `Rule<>` from the public API (Grammar is the sole
      owner; `Rule` handles are non-owning views).
- [x] Regression test under ASan (recursive_leak_test.cpp).

## Phase 2 — Grammar API + Textual Grammar Format (DONE)

The `Grammar<>` class is the primary user-facing API. Rules are defined
via `g["name"] = expr`, auto-named, and support recursive/mutually-recursive
references without forward declarations.

PEG text can be compiled at runtime via `GrammarCompiler::from_string()`.

### Done (Grammar Container)
- [x] `Grammar<Ctx>` class with `operator[]`, `set_start`, `parse`, `parse_string`
- [x] `RuleProxy` for assignment chaining and auto-naming
- [x] Lazy rule creation (forward references work automatically)
- [x] `undefined_rules()` validation helper
- [x] `unreachable_rules()` validation helper (dead-code detection)
- [x] Introspection: `rule_names()`, `has_rule()`, `at()`
- [x] All tests migrated to Grammar API

### Done (Textual Grammar Format)
- [x] **PEG-in-PEG self-hosting grammar**: C++ meta-grammar (MetaGrammar.h)
      parses PEG text into PegAstNode trees
- [x] `GrammarCompiler::from_string("Expr <- Term ('+' Term)*")` — compiles
      PEG text into a working `Grammar<>` at runtime
- [x] Canonical PEG syntax (Ford 2004):
      - `Identifier <- Expression` rule definitions
      - Sequence (juxtaposition), `/` ordered choice
      - `*` `+` `?` repetition, `!` `&` predicates
      - `.` any-char, `[a-z]` char class (ranges, negation, escapes)
      - `'lit'` / `"lit"` literals (with escape sequences)
      - `(...)` grouping
      - Comments (`# ...` to end of line)
- [x] **Grammar validation**: unreachable rules detection, undefined rules
      detection. Left-recursion detection deferred (we support it natively).
- [x] Semantic actions via post-binding: `g["name"].set_action(...)` after
      `from_string`
- [x] Self-hosting: `from_string` can compile `meta/peg.peg` itself

### Architecture Change (post-parse action model)
- `parse()` returns `ParseResult {success, tree}` instead of `bool`
- `ParseTreeNode` carries name, offsets, children, and a NodeType value
- Actions receive `ParseTreeNodePtr` and read `children[i]->value`
- Memo caches full `ParseResult` (tree + value) — no action replay conflict
- Value stack completely removed from Context (6 methods + vector deleted)
- Combinator value-stack rollback removed (tree flows via return values)

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
| Value stack reduce | Eliminated (post-parse action model) | Actions read from ParseTreeNode::children; value stack removed entirely from Context |
| `DiagnosticConsumer` | Not yet introduced (YAGNI) | Only one diagnostic kind today; revisit when multiple levels / recovery arrive |
| Textual grammar format | Canonical PEG syntax (Ford 2004) + `#` comments + `[^...]` negated classes | Maximally familiar to existing PEG users; deviations only where they add clear value |
| Whitespace model | Opt-in `set_skipper` + `lexeme()` escape hatch | pest/yhirose show that auto-skip is the right default; but library users must be able to disable |
| Rule ownership | Grammar sole owner via shared_ptr; Rule is non-owning handle (raw pointer) | X4 design: eliminates shared_ptr cycles at the source. Rule cannot outlive Grammar by design. Consolidates Rule + RuleProxy + RuleRefWrapper into one type. |
| Primary API | `Grammar<Ctx>` container (Phase 2) | Rules belong to a Grammar; auto-naming, lazy creation, and recursive references are handled automatically. Rule is internal. |
| Grammar-Context relationship | Grammar typed to Context, no Context owned (Level 1) | Same Grammar reusable across many parses; fresh Context per parse (fresh memo, position, value stack). |
