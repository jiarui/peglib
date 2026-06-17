# Changelog

All notable changes to peglib will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

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
