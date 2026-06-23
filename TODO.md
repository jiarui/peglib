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
- **Auto whitespace** (Phase 3): `Grammar::set_skipper` + `lexeme()` —
  transparent rule fires between adjacent sequence children and between
  repetition iterations; pest-style leading whitespace at the grammar
  boundary. `lexeme()` locally disables skip for token bodies.
- **Error recovery** (Phase 4): `Rule::set_recovery(spec)` / `peg::recover`
  sugar + `RecoverSpec` helpers (`recover_set` / `recover_eol` / `recover_eof`
  / `recover_predicate`). Multi-diagnostic channel (`Context::diagnostics()`).
  Cut-committed failures are not recovered. Text-grammar surface: `%recover`.
- **PEG-text extensions** (Phase 4): cut (`~`), lexeme (`< e >`), and
  recovery (`%recover(spec)`) now have textual syntax matching the C++ API.
- **Parse-impl dedup**: the static DSL (`NotExpr`/`AndExpr`/`SequenceExpr`/
  `AlternationExpr`/`RepeatExpr`) and the dynamic path (`DynNotExpr`/
  `DynAndExpr`/`DynSequenceExpr`/`DynAlternationExpr`/`DynRepeatExpr`) now
  share a single `*_parse_impl` body each (`predicate_parse_impl`,
  `sequence_parse_impl`, `choice_parse_impl`, `repeat_parse_impl`). One
  algorithm per combinator, not two.
- **Grammar visualization** (Phase 5 slice): `Grammar::to_dot()` Graphviz
  DOT export of rule dependencies.

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

## Phase 3 — Auto Whitespace & Token Boundaries (DONE)

Eliminate the boilerplate of manually threading `WS` rules through every
production — a feature every comparable PEG library offers.

### Done
- [x] **`Grammar::set_skipper(rule)`** — register a transparent rule that
      fires automatically between adjacent sequence children and between
      repetition iterations. Storage is on Grammar (one non-owning
      `NonTerminal*`); stamped onto Context at `Grammar::parse` entry so
      expressions see it without per-parse setup.
- [x] **`lexeme(rule)` combinator** — disable auto-skip within a
      sub-expression (for tokens, strings, numbers whose characters must
      stay contiguous). Implemented as `LexemeExpr` with exception-safe
      save/restore of `Context::skip_enabled` via `ScopeGuard`.
- [x] **`clear_skipper()` / `has_skipper()`** — disable globally or query
      configuration.
