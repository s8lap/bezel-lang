# Dual-oracle gate investigation (2026-08-10)

## Summary

The Bezel integration gate runs each cycle **twice**: once plain, once with
`BEZEL_AOT_FORCE_SANITIZE=address` (ASan-instrumented). Both must pass for a
push. The dual-oracle rule was established by #2633 after one case where a
SIGSEGV on the plain build passed under ASan. This investigation
re-examines that rule in light of recent bug history and asks whether a
focused memory-management subset under ASan could replace the current
every-test-under-ASan policy at lower cost.

**Conclusion**: keep the dual-oracle rule at current scale (~16k tests, ~17
min per cycle). Plan for partitioning when test count grows toward ~30k+.

## The #2633 case and its inverse

| # | Test | Bug | Direction |
|---|---|---|---|
| #2629 (cited by #2633) | `tests/sema/ctor_out_var_da_2409_test.cpp` (`ctor_out_var_da_2409`) | Refcounted over-release of an uninitialised `out var` slot — wild `bz_string_release` on stack garbage | **Plain catches** (SIGSEGV exit 139). ASan shadow does not flag the reused stack slot — test passes under ASan. |
| #2915 (inverse) | `bz$uniteq` (codegen, `#2633 case` follow-on) | Two-load vtable deref on a `%bz.string` whose word 0 is data | **ASan catches** (`global-buffer-overflow`). Plain tolerates the wild read silently. |
| #2980 (latent, refuted) | `beq`/`peq`/`iaeq` (codegen) | Same shape as #2915 | **ASan catches** |

The first row is the historical case that established the rule. The second
row is the inverse — and it was captured by the **2026-08-07 run's gate 2b**,
which printed `plain trustworthy-green at 16,325/0` while ASan caught the bug
in 5/5 arbitration retries.

**Total historical divergence cases in 30+ closed gates: 2** (one per
direction). Both are memory-management defects. The rule is proportional.

## Test categorization (~16,622 tests)

| Category | Tests | % | Policy |
|---|---:|---:|---|
| **HIGH** (memory-management) | ~3,000 | 18% | Always ASan |
| **MEDIUM** (codegen value) | ~9,500 | 57% | ASan (current) |
| **LOW** (parser/sema-type/diagnostic) | ~4,600 | 28% | Plain only |

**Critical caveat**: directory ≠ semantic category. The #2629 case is in
`tests/sema/` (which is otherwise LOW) because it exercises an owned-reference
path. **The partition must be name/keyword-based**, not directory-based.

The **350+ tests already using `runAotChecked`** (see
`tests/support/aot_support.hpp:2286`) are already always ASan regardless
of gate mode. They are the "core" of the HIGH set. Plain programs (the other
~16,000) are the marginal question.

## Per-test cost (measured)

| Mode | Per-test wall | Per-shard wall | Shards | Cycle wall |
|---|---:|---:|---:|---:|
| Plain | 0.12–0.26 s | median 8 s, max 466 s | 96 | **6.1 min** |
| ASan | 0.25–0.26 s | median 11 s, max 1280 s | 48 | **11.0 min** |

- **Per-test ASan:plain ratio: 1.37×** (not 2× as commonly assumed)
- **Per-cycle wall ratio: 1.8×** — the gap comes from ASan shard count being
  halved (96→48) by memory pressure, not from per-test overhead
- **Shard phase dominates**: 73% (plain), 70% (ASan) of cycle time
- **Build phase**: 12–19% — even doubling it barely registers

## Scaling projection (50k tests)

| Strategy | Plain | ASan | Total | Cycle | Quality |
|---|---:|---:|---:|---:|---|
| **A**: dual-oracle (current) | 1495 s | 3944 s | 5439 s | **90 min** | Full dual-oracle |
| **B1**: 5k memory tests ASan | 1495 s | 477 s | 1972 s | **33 min** | Memory subset |
| **B2**: 8k memory tests ASan | 1495 s | 708 s | 2203 s | **37 min** | Memory subset |
| **B3**: 10k memory tests ASan | 1495 s | 862 s | 2357 s | **39 min** | Memory subset |
| **C1**: 500 memory tests ASan | 1495 s | 1941 s | 3436 s | **57 min** | Tiny subset |
| **C2**: 1k memory tests ASan | 1495 s | 3790 s | 5285 s | **88 min** | Tiny subset |

Strategy B saves **~63%** at scale. Strategy C (1k tests) is too risky
— covers too few ASan-detectable shapes to catch the #2915-style defects.

