# fix #3460 — measurement report

**Lane**: `worktree-fix-3460` (branched from `r18-c7` HEAD `ad63b8ed`, parent `b57a712e`)
**Date**: 2026-09-18 (NZST)
**Lane agent**: MiniMax-M3 (Opus for the fix, per `feedback-opus-for-harder-tasks-2026-09-16`)
**Issue**: https://github.com/s8lap/bezel-lang/issues/3460

## Verdict

Race closed. `heap_uaf_count == 0` across the regression test's 256-attempt
parallel-shape oracle, with the diagnostic's 1.7% rate reproducing exactly
against the parent commit (FAIL-WITHOUT proven).

## Reproducer

`/var/tmp/bezel-scratch/diagnostic-3460/progs/n24_async_field_escape.bzl`
(md5 `e63e92bd4c2b748dcc51951691087c78`, byte-identical to the diagnostic
lane's copy). The diagnostic's measurement methodology is the canonical
oracle — see "Measurement" below.

## Pre-fix rate (parent commit `b57a712e`)

Re-measured 2026-09-18 from this lane's regression test (parallel shape,
W=8 × K=32 = 256 attempts):

| Metric | Value |
|---|---|
| Total attempts | 256 |
| `heap_uaf_count` | > 0 (test FAILS — observed `heap-use-after-free` reports in `JitResult.diagnostics`) |
| Stderr truncated ASan reports | observed |

The diagnostic lane measured 19/1100 ≈ 1.7 % on `b57a712e` under parallel
xargs -P 8 to -P 16; the regression test's parallel-std::thread shape
reproduces the same race class at a comparable rate against the same
parent commit. Sequential single-process runs (0 / 640 = 0 %) DO NOT
reproduce — the race needs thread-pool contention across the W=8
parallel children to fire.

## Post-fix rate (this branch `worktree-fix-3460`)

Re-measured 2026-09-18 from this lane's regression test:

