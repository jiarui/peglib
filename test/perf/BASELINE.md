# peglib benchmark baseline & profiling evidence

This directory holds the measurement infrastructure for performance work on
peglib. **Nothing here is a test** — `peglib_bench` is a standalone executable
gated behind `-DPEGLIB_BUILD_BENCHMARKS=ON` and is deliberately NOT registered
with ctest.

## How to reproduce

```sh
cmake -S . -B build-bench -DPEGLIB_BUILD_BENCHMARKS=ON
cmake --build build-bench -j2 --target peglib_bench
./build-bench/test/peglib_bench           # measurement run (~10s)
./build-bench/test/peglib_bench --quick   # smoke run (links + parses ok)
```

Profiling (this sandbox blocks `perf` PMU access, so callgrind is the tool):

```sh
valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.out \
    ./build-bench/test/peglib_bench --quick
callgrind_annotate /tmp/cg.out | head -40
```

## Workloads

| name | grammar | what it stresses |
|------|---------|------------------|
| `json wide array` | JSON | flat repetition, node allocation, memo |
| `json deep nest` | JSON | recursion + per-level node (capped at depth 1500 — see below) |
| `arith dense (backtrack)` | arithmetic PEG | ordered-choice backtracking, failure-path churn |
| `expr left-recursive` | `expr = expr "+" num / num` | Warth seed-grow loop, LR-stack scan |
| `lua chunk` | Lua 5.4 subset | real-world grammar breadth, recursive `expr` |

### Recursion-depth ceiling (important)

peglib is a **recursive-descent** engine: nested input drives one C++ stack
frame per nesting level (`array → value → array → …`), each level spanning
several frames (`NonTerminal::parseImpl → parse → Rule::parse →
SequenceExpr::parse …`). Under the default 8 MB stack, **depth ~2000 parses
cleanly and depth ~4000 overflows the stack (SIGSEGV).** The `json deep nest`
workload is therefore capped at 1500. This is an inherent property of the
library, not a bug — the harness documents it rather than papering over it with
a larger stack.

## Baseline numbers (GCC 15, -O2, this machine)

Numbers are ns/parse (mean over the batch) and MB/s. Run-to-run noise is
~3–5%; treat differences below ~5% as noise.

| workload | size(B) | iters | ns/parse | MB/s | ok |
|----------|--------:|------:|---------:|-----:|---:|
| json wide array | 32001 | 100 | ~26,000,000 | ~1.2 | 1 |
| json deep nest | 3002 | 100 | ~26,000,000 | ~0.1 | 1 |
| arith dense (backtrack) | 9999 | 300 | ~5,900,000 | ~1.6 | 1 |
| expr left-recursive | 9999 | 300 | ~4,000,000 | ~2.3 | 1 |
| lua chunk | 28000 | 100 | ~98,000,000 | ~0.3 | 1 |

## Profiling evidence (callgrind, `--quick`, self instruction refs)

Aggregated by hotspot cluster. These are the **measured** dominants — they
confirm (and slightly reorder) the predicted hotspot list.

| ~% of Ir | cluster | representative functions |
|---------:|---------|--------------------------|
| ~30% | **packrat memo `std::map` machinery** | `update_rule_state` 8.96%, inner-map `_Rb_tree emplace_hint` 1.53%, outer-map `emplace` folded into `NonTerminal::parse` |
| ~18% | **heap alloc / free** | `_int_malloc` 5.77%, `free` 5.14%, `malloc` 4.20%, `_int_free_*` 4.35% |
| ~17% | **`NonTerminal::parse` + `parseImpl`** (dispatch core, memo lookup) | 12.55% + 4.39% |
| ~12% | **failure-path diagnostics** | `_M_get_insert_unique_pos` 4.11%, `record_expected` 3.74%, `string::push_back` 2.74%, `ExpectedItem` insert 2.02% |
| ~7% | **`shared_ptr<ParseTreeNode>` churn** | `_Sp_counted_base::_M_release` 1.73%, `shared_ptr` ctor 1.59%, `vector::push_back` 1.66% |

### What the profile changes vs. the predicted order

The predicted top-3 (node alloc, memo DS, diagnostics) all show up. The
**profile elevated two things**:

1. **Packrat memo `std::map`** is the single largest cluster (~30%), even
   before counting the `NonTerminal::parse` time that is itself dominated by
   map lookup. → **Pass B (memo data structures) is the highest-ROI target.**
2. **Failure-path diagnostics (~12%)** is larger than predicted —
   `record_expected` builds a `std::string` on *every* failed terminal-seq
   match (the `string::push_back` + `record_expected` lines), most of which is
   discarded by the furthest-position filter. → Pass C is well-justified.

The **`shared_ptr<ParseTreeNode>` churn (~7%)** is real but secondary; Pass A
(node arena) is still worth doing but is not the top target.

## Optimization log

Each pass records before/after here. A pass that doesn't move the measured
number is reverted (we keep only what the evidence supports).

