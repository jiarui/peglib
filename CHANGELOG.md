# Changelog

All notable changes to peglib will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Removed — Dynamic PEG (runtime grammar-text compilation)

The runtime PEG-text layer is removed: `GrammarCompiler`, `DynExpr`, the
bootstrapping `MetaGrammar`, and `PegAst`/`PegAstNode`. peglib is now a
**pure static combinator library** — grammars are built from C++ combinators
(`g["r"] = g.token('+') >> g["x"]`), not parsed from `.peg` text at runtime.

This removes ~2900 lines of a parallel (untyped, tree-walk) expression
hierarchy that no static-grammar user needed, and — more importantly — removes
the static/dynamic coexistence collision that blocked move-only typed actions.
The dynamic path's values were built by Flavor-1 tree-walk (untyped actions
reading `node->children[i]->value` during parse), which fundamentally cannot
interleave with the typed fold (which computes values post-parse). Removing it
lets the typed model collapse to a single, clean two-phase design (below).

- Deleted headers: `DynExpr.h`, `GrammarCompiler.h`, `MetaGrammar.h`,
  `PegAst.h`.
- Deleted tests: `dynexpr_test`, `meta_grammar_test`, `self_parse_test`,
  `from_string_test`, `peg_text_extensions_test`, and the `GrammarCompiler`
  subcases of `skipper_test` / `recursive_leak_test`.