- [x] **Pest-style leading whitespace** consumed at the Grammar::parse
      boundary (so users don't need `ws >>` prefix on their start rule).
      Trailing whitespace is the user's choice via an explicit `EndOfFile`
      (`!.`) anchor.

### Design contract
- **Skip sites**: `SequenceExpr::parseSeq` (Index > 0),
  `DynSequenceExpr::parse` (bool first flag), `repeat_parse_impl`
  (loopCount > 0).
- **Excluded by design**: Alternation (no adjacency), predicates (would
  corrupt zero-width assertions), NonTerminal seed-grow (would break
  left-recursion), TerminalSeqExpr char loop (literals must be contiguous).
- **Reentrancy guard**: while the skipper runs, `skip_enabled` is cleared so
  the skipper's own internals don't recurse into `run_skipper()`. A skipper
  is therefore a single self-contained rule (typically `*e`).
- **CharT generality**: works for any `CharT` (`char`, `char32_t`, …).
- **`GrammarCompiler::from_string`** does NOT auto-inject a skipper rule —
  users call `set_skipper` after compilation.

### Ruled out — Atomic rules (`@{}` / `<...>`)

pest's `@{}` and yhirose's `< ... >` bundle two orthogonal semantics —
no-skip and no-inner-backtracking — into one construct, because pest lacks
independent primitives for either. peglib already has both as separate,
composable combinators:

  - **no-skip** → `lexeme(expr)` (Phase 3)
  - **no-inner-backtrack** → `cut()` (Phase 1)

Users express atomic semantics directly with `lexeme(... >> cut() >> ...)`,
with cut remaining explicit. An `atomic()` sugar that auto-inserts cut
between sequence children would violate the "cut must be a visible,
programmer-authored commitment" contract — a hidden cut silently escalates
a normal parse failure into a thrown `ParseError`, which the user did not
opt into at the failure site. No real consumer demand. The library does
not provide a separate `atomic()` form.

### PEG-text extensions — cut / lexeme / recovery

The textual PEG grammar (`GrammarCompiler::from_string`) supports three
peglib-specific constructs that mirror the C++ combinator API. All three
were added in the same design pass (Phase 4) so they share a coherent
syntax family.

- **Cut `~`** — a standalone primary (leaf). Inside an Alternation or
  Repetition, it commits the current scope: subsequent failure throws
  `ParseError`. Mirrors `cut()` / `CutExpr`. Outside any scope (top-level
  standalone `~`), the cut flag is dropped — `Context::cut(bool)` is a
  no-op on an empty cut stack. No PEG standard exists for cut; `~` follows
  Prolog / pest / Roslyn precedent.
- **Lexeme `< e >`** — disables auto-skip for the inner expression's
  subtree. Mirrors `lexeme()` / `LexemeExpr`. NOTE: with no skipper
  configured (the `GrammarCompiler` default — see Whitespace model row
  above), `< e >` is a **no-op**: it compiles to a `DynLexemeExpr` that
  toggles `Context::skip_enabled`, but that flag is already `false`. The
  plumbing exists so a future `%whitespace` directive can auto-install a
  skipper without changes here. `<` must be disambiguated from `<-`
  (LEFTARROW) via a `!'-'` lookahead in the lexer.
- **Recovery `%recover(spec)`** — definition-level suffix matching
  `Rule::set_recovery`. Three forms: `%recover({';', '}'})` (sync set),
  `%recover(eof)`, `%recover(eol)`. See Phase 4 above.

The C++ API retains strictly more power: `recover_predicate` (arbitrary
sync predicate) has no textual form, since user-defined predicates aren't
expressible in PEG text.

## Phase 4 — Error Recovery (DONE)

Production parsers (IDEs, linters, formatters) need to report *many* errors per
file, not just the first one.

- [x] **Recovery combinator** (C++ API): `Rule::set_recovery(spec)` and the
      `peg::recover(rule, spec)` sugar attach a `RecoverSpec` to a `NonTerminal`.
      On body failure, the rule scans forward to the next sync token, records a
      diagnostic at the original failure position, consumes the sync token, and
      reports recovered success with a transparent null tree. Cut-committed
      failures are NOT recovered — cut is an explicit commitment.
- [x] **`RecoverSpec` helpers**: `recover_set({';', '}'}, label)`,
      `recover_eol(label)`, `recover_eof(label)`, and `recover_predicate(fn, label)`
      cover the common sync patterns. User-defined predicates stay C++-API-only
      (not expressible in PEG text).
- [x] **Multi-diagnostic reporting**: `Context::record_diagnostic` /
      `diagnostics()` / `take_diagnostics()` — a parallel, append-only channel
      alongside the furthest-failure path. Recovery points emit one diagnostic
      per resync so a parser can report many errors per file.
- [x] **Text-grammar surface** (`%recover`): `S <- 'x' %recover({';'})`,
      `%recover(eof)`, `%recover(eol)` — definition-level suffix matching
      `Rule::set_recovery`. The sync spec is encoded into a `Recovery` AST node.
- [ ] **Labeled recovery expressions**: `rule^label` — when `rule` fails,
      consult a labeled recovery grammar. Deferred; the current `%recover`
      directive uses the rule's own name as the diagnostic label, which covers
      the common case.

### Verification

- `recover_test.cpp` (7 cases) — C++ API: basic resync, multi-diagnostic
  accumulation, cut-committed non-recovery, recover_eof, transparency,
  API-form equivalence (`Rule::set_recovery` vs `peg::recover`), predicate
  sync.
- `peg_text_extensions_test.cpp` (13 cases) — text-grammar surface: cut
  commits alternative/repetition, cut-as-primary, lexeme no-op/nested/
  leftarrow-disambiguation, recover set/eof/eol/multi-char/multi-rule,
  cut-committed non-recovery, formatting flexibility.
- `error_test.cpp` — multi-diagnostic channel append/clear/independence.
- `self_parse_test.cpp` — C++ meta-grammar parses the updated `meta/peg.peg`
  (which now documents `~`, `< e >`, `%recover`).
- Full suite: **196 test cases / 35226 assertions pass**, 0 failures.

## Phase 5 — Visualization

Grammar visualization via `to_dot()` is shipped. The remaining tracer items
originally listed here (per-rule trace callbacks, hit counter, time-per-rule
profiling) are **ruled out** — see rationale below.

- [x] **Grammar visualization**: `Grammar::to_dot()` exports a Graphviz DOT
      digraph of rule dependencies (DONE — shipped as a Phase 3 companion
      since `collect_rule_refs` was already in place; ~30 lines reusing the
      existing DFS). Every defined rule is a node (start rule gets
      `peripheries=2`), every rule-reference is an edge, undefined references
      appear as dangling edge targets for typo detection.
- [ ] Optional `PEGLIB_TRACE` macro for verbose stdout logging. Low priority:
      useful only when debugging peglib itself (not a user-facing feature);
      punt until someone actually needs it.

### Ruled out — per-rule tracer callbacks / hit counter / time-per-rule

The yhirose original peglib exposes a `log` callback, and a "PEG library
should have a tracer" was carried over as a roadmap item without re-checking
its value against peglib's actual model. Re-evaluated: **no real user
need in peglib's setting**.

- The post-parse action model already gives users the full `ParseTreeNode`
  tree — they can directly observe which rules succeeded and what AST they
  produced, without an observation hook.
- Phase 1's furthest-failure position + expected-set already pinpoints *where*
  a grammar failed and *what* was expected — the dominant "why did my grammar
  fail" question. A tracer would only re-report the same failures as discrete
  events.
- Per-rule timing is strictly better served by a system profiler (perf /
  VTune / Instruments): finer granularity, no in-process measurement
  perturbation, no instrumentation tax on every parse.
- The one thing a tracer would uniquely provide — packrat cache-hit ratio —
  is niche: PEG memo hit rates are structural, not a tuning knob the user
  pulls, and there is no consumer asking for the number.

In short: the tracer is a vestige of a no-AST PEG library's debugging
story. With `ParseTreeNode` visible and Phase 1 error tracking in place,
it has no equivalent value here.

## Phase 6 — Capture & Backreference

Regex-style capture within a single production: `$name<...>` captures a
sub-match, `$name` references it later. Self-contained — needs a capture
stack on Context (similar in shape to the deleted value stack) but does
not touch the expression virtual hierarchy or the X4 Rule-ownership
model.

- [ ] **Capture/backreference**: `$name<...>` captures a sub-match, `$name`
      references it later in the same production (regex-style, like
      yhirose). The capture stack lives on Context; entries are pushed by
      a new `CaptureExpr` and popped on backtrack via the existing
      state-restore mechanism.

### Ruled out — grammar composition (X4 design conflict)

- ~~**Grammar imports**~~ and ~~**Rule override**~~: not pursued. Both
  require either deep-cloning a `Grammar`'s `NonTerminal`s (which breaks
  the `(pos, NonTerminal*)` memo key and left-recursion seed identity,
  and forces a `collect_rule_refs` pointer-rewrite pass across all 17
  expression types to reconnect the `Rule` handles embedded by value in
  the tree) or shallow-aliasing across `Grammar` objects (which violates
  "Rule cannot outlive its Grammar"). Neither brings new capability:
  multi-source grammar composition is already expressible by
  constructing a single `Grammar` and adding rules from several code
  paths, and text-level file splitting is a trivial `#include`-style
  preprocessing pass over the PEG source (concatenate, then
  `from_string`) — no library support needed. No real consumer demand;
  yhirose's peglib has no imports either.