| pass | date | workloads affected | ns/parse delta | kept? |
|------|------|--------------------|---------------:|:-----:|
| (baseline) | 2026-07-01 | — | — | — |
| Pass B: two-level `std::map` → `std::unordered_map` for the packrat memo | 2026-07-01 | all | lua chunk −22% (98M→76M ns/parse); arith −16%; LR −12%; json −6..8%. Callgrind total Ir −10.4% (1.49B→1.34B); `update_rule_state` self-Ir 8.96%→3.00%. | ✓ |
| Pass C-step1: lazy string construction in failure path | 2026-07-01 | backtracking-heavy (arith, LR, lua) | modest. Callgrind total Ir −1.5% (1.34B→1.32B); `string::push_back` self-Ir 3.06%→2.46%. Wall-clock within noise. | ✓ |
| Pass C-step2: `std::set<ExpectedItem>` → flat sorted-vector `ExpectedSet` | 2026-07-01 | all backtracking-heavy | Callgrind total Ir −2.8% more (1.32B→1.28B); **−14.2% cumulative** from baseline. `_Rb_tree_insert_and_rebalance` (1.17%) gone (no per-insert node alloc). Wall-clock: lua chunk −19% (76M→61M ns/parse), arith −9%. | ✓ |
| Pass A-step1: move (don't copy) child trees into parent `children` | 2026-07-01 | all | Callgrind total Ir −0.7% (1.28B→1.274B). `_Sp_counted_base::_M_release` (1.81%) dropped out of the top 20 — the gratuitous refcount inc/dec pair per sequence/repetition child is gone. Preparatory; superseded by step 2 below (same sites became plain pointer copies). | ✓ |
| Pass A-step2: drop `shared_ptr<ParseTreeNode>` — Context arena owns all nodes, observers are raw pointers | 2026-07-01 | all | Callgrind total Ir **−14.8%** (1.274B→1.085B); **−27.4% cumulative** from baseline (1.49B→1.09B). `make_shared` ctor + `_Sp_counted_base` refcount machinery gone from the top 20; node allocation is now `deque::emplace_back` at 1.97% with no per-node free. Wall-clock: expr left-recursive 1.64×, arith 1.41×, lua 1.59×, json wide 1.20×. | ✓ |

### Pass B notes

Replaced both layers of the packrat memo (`std::map<pos, std::map<rule*,
RuleState>>`) with `std::unordered_map`. The method bodies are unchanged
(they use only `find`/`try_emplace`/`emplace`/`erase`, which both containers
support identically) — a one-line container-type swap, plus `rule_state`
switched to `try_emplace(pos)` so it doesn't name the (now hash-map) inner
type.

**Why two-level and not a single flat `(pos, rule*)` map:** a flat map was
tried first and *hung* the benchmark. `clear_siblings_at(pos, keep)` — called
every left-recursion growth iteration — must iterate every rule *at a given
position*. With a flat map keyed by the pair, that becomes a full-table scan
(O(total entries) per growth step), which is **quadratic** on left-recursive
grammars (the lua `expr = expr binop expr` workload enters the growth loop at
many positions over a large table). The two-level shape preserves
O(entries-at-pos) locality for that path while still removing the RB-tree node
allocations from the hot lookup path. All four ctest targets stay green,
including the left-recursion suite (`lr_triangle_repro_test`,
`lr_token_triangle_test`).

The new top hotspot (post Pass B+C) is heap allocation — `_int_malloc`/
`free`/`malloc`/`operator new` together ~18% of instruction refs, dominated by
`make_shared<ParseTreeNode>` firing on every successful match. That is the
**Pass A target** — addressed by the ownership refactor below.

### Pass A notes — the ownership refactor

This was the largest single win and the most important design decision. The
starting point was a `shared_ptr<ParseTreeNode>` model where the memo cached a
successful `(rule,pos)` tree and that same tree was also linked into the live
parse tree (memo ↔ tree aliasing), plus a parent's `children` vector held each
child. Refcounting reconciled "who owns this node."

**The key realization**: the sharing was *lifetime-only*, never mutation-after-
build. The fold and `on_match` only read nodes; nothing mutates a cached node.
So `shared_ptr` was solving a lifetime question that a single owner + observers
answers directly:

- The **Context owns every node** via a monotonic arena (`std::deque<ParseTreeNode>`,
  stable addresses, no per-node free).
- The memo (`RuleState::m_cached_result`), each node's `children`, and the
  `parse_tree()` return value all hold **raw `ParseTreeNode*` observers**.
- The arena outlives the entire parse + fold + the caller's in-scope use of the
  returned tree, so every observer is valid for its purpose.

`ParseTreeNodePtr` became a `using`-alias to `ParseTreeNode*`, so existing
`tree->name` / `if (tree)` call sites compile unchanged. `parse_ast` is
unaffected (it returns `optional<NodeType>`, fully owned and context-
independent). The one observable contract change: a tree returned by
`parse_tree()` is now valid only for its Context's lifetime (previously a
`shared_ptr` could keep a node alive past the Context) — but no caller ever
used that capability (parse_ast folds the tree away; parse_tree is always used
in-scope), so it was purely theoretical.

**Lifecycle safety** (all verified by the ASan/UBSan suite — 187/187 pass):
- Backtracking discards: failed-branch nodes become unreachable arena garbage,
  freed wholesale at parse end. Correctness-neutral high-water-mark tradeoff.
- Cut-eviction (`remove_cut`) drops memo *records* (cache pointers), not nodes.
- LR growth loop: superseded seeds overwritten in the memo; old seeds are
  unreachable garbage.
- No cross-parse aliasing: memo and arena both live in the per-parse Context.