- The untyped `Rule::set_action` hook is **retained** as an escape hatch for
  side-effect actions (e.g. `lua_lex`'s tokenization), but no longer has a
  dynamic-grammar consumer.

### Added — Typed semantic actions (pure two-phase fold model)

Compile-time type-checked semantic actions for the static DSL. The old untyped
`SemanticAction = std::function<NodeType(Context&, ParseTreeNodePtr)>` forced
every action to hand-search the parse tree for sub-results by name
(`find_named`/`pass_through`) — fragile (silent null on typo), O(subtree) per
action, and impossible to reason about positionally. The new typed API derives
each rule's result type from its body at compile time and hands the action
already-typed child results, positionally — no tree search, wrong arity/type is
a hard compile error.

The model is **two-phase and unconditionally move-safe**:

1. **Parse phase** — `parse()` builds the tree (structure + offsets), memoized,
   with `shared_ptr` nodes (required for backtrack-safety). Typed actions do
   **not** run during parse.
2. **Fold phase** — `parse_ast()` walks the final (acyclic) tree **once** and
   builds the AST bottom-up. Each child value is an owned local, moved up to
   the parent exactly once. No value is ever stored at a shared/multi-reader
   location, so a **move-only NodeType** (e.g. an AST with `unique_ptr`
   children, like yueshi's) composes freely — `vector<MoveOnly>`,
   `optional<MoveOnly>`, `tuple<char, MoveNode>` all work, with natural
   by-value action signatures and **no `shared_ptr<AstNode>`** required.

This resolves two defects that blocked yueshi (a move-only AST over a
token-level grammar):
- **Defect 1 (move-only NodeType):** the old extractor `return node->value;`
  copy-removed a move-only type → compile error. The fold owns values
  transitively, so move-only composes. Regression: `typed-action: move-only
  NodeType (Defect 1)`.
- **Defect 2 (alternation-of-tokens):** the old extractor read `node->value`
  (the `NodeType` channel) for alternations, but a `token | token` winner's
  value is a `value_type`. The fold dispatches on the **winning branch's**
  static type via a runtime `alt_winner` index stamped during parse, so there
  is no wrong-channel read. Regression: `typed-action: alternation-of-tokens
  (Defect 2)`.

API surface:
- **`RuleHandle<Context, ExprType>`**: `g["r"] = body` returns a typed handle
  carrying the body's static type. `h.set_action<F>` compile-time-checks that
  `F` is invocable as `F(Context&, Span, Args...)` where `Args` is the body's
  filtered result type, positionally unpacked:
    - void body (`g.terminal`, `g.cut`, predicates)  → `F(Context&, Span)`
    - single result                                  → `F(Context&, Span, T)`
    - multi result                                   → `F(Context&, Span, T0, T1, ...)`
  `set_action` registers the action as a **typed fold** on the producing
  `NonTerminal` (NOT on `m_action`) — the action runs only in the fold.
- **`Span`**: `{ start, end }` mirroring `ParseTreeNode` offsets; char-level
  Context → byte offsets, token-level → token indices (read the token via
  `ctx.at(sp.start)`).
- **`g.terminal(x)` vs `g.token(x)`** (terminal result model):
    - `g.terminal(x)` → `void` (filtered; structural tokens like parentheses
      and keywords never appear as action parameters — no "drop" combinator).
    - `g.token(x)`    → `value_type` (kept; the matched element is recovered
      by the fold from `ctx.at(span.start)` — no value is stashed on the node).
- **`Grammar::parse_ast(rule, ctx) -> std::optional<NodeType>`**: the value
  entry point. Parses then folds. `parse_tree` returns structure for
  introspection/tooling (no typed-action values on the tree); `parse` returns
  boolean success.
- **`result_of<E>`, `fold<E>`, `action_matches`** (`include/peglib/ResultType.h`):
  per-expression result-type derivation, the post-parse fold driver, and the
  arity/type concept.
- `ParseTreeNode::token_value` **removed** — the token's matched element is
  recovered from the span at fold time, not stashed on the node.

#### Migration notes (typed actions)
- `auto h = (g["r"] = body);` then `h.set_action([](Context& c, Span sp, ...) {...});`
- Retrieve the result via `g.parse_ast("r", ctx)` → `std::optional<NodeType>`.
- Structural tokens (parens, keywords, separators): `g.terminal(x)` — they
  won't appear as action parameters.
- Operator/value tokens you need in the action: `g.token(x)`.
- A rule referenced via `g["name"]` is always `node_type`-typed (its body's
  structure is opaque at the reference site), so transparent sub-rules appear
  as positional `NodeType` slots.

### Changed — Token-level grammars with custom NodeType

Support for `Grammar<TokenType, CustomNodeType>` (non-trivially-copyable token
streams feeding a user-defined AST product type), fixing two legacy
`char`-and-`monostate` assumptions.

- **`Context::substr` removed**; input slicing moved to `InputSource`. The old
  `Context::substr` returned `std::basic_string<value_type>`, which is
  ill-formed for a non-trivially-copyable value type (libstdc++ asserts). The
  `PegContext` concept no longer constrains slicing. `Context::input()` exposes
  the `InputSourceBase`, and `InputSourceBase::slice(off, count)` is a
  non-virtual member constrained to `std::is_integral_v<CharT>` — so it does
  not exist for token-level grammars (token actions read payloads via
  `ctx.at(off)`, not via slicing).
- **Free factories removed; `Grammar` member factories added.** `peg::terminal`
  / `terminalSeq` / `empty` / `cut` / `lexeme` hardcoded `Context<elem>`
  (NodeType = `monostate`), so their expressions could not assign into a
  `Rule<Context<CharT, MyNode>>`. These are now `Grammar::terminal` /
  `terminalSeq` / `empty` / `cut` / `lexeme`, which close over the Grammar's
  own `Context` so every expression carries the correct `NodeType` and composes
  freely with `g[...]` handles. The combining operators (`>>`, `|`, `*`, `+`,
  `-`, `!`, `&`, `n*`) remain free functions and deduce `Context` from their
  operands' `context_type`; the mixed-literal forms (`expr >> value`,
  `expr | value`) now build a `TerminalExpr` matching the operand's Context
  instead of calling the removed free `terminal`.
- `MetaGrammar` and `parse_tree_test` no longer hand-write
  `TerminalExpr<Ctx, ...>` aliases — they use `g.terminal(...)`.

### Migration notes

- `terminal(x)` → `g.terminal(x)`; `terminalSeq(...)` → `g.terminalSeq(...)`;
  `empty()` → `g.empty()`; `cut()` → `g.cut()`; `lexeme(e)` → `g.lexeme(e)`.
- `ctx.substr(off, len)` → `ctx.input().slice(off, len)` (character grammars
  only; token grammars have no equivalent and do not need one).
- `terminal<CharT>(pred)` → `g.terminal(pred)` (drop the explicit template
  argument — the Grammar already knows `CharT`).

### Added — Phase 3 (Auto Whitespace) + Phase 5 to_dot() slice

#### Auto whitespace (`set_skipper` + `lexeme`)
- **`Grammar::set_skipper(rule)`**: register a transparent rule that fires
  automatically between adjacent sequence children and between repetition
  iterations — eliminates the manual `>> ws >>` threading that every
  whitespace-tolerant grammar previously needed. Storage is on the Grammar
  (one non-owning `NonTerminal*`); `Grammar::parse` stamps it onto the
  Context at entry so expressions see it without any per-parse setup.
- **`Grammar::clear_skipper()` / `has_skipper()`**: disable auto-skip
  globally or query whether a skipper is configured.
- **`lexeme(expr)` combinator** (`include/peglib/Combinators.h`,
  `LexemeExpr`): locally disable auto-skip for a sub-expression's subtree.
  Token bodies (numbers, identifiers, string literals) whose characters must
  stay contiguous are wrapped in `lexeme(...)`. Implemented as an
  exception-safe save/restore of `Context::skip_enabled` via `ScopeGuard`,
  so nested `lexeme(lexeme(...))` is a no-op-on-the-flag.
- **`ParsingExpr::context_type` trait**: enables `peg::lexeme(expr)` to
  deduce the Context type from any expression object, so users write
  `lexeme(+terminal(...))` with no explicit template arguments.
- **`Context::run_skipper()` / `internal_set_skipper()` / `skip_enabled()` /
  `has_skipper()`**: internal hooks (public only because the expression types
  live in `peg::parsers`). End users drive skip via `Grammar::set_skipper` /
  `lexeme`.
- **Reentrancy guard**: while the skipper itself runs, `skip_enabled` is
  temporarily cleared so the skipper's own internal Repetition/Sequence
  children do not recursively invoke `run_skipper()` (which would
  double-consume input). A skipper is therefore a single self-contained rule
  (typically `*e`) and cannot rely on auto-skip itself.
- **Pest-style leading whitespace**: `Grammar::parse` and `parse_tree`
  invoke `ctx.run_skipper()` once before the start rule, so leading
  whitespace is consumed at the grammar boundary without an explicit `ws >>`
  prefix in the start rule. Trailing whitespace is intentionally NOT consumed
  (parse_string is partial-match; users wanting full consumption append
  `EndOfFile` (`!.`) to the start rule).
- New test files: `skipper_test.cpp` (14 cases), `json_skipper_test.cpp`
  (6 cases, a real-world JSON grammar built with `set_skipper` vs the
  manual threading of `json_test.cpp`).

#### Grammar visualization (Phase 5 to_dot() slice)
- **`Grammar::to_dot()`**: emit a Graphviz DOT digraph of rule dependencies.
  Every defined rule is a node (start rule gets `peripheries=2`); every
  rule reference (via `collect_rule_refs`) is a directed edge; undefined
  references appear as dangling edge targets for spotting typos.
  Implementation reuses the existing DFS / `collect_rule_refs` visitor, so
  no new virtual is introduced. Suitable for piping through `dot -Tsvg`.
- New test file: `to_dot_test.cpp` (11 cases — digraph structure, start-rule
  marking, recursive self-loops, typo detection, DOT-special character
  escaping, empty/unset-start robustness).

#### Skip sites wired (design contract)
- **`SequenceExpr::parseSeq<Index>`**: skip when `Index > 0` (between
  adjacent tuple children; never before the first).
- **`DynSequenceExpr::parse`**: skip between adjacent vector children via a
  `bool first` flag (mirrors the static path).
- **`repeat_parse_impl`**: skip when `loopCount > 0` (between iterations;
  never before the first). The zero-width termination guard is preserved.
- **Excluded by design** (no adjacency): `AlternationExpr`,
  `DynAlternationExpr`, predicates (`NotExpr`/`AndExpr`), NonTerminal
  seed-grow, and `TerminalSeqExpr`'s internal char loop.

### Changed — source erasure + FixSizeBuffer + Tier 1 char (6-phase refactor)

A structural refactor that decouples the storage strategy from the Context
type, makes FileSource embedded-friendly, and clears the char debt so a
non-char `value_type` is first-class. Each phase is an independently
revertable commit.

#### Phase 2 — Source erasure: `Context<CharT, NodeType>`
- **`Context` is now `Context<CharT, NodeType = std::monostate>`** (was
  `Context<InputSource, NodeType>`). The storage strategy (contiguous span
  vs paged FileSource) is type-erased behind `InputSourceBase<CharT>`,
  selected at construction, invisible to the template signature.
- **A single `Grammar<char>` can now drive either a string or a file.**
  Previously `Grammar<>` (span-backed) could not parse a
  `Context<FileSource<char>>` — the type mismatch was structural. This
  friction is gone.
- **`Context::get_input()` is removed.** Use `ctx.substr(off, count)` and
  `ctx.at(off)` (offset-based slice access that works for both span and
  FileSource sources). Meta-grammar actions and all tests migrated.
- **`SpanSource` fills a raw-pointer cache** (`m_fast_data`) at construction;
  the per-character hot path (`current()` / `at()`) indexes it directly with
  zero virtual dispatch for span-backed inputs. FileSource-backed parses go
  through one virtual call per character (I/O-bound anyway).

#### Phase 3 — FixSizeBuffer: `FileSource<CharT, PageSize>`
- **`FileSource` is now `FileSource<CharT, PageSize>`** — each of the two
  buffer pages is a fixed-size `std::array<CharT, PageSize>` instead of a
  `std::vector` that `resize()`d on every read. This drops the per-page heap
  allocation, makes FileSource suitable for embedded/freestanding use, and
  improves cache locality.
- **`FileSource(path)` constructor** drops the runtime `buffer_byte_size`
  parameter; `PageSize` is a compile-time constant. `from_file<CharT,
  PageSize = 4096>(path)` defaults to 4096 so existing calls are unaffected.
- **`SourceMap`'s FileSource constructor is a template over PageSize**; the
  source is held type-erased (`const void*` + size/at function pointers) so
  SourceMap itself stays non-templated.

#### Phase 4 — Tier 1 char: value_type≠char is first-class
- **`escape_char_for_expected` / `escape_string_for_expected` are templated
  over `CharT`.** `char` path preserved; wider non-printable codepoints
  (e.g. `char32_t`) render as `\UNNNNNNNN` (full-width hex) instead of
  `\xNN` (which truncated to the low byte). A minimal UTF-8 encoder
  (`to_display`) renders codepoints for the sequence terminal path.
- **`record_expected` no longer truncates via `static_cast<char>`** — the
  three sites (range lo/hi, set elements, sequence elements) preserve the
  original value_type so diagnostics are correct for any character type.
- **`Context<char32_t>` is a first-class specialization** (satisfies
  `PegContext`), tested by `test/char32_smoke_test.cpp`. Tier 2/3 (UTF-8
  decoder, codepoint-aware `.`, templated `GrammarCompiler`) remain future
  work.

### Removed — custom Context extension point (semantic change)
- **Source erasure removes the ability to plug in a fully custom Context
  type.** Previously a user could write any type satisfying `PegContext` and
  use it as `Grammar`'s template argument. After erasure, `Grammar<CharT,
  NodeType>` instantiates a `Context<CharT, NodeType>` internally — the
  InputSource is no longer a template parameter you control. This is a
  deliberate trade: type-safety and the Grammar-drives-any-source property
  replace the open extension point. If you had a custom Context, port it to
  the `Context<CharT, NodeType>` + custom `InputSourceBase<CharT>` adapter
  pattern (see `SpanSource` / `FileSourceSource` in `InputSource.h`).

### Changed — parse API error contract
- **`Grammar::parse` / `parse_tree` / `parse_string` no longer throw
  `peg::ParseError` for cut-committed failures.** Previously, a cut operator
  inside an alternative or unbounded repetition caused a `peg::ParseError` to
  propagate out of the parse call (despite the `bool` return type suggesting
  all failures were reported via the return value). These exceptions are now
  caught at the `Grammar` boundary and reported as normal failures: callers
  see `false` from `parse` (or `nullptr` from `parse_tree`) and retrieve the
  diagnostic via `ctx.take_error()`, exactly as for a regular parse failure.
  This makes the `bool` return contract self-consistent and unifies error
  handling. **Migration**: any caller code of the form
  `try { g.parse(ctx); } catch (const peg::ParseError&) { ... }` should be
  rewritten as `if (!g.parse(ctx)) { auto err = ctx.take_error(); ... }`.
  `ParseError` is still thrown internally and remains constructible by users
  for ad-hoc diagnostics; only the `Grammar::parse*` call boundary changes.

### Added — PegContext concept enforcement
- **`PegContext<C>` is now applied as a constraint on `Grammar`'s template
  parameter.** The concept was previously defined but unused; it now mirrors
  the full Context API that combinators depend on (position/state, memo, cut,
  error tracking, and all nested types). A custom Context type that's missing
  a required method fails fast at `Grammar` instantiation with a single
  concept diagnostic instead of a deep template error inside a combinator.
- Self-check `static_assert`s verify the three shipped Context specializations
  (`span<const char>`, `span<const char>, PegAstNodePtr`, `FileSource<char>`)
  all satisfy the concept.

### Changed — dead-code cleanup (preparing for concept enforcement)
- **Removed `RuleState::m_last_return`** and the four-argument
  `Context::update_rule_state` overload. Both were write-only leftovers from
  the pre-Phase-2 boolean-returning `parse` model; every memo-hit path now
  uses `RuleState::m_cached_result.success` instead.
- **`Context::State` now exposes `operator==`**, and the combinators
  (`Repetition`, `DynRepeatExpr`) compare `State` values directly instead of
  reaching into `State::m_pos`. This keeps `m_pos` an internal detail so the
  public Context contract (now expressed by `PegContext`) stays clean.

### Fixed — Phase 2 (X4 Rule redesign)
- **shared_ptr cycle leak in recursive grammars**: eliminated at the source
  by making `Rule` a non-owning handle. Previously, recursive grammars
  (both textual via `GrammarCompiler` and the primary C++ DSL) formed
  reference cycles through `shared_ptr<NonTerminal>` edges embedded in
  expression trees; ASan reported ~7720 bytes leaked in 20 allocations
  from 100 recursive grammars + a static JSON grammar. With the X4
  redesign, `~Grammar()` is `= default` — no runtime cycle-breaking
  patches, no `weak_ptr`, no manual `clear_body()`. ASan now reports
  zero leaks.

### Changed — Phase 2 (X4 Rule redesign)
- **`Grammar` now stores `std::shared_ptr<NonTerminal>` directly** in its
  rule map (was via the `Rule` wrapper class). Grammar is the sole owner
  of all rule entities.
- **`Rule` (formerly `RuleProxy`) is now a non-owning handle**: it stores
  a bare `NonTerminal*` plus the rule name, and lives in
  `peg::parsers::`. Returned by `Grammar::operator[]`. Expression trees
  embed `Rule` copies by value (~16 bytes: pointer + string reference).
- **Design constraint**: a `Rule` cannot outlive its `Grammar`. This is
  intentional — it is what eliminates the shared_ptr cycle. Documented
  in README's "Lifetime & Recursive Rules" section.
- **`Rule::operator=(const Rule&)` is now alias assignment** with lazy
  semantics: `g["A"] = g["B"]` makes A's body delegate to B's NonTerminal.
  If B is later reassigned, parsing A sees the update. **Behavior change**:
  the previous `RuleProxy::operator=(RuleProxy)` did a shared_ptr copy
  that made A and B share the same NonTerminal entity (and `set_name`
  then renamed that shared entity to A — effectively mutating B). The
  new semantics keep A and B distinct: only A's body delegates to B.

### Removed — Phase 2 (X4 Rule redesign)
- **Old `Rule` class** (shared_ptr owner wrapper from Phase 2.0) — replaced
  by the non-owning `Rule` (formerly `RuleProxy`).
- **`RuleProxy`** name — class renamed to `Rule`.
- **`RuleRefWrapper`** — `Rule` is itself a `ParsingExpr`, no wrapper needed.
  `GrammarCompiler::compile_ruleref` now uses `make_shared<Rule>(...)`
  directly.
- **`Grammar::at(name)`** — use `has_rule()` + `operator[]` for introspection.
- **`Context::Rule`** typedef — no longer needed.
- **`peg::Rule<>`** namespace alias — removed.
- **`include/peglib/Macros.h`** — empty stub since Phase 2 (PEG_RULE macros
  were removed); now physically deleted.

### Migrated — Phase 2 (X4 Rule redesign)
- `error_test.cpp` 4 cases rewritten from `Ctxt::Rule{peg::terminal('x')}`
  to `Grammar<>` API. Updated `error-terminal-records-expected-on-failure`
  assertion (named rule now contributes both Literal and RuleName to the
  expected set, so size is 2 not 1).
- `recursive_leak_test.cpp` cleaned up: commented-out standalone `Rule<>`
  subcase removed (no longer applicable — standalone Rule is gone).

### Added — Phase 2 (Textual Grammar Format)
- **`GrammarCompiler::from_string(text)`** (`include/peglib/GrammarCompiler.h`):
  compiles PEG text at runtime into a working `Grammar<>`. Supports canonical
  PEG syntax (Ford 2004): rule definitions (`<-`), sequence, ordered choice
  (`/`), repetition (`* + ?`), predicates (`! &`), dot (`.`), character
  classes (`[a-z]`, `[^0-9]`), literals (`'...'`, `"..."` with escapes),
  grouping (`(...)`), and comments (`# ...`).
- **`try_from_string(text, out, err)`**: non-throwing version. Reports
  undefined rule references via `Diagnostic`.
- **`meta_grammar()`** (`include/peglib/MetaGrammar.h`): C++ reference
  PEG-in-PEG parser. Produces `PegAstNode` trees via tree-based actions.
- **`meta/peg.peg`**: authoritative textual PEG spec (20 rules). Used as a
  self-parse regression test.
- **`unreachable_rules()`**: grammar validation helper listing defined rules
  not reachable from the start rule (dead-code detection). Works for both
  C++ and text grammars via `collect_rule_refs()` virtual visitor.
- **`ParseTreeNode`**: immutable record of a match (name, start/end offset,
  children, NodeType value). Returned in `ParseResult`.
- New test files: `from_string_test.cpp` (19 cases), `meta_grammar_test.cpp`
  (20 cases), `self_parse_test.cpp` (3 cases), `dynexpr_test.cpp` (9 cases).

### Changed — Phase 2 (Post-Parse Action Model)
- **Breaking**: `parse()` returns `ParseResult {success, tree}` instead of
  `bool`. `ParseResult` has `explicit operator bool()`.
- **Breaking**: Semantic action signature changed from
  `(Context&, Context::match_range)` to `(Context&, ParseTreeNodePtr)`.
  Actions read `node->children[i]->value` for sub-rule results, and access
  matched text via `ctx.get_input()` + `node->start_offset`/`end_offset`.
- **Breaking**: Value stack removed from `Context`. Deleted: `push_node`,
  `pop_node`, `peek_node`, `node_count`, `truncate_node_stack`, `clear_stack`,
  `m_value_stack`.
- **Memoization**: `RuleState` caches full `ParseResult` (including tree +
  action value). Memo hits return cached result directly — eliminates the
  packrat-vs-action conflict.
- **Seed-grow**: fixed zero-width match (now records result before breaking),
  and updates intermediate `cached_result` for recursive memo hits.

### Added — Phase 2 (Grammar API)
- **`Grammar<Ctx>`** class (`include/peglib/Grammar.h`): the primary user-facing
  API. Container of named rules with lazy creation, auto-naming, and parse
  convenience methods.
- **`RuleProxy<Context>`**: transient handle returned by `Grammar::operator[]`.
  Supports assignment (auto-names the rule), chaining (`set_action`, `set_label`),
  and participates in operator DSL expressions.
- **`parse_string(input)`**: convenience method that creates a Context internally.
- **`set_start(rule)`**: designates the entry-point rule for `parse(ctx)`.
- **`undefined_rules()`**: validation helper listing rules that were accessed
  but never assigned a definition.
- **`rule_names()`, `has_rule()`, `at()`**: introspection methods.
- New test file: `grammar_test.cpp` (10 test cases covering basic grammar,
  recursion, auto-naming, forward references, semantic actions, introspection,
  reusability).

### Changed — Phase 2 (Grammar API)
- **Grammar is the primary API**. `Rule<>` is now an internal implementation
  detail (still accessible in `peg::parsers::` for advanced use, but not
  advertised in documentation or examples).
- **Rules are auto-named** from the Grammar map key — no more `set_name()`
  or `PEG_RULE` macros needed.
- **Recursive rules are trivial**: `g["expr"] = g["expr"] >> '+' >> g["number"]`
  works directly. No forward declarations, no static-initializer lambdas.
- **`PegContext` concept**: removed `typename C::Rule` requirement (Rule is
  no longer a Context member in the public API).

### Removed — Phase 2 (Grammar API)
- **`PEG_RULE`, `PEG_RULE_LABELED`, `PEG_RULE_DEF`, `PEG_RULE_DEF_LABELED`,
  `PEG_RULE_RECURSIVE`** macros — superseded by Grammar auto-naming.
- `peg_rule_name`, `peg_rule_label` helper functions.

### Migrated — Phase 2
- All test files (`rule_test.cpp`, `parser_test.cpp`, `error_test.cpp`,
  `value_stack_test.cpp`, `json_test.cpp`, `lua.cpp`, `lua_lex.cpp`,
  `main.cpp`) migrated from `Rule<>` to `Grammar<>` API.

### Added — Phase 2.0 (Rule Lifetime Redesign)
- **`PEG_RULE_DEF`** and **`PEG_RULE_DEF_LABELED`** macros for recursive rules
  (forward-declare + assign + name in one line).
- **`PEG_RULE_RECURSIVE`** alias for `PEG_RULE_DEF`.
- **Recursive rules** documentation section in README.
- Regression test: `local-rule-in-lambda-does-not-dangle` — verifies that
  Rule variables created inside a lambda survive after the lambda exits.

### Changed — Phase 2.0
- **Breaking**: `Rule` is now a `shared_ptr<NonTerminal>` handle, not a
  `NonTerminal` alias. This fixes the dangling-reference crash when local
  Rule variables go out of scope. Copy is shallow (shared ownership) —
  multiple Rule objects can point to the same NonTerminal identity.
- **Breaking**: `Rule<> r = r >> 'b' | 'a'` (self-referential copy-init) no
  longer works and will crash. Use `Rule<> r; r = r >> 'b' | 'a';` instead,
  or `PEG_RULE_DEF(Context, r, r >> 'b' | 'a')`. This is a C++ language
  constraint — the initializer expression is evaluated before `r` is
  constructed, and copying an uninitialized `shared_ptr` is undefined behavior.
- **Breaking**: `NonTerminal` is now non-copyable. Users interact exclusively
  through `Rule` (the handle).
- `ParsingExpr` move constructor/assignment now defaulted (was `= delete`),
  enabling derived types (including `Rule`) to be copy-assigned.

### Removed — Phase 2.0
- **`NonTerminalRef`** deleted entirely. `Rule` itself is now the reference
  (via shared ownership).
- **`self()`** helper deleted. All operator overloads in `Rule.h` handle
  operands uniformly via `static_cast<const ParsingExprType&>(expr)`.

### Added — Phase 1 (Core Infrastructure for Lua)
- **`SourceMap`** (`include/peglib/SourceMap.h`): byte offset ↔ (line, col)
  mapping. Supports both contiguous (`std::string_view`) and streaming
  (`FileSource<char>`) input sources. O(n) prescan + lazy line re-read for
  `FileSource`. Provides `locate()`, `offset_of()`, `line_view()` (contiguous
  only, O(1)) and `line_content()` (both, O(line length)).
- **`Diagnostic`** and **`ParseError`** (`include/peglib/ParseError.h`):
  - `ExpectedItem` / `ExpectedKind`: structured "expected" set with
    `RuleLabel`, `RuleName`, `Literal`, `Range` variants.
  - `escape_char_for_expected` / `escape_string_for_expected`: printable forms
    for control characters (`\t`, `\n`, `\r`, `\xNN`).
  - `Diagnostic`: value object carrying the furthest-failure position and
    expected set; `format(SourceMap, filename)` produces
    `file:line:col: error: expected A or B` messages.
  - `ParseError`: exception type (derives `std::runtime_error`) thrown on
    cut-committed failure. Convertible to `Diagnostic` via `to_diagnostic()`.
- **Error tracking in `Context`**: `record_failure()`, `furthest_failure_pos()`,
  `expected()`, `has_error()`, `take_error()`. Furthest-wins / same-position-
  accumulates / earlier-ignored update policy.
- **`set_name` / `set_label`** on `NonTerminal`: named rules produce better
  error messages. `PEG_RULE` and `PEG_RULE_LABELED` macros in `Macros.h`
  auto-stringify the rule name.
- **Cut-committed failures now throw `peg::ParseError`** instead of returning
  false. Affects `AlternationExpr::parse` and unbounded `Repetition::parse`.
- **Value stack in `Context`**: `push_node()`, `pop_node()`, `peek_node()`,
  `node_count()`, `clear_stack()`. Semantic-action return values are now pushed
  (previously discarded).
- **`Context<InputSource, NodeType>` template**: second template parameter
  defaults to `std::monostate` for backward compatibility. `Rule` typedef
  carries the NodeType through.
- **`PegContext` concept** (`include/peglib/Concepts.h`): compile-time
  validation of the Context API (typedefs + position + value stack + error).
- **`FileSource::release_before` wired**: cut-driven buffer eviction is now
  invoked from `Context::remove_cut` via `is_context_releasable_v` trait
  (was previously a TODO; only memo was released).
- **`FileSource::seek`, `at`, `size` public API**: enables `SourceMap` and
  other consumers to access file content by position without friendship.
- **`Parser.h` split** into `ParserFwd.h`, `Terminals.h`, `Combinators.h`,
  `NonTerminal.h`. `Parser.h` preserved as an umbrella for backward
  compatibility.
- New tests: `sourcemap_test.cpp` (8 cases), `value_stack_test.cpp` (10 cases),
  expanded `error_test.cpp` (17 cases), `context_test.cpp` gained
  `release_before` integration tests (2 cases).

### Changed — Phase 1
- **Breaking**: `ParsingExpr` template lost its third parameter (`NodeType_`);
  `NodeType` is now derived from `Context::node_type` (defaults to
  `std::monostate`).
- **Breaking**: semantic-action lambdas must now return `Context::node_type`
  (`std::monostate{}` by default) instead of `void`. Updated `lua.cpp`,
  `lua_lex.cpp`, `rule_test.cpp` accordingly.
- **Breaking**: cut-committed failures throw instead of returning false.
  `parser_test.cpp`'s `cut-suppresses-alternatives` renamed to
  `cut-committed-failure-throws-parse-error` and updated to `try/catch`.
- `FileSource::iterator` gained a public `position()` method.
- `Context::remove_cut` now invokes `m_input.release_before(m_last_cut)` for
  `FileSource`-backed inputs via `if constexpr`.

### Added — Phase 0 (pre-existing)
- Vendored doctest 2.4.11 as the test framework (`third_party/doctest.h`).
- Structured test suite split per header: `rule_test`, `parser_test`,
  `context_test`, `file_source_test`, `error_test` (placeholder).
- Modern CMake build: `peglib` INTERFACE target, C++20 required, options for
  coverage / clang-tidy / sanitizers.
- `.clang-format` and `.clang-tidy` configs for consistent style.
- GitHub Actions CI: build-test matrix (GCC/Clang/MSVC on Linux + Windows),
  format-check, clang-tidy, coverage and sanitizer jobs (master-only).
- PR and issue templates.
- This CHANGELOG.

### Changed
- **Breaking**: renamed `emtpy()` → `empty()` in `Rule.h`.
- **Breaking**: renamed `SematicAction` → `SemanticAction` across the library.
- `Context::get_input()` now returns `const InputSource&` (was by-value),
  enabling `FileSource` (move-only) to compile.
- `Context::State` no longer carries the unused `m_matchCount` field.
- `Context::reset()` assertion relaxed: lower-bound (`m_last_cut`) check
  removed so valid post-cut rewinds are not rejected.
- `FileSource` completely rewritten:
  - Constructor parameter is now documented as **bytes** (was ambiguous).
  - Internal sizes (`m_buffer_size`, `m_filesize`) are consistently **items**.
  - `buffer::read` now `resize`s before `fread` (was `reserve` → UB).
  - `iterator` has full comparison operator set (`!=`, `>`, `>=`, `<=`).
  - Cross-iterator comparison asserts same `FileSource` (was silently broken).
- `NonTerminal` gained a default constructor (supports forward-declared
  mutually-recursive rules via assignment).
- `NonTerminal::operator=` now correctly downcasts the RHS (was missing
  `static_cast`, making assignment of recursive grammars fail).
- `empty()` and `cut()` default their `Context` template parameter to match
  `Rule<>`.
- `operator|` for `(ParsingExpr, AlternationExpr)` now uses `decltype(lhs)`
  consistently (was `ParsingExprType::ParseExprType`, causing type mismatch).
- All source files reformatted with clang-format (LLVM base, 4-space, 100-col).
- README rewritten with status, quick start, build instructions, roadmap.
- TODO rewritten with phased Lua milestone roadmap.

### Removed
- Boost.Test dependency (all tests migrated to doctest).
- Dead code: `eval()` helper in old `test/main.cpp` referencing non-existent
  AST APIs.
- Misleading `NonTerminal(ParsingExpr&&)` constructor (was identical to the
  copy constructor).
- Obsolete `test/file_test.cpp` (superseded by `file_source_test.cpp`).

### Fixed
- `FileSource::buffer::read`: `reserve` → `resize` (undefined behavior: `fread`
  wrote to uninitialized memory).
- `FileSource::get`: removed `-1` EOF sentinel (conflicted with `char 0xFF`);
  now asserts in-range on dereference.
- `FileSource` `buffer_size` unit confusion (bytes vs items) — documented and
  made consistent.
- `FileSource::iterator` missing comparison operators (`!=`, `>`, `>=`, `<=`).
- `Context::get_input` returning by-value prevented `FileSource` compilation.
- `operator|` type inconsistency in `Rule.h` (`ParsingExprType::ParseExprType`
  vs `decltype(lhs)`).
- `NonTerminal::operator=` missing downcast (`static_cast<const ExprType&>`).
- `symbolConsumable(std::array)` now asserts `min <= max`.
- `test/file_test.cpp` hardcoded absolute path replaced with
  `CMAKE_CURRENT_SOURCE_DIR`.
- **Zero-width match memoization bug** (`NonTerminal::parseImpl`): when a
  rule's seed-grow loop produced a successful match that did not advance the
  position (e.g. `*ws` matching zero characters), the result was stored in
  the local `RuleState` copy but **not persisted to the memo map**. A second
  lookup at the same position returned the stale initial state `{pos, false}`,
  causing valid input to be rejected. Now calls `update_rule_state` before
  breaking out of the no-progress branch.
- **Repetition position restoration**: `Repetition::parse` did not restore
  the parser position after a child failure in the optional/bounded case.
  A failed child could leave the parser stranded mid-input, causing the next
  sibling expression in a sequence to see corrupted state. Now saves and
  restores `lastSuccessState` on child failure.