### Deferred — parameterized rules (ruled out for X4)
- ~~**Rule templates**: `List(Item, Sep)` macro-style — a single definition
      instantiated with different item/separator rules.~~ **Not pursued**:
      conflicts with the non-owning Rule design. Equivalent ergonomics
      available today via user-level C++ helpers (`List(item, sep) =
      item >> *(sep >> item)`); see README "Common patterns". A future
      C++-macro form (`PEG_RULE_TEMPLATE`) that expands at C++ compile time
      (no runtime parameterization) is the only viable path and is not
      currently scheduled.

## Future (not currently scheduled)

- **CharBitmap for char classes**: micro-optimization, low priority. Today
  `GrammarCompiler::compile_charclass` builds `[a-z]` / `[^0-9_]` as a
  `std::set<char>`, and every `TerminalExpr::parse` does a `set.find()` —
  a red-black-tree descent with heap-allocated nodes. For the common case
  (small ASCII character classes), a 256-bit bitmap is strictly better:

  ```cpp
  struct CharBitmap {
      std::array<bool, 256> bits{};
      constexpr void set(char c) { bits[static_cast<unsigned char>(c)] = true; }
      constexpr bool contains(char c) const { return bits[static_cast<unsigned char>(c)]; }
  };
  ```

  Lookup drops from `O(log n)` tree descent to `O(1)` array index;
  construction drops from N heap allocations to a fixed 32-byte stack
  object; the type is `constexpr`-constructible, so static DSL char classes
  could in principle be compile-time constants. `symbolConsumable` already
  has `std::set<elem>` and `std::array<elem, 2>` overloads; a `CharBitmap`
  overload slots in alongside them with no API churn. Not a hot path
  today — defer until profiling shows char-class matching matters. Worth
  recording because the current `std::set<char>` choice is a naive
  implementation detail, not a deliberate design.

