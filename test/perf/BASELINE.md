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