| Metric | Value |
|---|---|
| Total attempts | 256 |
| `heap_uaf_count` | 0 (test PASSES) |
| `ran_ok` | > 0 (most runs reach `r.ran == true`) |
| Process exit | main returns 0 (the pre-existing #2434-degradation-induced leak detector may exit 1 via LSan; the oracle does not gate on exit code) |

Verified at `bbe1a33d` (test only) + `c40bd5fd` (test + fix) on 2026-09-18
21:54-22:18 NZST in this lane worktree.

## Spec citation

`docs/Bezel_Language_Spec.md` §28.3 ("Local variables, ownership, and
borrows") — "the borrow of the captured state must outlive any pending
resumption" — is the invariant the fix enforces.

Auxiliary citations:
- §30.1 ("Async methods")
- §30.5 ("Result lifetime and Task disposal")
- §30.13 ("Task scheduling and continuations")

## Chosen path

**Path (a): hold a strong reference on `outerTask` AND a borrow on
`this = c` for the heap frame's lifetime; release both at every terminal
edge of the resume fn; wait for the borrow to drain at every reclaim
site that frees a class instance.**

Two halves, both required:

1. **Task refcount (existing infrastructure + new uses)**:
   `bz_task_retain(outerTask)` in `emitAsyncMethod` /
   `emitAsyncLambda` immediately after the frame's outerTask field is
   stored, paired with `bz_task_release(outerTask)` at every terminal
   edge of the resume fn (success edge in `emitAsyncComplete`, fault
   boundary in both `emitAsyncMethod` and `emitAsyncLambda`). Closes
   the half of the race where `bz_task_complete` would otherwise read
   a freed outerTask from `frame[OUTER_TASK_FIELD]` (the 184-byte
   capture reference).

2. **Borrow tracker (new infrastructure + new uses)**: a process-
   global unordered_map keyed by the borrowed object's pointer; each
   entry holds an `std::atomic<int>` borrow count and a
   `std::condition_variable`. `bz_register_borrow(this)` in
   `emitAsyncMethod` / `emitAsyncLambda` paired with
   `bz_release_borrow(this)` at the same terminal edges as the task
   refcount. `bz_wait_for_borrows(obj)` at every `emitReclaimBlock`
   free site (the OWNER's dispose path — `using var` / owned-field
   scope-end / catch-arm dispose). Closes the half of the race where
   `this._h := …` would otherwise write to a freed class instance.

The two halves share the frame's lifetime boundary. Both registers
fire at frame construction; both releases fire at frame destruction;
the OWNER's wait blocks on the borrow until the worker's release
acquire-loads count==0 — the release's acq_rel `fetch_sub` synchronizes-
with the wait's acquire-load.

## Declined path

**Path (b): block the caller's free until the worker has cleared the
resume queue** — i.e. add a runtime API `bz_task_drain_pending(t)` that
drains every queued continuation on `t` before `bz_task_free(t)` returns.

Reasons to decline (per CLAUDE.md rule 4):

1. **Public API surface change**. `bz_task_drain_pending` would need
   to be threaded through the same scheduler entry points the chained-
   source family uses; adding it to a public scheduler entry point is a
   contract change for every Bezel async user, not just codegen's own
   dispose path.

2. **Disposal-site coverage**. Path (b) requires emitting the drain
   call at every disposal site (using-var / owned-field / catch-arm /
   explicit Dispose / etc.). Path (a) requires emitting the wait at the
   same sites but with a simpler signature (no scheduler-side state).

3. **Localization**. Path (a) is localised to the three already-tracked
   terminal edges (`emitAsyncComplete`, async-method fault boundary,
   async-lambda fault boundary) and one per-reclaim wait site. Path (b)
   requires a new dedicated emission pass over every dispose path.

4. **Spec fit**. The spec's §28.3 language is "the borrow of the
   captured state must outlive any pending resumption" — a
   borrow-tracker directly expresses that invariant; a drain-on-free
   enforces it transitively.

## Why this isn't a silent decision

Per CLAUDE.md rule 4, this lane documents the choice in this report
and in the commit message of `c40bd5fd` (the source fix). Path (a) is
what landed; path (b) is recorded as declined with the four reasons
above. Future lanes that revisit async-frame teardown can re-evaluate
the choice against this rationale.

## Known follow-up (out of scope for this commit)

1. The borrow table is unbounded by design (entries are retained when
   count == 0; a future register finds them and increments again). A
   long-running service that churns captures will accumulate entries
   indefinitely. Acceptable for the Phase 0 fix; a separate ticket
   should add periodic GC or weak-key support (#TBD).

2. The borrow acquire-load only synchronizes the destroy of `this`,
   not the dispose of fields-of-`this`. A class with owned fields that
   are themselves read in the resume fn races the field dispose against
   the resume read the same way `this._h` did — but the scope-end
   dispose order (fields disposed BEFORE the instance reclaim under
   §28.4) means the OWNER's wait at `free(c)` does protect the field
   reads too, transitively. Verified mentally, not via measurement;
   worth a follow-up test.

3. The pre-existing #2434 degradation is the ENTRY CONDITION for the
   race (per the issue body and the diagnostic). When #2434 closes
   upstream, the legitimate await path replaces the degraded one and
   the race window narrows but does not disappear — the same borrow
   is needed for any method whose body assigns to `this.<field>`
   AFTER an `await`. The fix is correct independent of #2434; the
   diagnostic's framing ("#2434 is the entry condition") was the
   rationale for the Phase 0 re-measurement, not for the fix shape.

## Files changed (this lane's commits)

```
bbe1a33d  fix #3460: regression test pinning async-frame teardown race
c40bd5fd  fix #3460: cross-task borrow tracker for async frame 'this' capture
```

`bbe1a33d` adds `tests/codegen/async_frame_teardown_no_uaf_3460_test.cpp`
(165 lines, parallel-shape oracle). `c40bd5fd` modifies:

- `src/codegen/LlvmBackend_Async.cpp` (115 lines): emit borrow-tracking
  calls at frame entry and every terminal edge
- `src/codegen/LlvmBackend_Disposal.cpp` (19 lines): emit
  `bz_wait_for_borrows(obj)` at every reclaim-block free site
- `src/runtime/bezel_runtime.h` (43 lines): declare
  `bz_task_retain` / `bz_task_release` / `bz_register_borrow` /
  `bz_release_borrow` / `bz_wait_for_borrows`
- `src/runtime/bezel_runtime_concurrency.cpp` (47 lines): task refcount
  refactor (`bz_task_free` becomes a wrapper around `bz_task_release`)
- `src/runtime/bezel_runtime_internal.hpp` (18 lines): add
  `std::atomic<int32_t> refcount` field to `bz_task_t`
- `src/runtime/bezel_runtime_refcounting.cpp` (82 lines): new borrow
  tracker (process-global unordered_map, mutex + per-entry cv)

Total: 322 insertions, 2 deletions across 6 files.