- **Bytecode VM execution**: strategically significant, deferred. A bytecode
  execution backend layered alongside the existing tree-walk interpreter.
  The existing `DynExpr` tree is already a type-erased object graph, so a VM
  is a compile pass over it rather than a library rewrite. Not pursued now:
  no consumer needs the non-performance value yet, and the permanent
  dual-track maintenance cost is real (every future semantic change —
  Phase 4 recovery, Phase 6 capture, etc. — must be implemented twice).

  **Seven value dimensions** (most are non-performance; this is *not* a
  performance-only item):

  1. **Grammar persistence** — `compile()` once, `save_bytecode()` to disk,
     `load_bytecode()` on later runs skips the whole setup / textual-grammar
     compile. Matters for start-latency-sensitive consumers (CLI tools, IDE
     plugins, embedded).
  2. **Cross-language bindings** — bytecode is a language-neutral IR; the
     same `.pegbc` can be executed by a C++ / Rust / Python VM. Current
     expression-tree object graph cannot cross language boundaries.
  3. **Untrusted-grammar sandboxing** — VM execution is interruptible and
     budgetable (`max_instructions`, `max_backtracks`, `max_memo_entries`,
     `max_stack_depth`). Tree-walk cannot bound cost; virtual dispatch is
     open-ended. Matters for SaaS parsing services, online playgrounds.
  4. **AOT / JIT precondition** — bytecode is the substrate for ahead-of-time
     codegen and runtime JIT. This is where LPEG's real speed comes from
     (`lpegre.so`), not the VM itself. Expression trees cannot be JIT'd
     because virtual-dispatch targets are unknown until runtime.
  5. **Static analysis & optimization passes** — bytecode is an explicit IR;
     rule reachability, redundant-choice merging, peephole, and
     profile-guided layout all operate on a linear instruction stream, not
     an object graph via visitors.
  6. **Predictable memory budget** — VM stack depth is statically bounded by
     the call graph; memo size is bounded by `input_len × rule_count`;
     bytecode itself is fixed-size. Tree-walk memory is dynamic (shared_ptr
     allocation timing is runtime-dependent). Matters for embedded / real-time.
  7. **Observability & pedagogy** — `disassemble()` produces a readable
     instruction listing. The tree-walk's virtual-dispatch chain is opaque;
     bytecode makes PEG semantics visible (useful for teaching, debugging
     subtle grammar bugs, and library-internal instrumentation).

  Performance (dimension zero) is bounded at **1.5-2.5×**, *not* 5-10×:
  the VM removes virtual dispatch at NonTerminal boundaries and shared_ptr
  refcounting in the expression tree, but memo lookup (still needed for the
  PEG O(n) guarantee) and ParseTreeNode allocation (still needed for
  semantic actions) remain. Packrat memo data-structure optimizations
  (see Open Design Decisions) achieve 2-3× at ~200 lines with zero
  semantic/API risk — for performance alone the VM is not worth it. The
  VM's case rests on dimensions 1-7.

  **API design** (opt-in, current API unchanged):

  ```cpp
  class Grammar {
  public:
    // === Existing API — unchanged ===
    ParseResult parse(input);
    ParseResult parse_tree(input);
    Rule operator[](name);
    void set_start(name);
    void set_skipper(rule);

    // === New VM API — opt-in ===
    void compile();                       // tree → bytecode, throws CompileError
    bool is_compiled() const noexcept;
    ParseResult parse_vm(input);          // VM execution of compiled bytecode
    ParseResult parse_tree_vm(input);

    // Persistence
    void save_bytecode(filename) const;
    static Grammar load_bytecode(filename);   // skips expression-tree construction

    // Debugging / introspection
    std::string disassemble() const;
    std::string disassemble(rule_name) const;
    BytecodeStats bytecode_stats() const;
  };
  ```

  Design choices:

  - **Opt-in layer 2**, not automatic: explicit `compile()` + `parse_vm()`,
    so users choose and can always fall back to `parse()`. Automatic
    silent VM would make VM-only bugs unreproducible via existing code paths.
  - **`compile()` is a separate step**, not implicit: bytecode generation is
    a real cost; users control when to pay it and can cache the result via
    `save_bytecode()`.
  - **`from_string` unchanged** — returns the existing `Grammar` with a
    DynExpr tree; `compile()` then lowers the tree to bytecode. Backward
    compatible.
  - **`set_action` signature unchanged** — actions live in an action table
    indexed by bytecode `ACTION slot` instructions. The VM calls back into
    the same `std::function<NodeType(Context&, NodePtr)>` the user already
    registered; action invocation remains a virtual call. Users pay zero
    migration cost; the VM does not accelerate actions (acceptable — the
    VM's value is dimensions 1-7, not action speedup).
  - **`CompileResult` / `CompileError`** — if the tree contains unsupported
    constructs (e.g. arbitrary functor terminals that cannot be encoded as
    bytecode), `compile()` fails explicitly; `parse_vm()` is unavailable
    but `parse()` continues to work.
  - **Bytecode format is a versioned custom binary**, not JSON/text — tight
    and fast to load; a separate `disassemble()` provides human-readable
    form for debugging.

  **Engineering cost**: ~3000 lines / 2-4 weeks full-time. Breakdown:
  bytecode ISA + builder (~200), tree → bytecode compiler visitor (~400),
  VM interpreter dispatch loop (~700), action-table integration (~100),
  cut / backtrack-frame management (~150), memo with `(pos, rule_id)` key
  (~100), **left-recursion seed-grow on the VM** (~200, the hardest part —
  `CALL` recursion detection + seed iteration in the state machine; academic
  literature skips its interaction with cut and semantic actions, so this
  is near-original work), skipper / lexeme embedding (~100), disassembler
  (~200), tests (~1000).

  **Trigger conditions** (any one): a non-C++ consumer appears (cross-
  language value); start latency becomes a measured problem (persistence);
  untrusted-grammar support is requested (sandbox); peglib's positioning
  explicitly shifts from "C++ header-only library" to "general PEG platform".

  **Permanent cost**: dual-track maintenance. Every future semantic change —
  Phase 4 error recovery, Phase 6 capture/backreference, any new combinator —
  must be implemented in both tree-walk (add expression type) and VM (add
  bytecode instruction + compiler case + interpreter case). The test matrix
  doubles (every test runs under both `parse()` and `parse_vm()`). This
  ongoing cost exceeds the one-time engineering cost and is the primary
  reason to defer until a trigger condition is real.

- **Binary parsing**: non-core goal. The `CharT` template parameter already
  provides an escape hatch — `Context<uint8_t>` switches the whole parse
  stack to byte-level matching, and `terminal(std::array<uint8_t,2>{lo,hi})`
  / `terminal(std::set<uint8_t>{...})` cover byte ranges and classes.
  Multi-byte primitives (endianness-aware `u16le` / `u32be`, varint, bit
  fields) are left to consumer projects as small custom `DynExpr` types —
  the same "library provides orthogonal primitives, domain helpers live in
  consumer code" decision as parameterized rules. For production binary
  parsing, [Kaitai Struct](https://kaitai.io/) is the right tool: it
  generates straight-line C++ with no memo / virtual-dispatch / shared_ptr
  overhead, and ships a large format zoo. peglib's PEG model (backtracking,
  packrat memo, per-match tree allocation) pays for capabilities that
  unambiguous binary formats don't need; peglib is only competitive in
  narrow niches — forensics, polyglot detection, corrupt-file best-effort
  recovery — where ordered choice and predicate backtracking genuinely
  earn their keep.
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
| AST node typing | `Context<CharT, NodeType>` template | Library stays AST-agnostic; default `std::monostate` keeps backward compatibility. CharT and NodeType are orthogonal template params; Source is type-erased behind InputSourceBase (Phase 2 refactor). |
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
| Skipper storage | Grammar-owned, stamped to Context at parse entry | Avoids polluting Context construction; zero overhead when unset (Context's `m_skipper` defaults to nullptr, so `run_skipper()` early-returns before any virtual dispatch). `lexeme()` toggles a separate `skip_enabled` flag with save/restore. |
| Parameterized rules | Ruled out for runtime; C++ helper suffices for compile time | Conflicts with X4 non-owning Rule design — `parse(Context&)` has no parameter slot, and per-instantiation NonTerminals break the `(pos, NonTerminal*)` memo key. `List(item, sep) = item >> *(sep >> item)` as a user-level C++ function delivers the same ergonomics with zero library cost (see README "Common patterns"). |
| Grammar composition (imports / override) | Ruled out (X4 design conflict + no demand) | Deep clone breaks the `(pos, NonTerminal*)` memo key and left-recursion seed identity and requires a `collect_rule_refs` pointer-rewrite pass across all 17 expression types; shallow alias violates "Rule cannot outlive Grammar". Multi-source composition is already expressible by adding rules to a single Grammar from several code paths, and text-level file splitting is a trivial `#include` preprocessing pass (concatenate, then `from_string`). No consumer demand; yhirose's peglib has no imports either. |
| Grammar-Context relationship | Grammar typed to Context, no Context owned (Level 1) | Same Grammar reusable across many parses; fresh Context per parse (fresh memo, position, value stack). |
| Binary parsing support | Ruled out as core goal; `Context<uint8_t>` is the escape hatch | CharT template already provides byte-level matching at zero library cost; multi-byte primitives (u32le, varint, bit fields) belong in consumer code as custom DynExpr types (same precedent as parameterized rules). Kaitai Struct dominates mainstream binary parsing — it generates straight-line C++ with no memo / virtual-dispatch / shared_ptr overhead and ships a large format zoo. peglib's PEG model pays for backtracking + packrat + per-match tree allocation that unambiguous binary formats don't need; only competitive in narrow niches (forensics, polyglot detection, corrupt-file recovery). |
| Static zero-virtual grammar path | Ruled out | A fully static, compile-time-fixed grammar (Spirit X3 model) would eliminate the NonTerminal → body virtual dispatch, but at the cost of the textual-grammar compiler, the runtime `Grammar` API (operator[] assignment, forward references, recursive types), and a large compile-time / template-depth burden — all for ~5-10% hot-path speedup. peglib's differentiation is the runtime API + `from_string`; the static niche is already well-served by Spirit X3. |
| Packrat memo optimization | Analyzed, deferred until a real bottleneck appears | Five feasible, semantics-preserving, API-neutral optimizations identified: (1) `std::map` → `std::unordered_map` / open-addressing hash for both outer (pos) and inner (NonTerminal*) layers; (2) split succeed-memo (needs cached ParseResult) from fail-memo (existence-only, fits a set/bitmap, and fail is the high-frequency case under ordered choice); (3) passthrough skip for rules with no action and a single-NonTerminal body; (4) paged cut-eviction replacing the current O(n) erase_if sweep; (5) intrusive_ptr replacing shared_ptr in RuleState. Combined projected speedup 2-3×. Not pursued now: premature without profiling evidence; all five are mechanical changes that can land in a day once a bottleneck is real. |
| Phase 5 tracer callbacks | Ruled out | The `on_rule_enter` / `on_rule_leave` / `on_rule_fail` callbacks (plus hit counter and per-rule timing) were a vestige of yhirose's no-AST `log` API. In peglib's model the full `ParseTreeNode` tree is already observable post-parse, Phase 1's furthest-failure + expected-set already pinpoints parse failures, and system profilers cover per-rule timing with finer granularity and zero instrumentation tax. The unique capability — packrat cache-hit ratio — is niche and ungovernable (PEG hit rates are structural). See Phase 5 section above. |
| Atomic rules (`@{}` / `<...>`) | Ruled out; `lexeme` + `cut` already express it | pest bundles no-skip + no-inner-backtrack because it lacks independent primitives; peglib has `lexeme` (Phase 3) and `cut` (Phase 1) as orthogonal combinators the user composes directly. An auto-cutting `atomic()` sugar would hide a `ParseError`-throwing commitment inside sequence children, violating the "cut is a visible, programmer-authored commitment" contract. No real consumer demand. |
| Bytecode VM execution | Strategically significant, deferred; opt-in layer-2 API when triggered | Not a performance-only item: it unlocks seven orthogonal value dimensions — grammar persistence (start latency), cross-language bindings, untrusted-grammar sandboxing (budgetable execution), AOT/JIT precondition, static-analysis/optimization passes, predictable memory budget, and observability/pedagogy via `disassemble()`. Performance itself is bounded at 1.5-2.5× (memo lookup and ParseTreeNode allocation remain). Existing API stays unchanged; new `compile()` + `parse_vm()` + `save/load_bytecode()` + `disassemble()` are opt-in. `set_action` signature preserved via an action table indexed by bytecode `ACTION` slots. Engineering cost ~3000 lines / 2-4 weeks; the dominant subtask is left-recursion seed-grow on the VM (no academic coverage of its interaction with cut + actions). Permanent cost is dual-track maintenance (every future semantic change implemented twice). Triggered by: non-C++ consumer, measured start-latency problem, untrusted-grammar request, or an explicit positioning shift to "general PEG platform". Until then, packrat memo data-structure optimizations deliver comparable speedup at far lower cost. |
| ChildContainer Concept (storage-model unification) | Long-term architectural direction, not a Phase 4/6 prerequisite | The static DSL is the first-class citizen; DynExpr exists only to serve `GrammarCompiler::from_string`. Each expression type today has two implementations (static: `std::tuple` storage + compile-time recursive template; dynamic: `std::vector<std::shared_ptr<ParsingExprInterface>>` storage + runtime loop). Introducing a `ChildContainer<Context>` concept (`{ child_count(), parse_child(c, i), collect_child_refs(i, refs) }`) lets `SequenceExpr<C, Container>` take the storage as a template parameter, forces both containers to honor the same interface contract, and lets a single `sequence_parse_impl(Ctx, Container)` instantiate for either. **What it gains**: explicit interface alignment (drift becomes a compile error), Concept-constrained tests that automatically cover both paths, a single parse shell per expression. **What it cannot eliminate**: the two storage models (tuple vs vector is fundamental), the two algorithm bodies (compile-time recursion for inlining vs runtime loop for type-erased children), and therefore the per-new-expression-type dual-track cost that Phase 4/6 will still pay. Static-DSL zero-virtual-dispatch performance must be preserved (the whole point of keeping the static path), so the static container's `parse_child(i)` needs a compile-time dispatch (recursion or jump table over `index_sequence`); the dynamic container's is a vector index. Deferred: the immediate value is interface discipline, not code reduction; Phase 4/6's dominant cost is MetaGrammar + GrammarCompiler extension, not expression-type duplication.

**Alternative considered — `constexpr std::array` instead of `std::tuple`**: evaluated and rejected. A homogeneous container (`std::array<T, N>`) cannot hold heterogeneous children without type-erasing them to a common `T` (shared_ptr<Interface> or variant), which collapses the static DSL back to DynExpr's virtual-dispatch model and destroys the static path's reason to exist. A `std::variant<TerminalExpr, SequenceExpr, ...>` array is a closed set (users cannot add expression types), hits recursive-variant problems (SequenceExpr contains a variant that contains SequenceExpr), and `std::visit` is a jump table anyway. A tuple-with-range-adapter lets `range-for` work but its `parse_at(i)` is still an indirect call because `i` is a runtime value. The divergence between static DSL and DynExpr is not at the storage layer — it is at compile-time-vs-runtime knowability of each child's type, which no container choice can erase. |