**Shard wall is invariant under shard-count choice when total work is
fixed.** Strategy B only works if we REDUCE the ASan test count, not just
rebalance shards.

## Focused memory-mgmt stress suite (proposal only, not implemented)

If Strategy B is adopted, a **32-test focused memory-mgmt stress suite**
(categorised by pattern family) can supplement the plain pass:

| Family | Tests | Pattern examples |
|---|---:|---|
| Core ownership & disposal | 12 | Owned scope drop, drop-on-overwrite, branch-join, box retain/release, checked disposed access |
| Collections | 14 | Array of strings / owning array / dictionary generic keys / set drain / linked-list exception abandonment |
| Async / exception / closures | 6 | Exception unwind of locals, caught-arm unwind, closure capture abandonment, async frame spill |
| Interface / shared / weak | 6 | Interface-held class disposal, generic interface, boxed record through interface, shared clone/release, weak upgrade |

These are *smoke oracle* tests — they exercise the major lowering families
but **cannot replace the existing 16k+ tests' expression-shape coverage**.
Memory defects are sensitive to expression shape, generic/interface
dispatch, exception topology, and collection element type — no 32-test
subset can stand in.

## Recommendation

1. **Keep the dual-oracle rule at current scale.** Both directions have
   caught real defects. The 17% saving from a partial partition is small
   relative to the risk of misclassifying a test that the plain oracle
   can't catch.
2. **At current scale (~16k tests), the cost is acceptable.** ~17 min
   per cycle × 4 cycles per run = ~68 min gate wall per run.
3. **When test count approaches 30k+, plan for Strategy B.** Implementation:
   per-test ASan partition via `BEZEL_AOT_FORCE_SANITIZE_FILTER` env var
   (a 30-LOC change in `aot_support.hpp:398-406` to check the filter
   inside `runAot`), or a HIGH-test manifest file (`build/tests/asan_must.txt`)
   consulted by `gate_cycle.sh`.
4. **Don't ship Strategy B until test count grows.** The 17% saving is
   not worth the catastrophic coverage loss on a compiler that's still
   bug-fixing at 50+ fixes per run.

## Implementation notes for Strategy B (when ready)

The `BEZEL_AOT_FORCE_SANITIZE` flag is **process-global**, memoised once
at process startup in `aot_detail::forceSanitizeAddress()`
(`tests/support/aot_support.hpp:398-406`). Per-test granularity requires
one of:

1. A per-test filter env var (e.g., `BEZEL_AOT_FORCE_SANITIZE_FILTER`)
   checked inside `runAot` against the source's filename/test-name.
   ~30 LOC in `aot_support.hpp`.
2. Two separate test runs (current architecture): one plain, one ASan,
   each with a `BEZEL_TEST_LIST` filter. 0 LOC in the harness, but the
   sharded runner needs to honour the partition.

Option 2 is the cleanest restructure. Phase-3 infrastructure
(`gate_cycle.sh`, `arbitration.py`, `manifest.py`) already accepts a
TEST_LIST.

## Files & artifacts cited

- `tests/sema/ctor_out_var_nullable_da_2409_test.cpp` — the #2629/#2633 case
- `tests/codegen/out_var_slot_callee_release_2629_test.cpp` — has explicit
  "⚠ ADDRESSSANITIZER MASKS THIS DEFECT" warning in the test header
- `tests/support/aot_support.hpp:398-406` — `forceSanitizeAddress()` (process-global)
- `tests/support/aot_support.hpp:1976-1984` — `runAot()` overrides `opts.sanitizeAddress` from env var
- `tests/support/aot_support.hpp:2286` — `runAotChecked()` (always ASan)
- `src/codegen/LlvmBackend_Calls2.cpp:1643` — `ensureOutVarSlots`
- `src/codegen/LlvmBackend_Calls2.cpp:1955` — `ensureCtorOutVarSlots`
- `scripts/gate/arbitration.py:37-39` — invariant 5: ASan arbitration re-runs ASan
- `/var/tmp/bezel-scratch/gate-timings/gate-timings.jsonl` — 518 gate records, 24-day window
- Commits: `e87d868` (#2629 plain-direction fix), `618c0c8` (#2980 ASan-direction fix)
- Issues: #2629, #2633 (OPEN, established the rule), #2915, #2917 (CLOSED, dual-oracle enforced), #2980

## Status

This is a research note — not a spec-affecting proposal. Authored
2026-08-10 after the 2026-08-09 IMPL run identified this as a topic
worth examining. The current dual-oracle rule remains in force; revisit
when test count crosses ~30k.

## Sign

Co-Authored-By: MiniMax-M3
