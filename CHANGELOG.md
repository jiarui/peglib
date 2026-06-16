# Changelog

All notable changes to peglib will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
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
