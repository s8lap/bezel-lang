# Catch-up run sequence — 2026-08-12 (the rulings-conformance implementation run)

*Supporting record (not a numbered proposal). Written 2026-08-11 for the implementation run
starting 2026-08-12: the ordered work-list for catching the compiler up to the v0.241 rulings
sweep and the elegance graduation program (v0.242+). Complements the 2026-08-11 refresh banner
in [`implementation-roadmap-2026-07.md`](implementation-roadmap-2026-07.md) (history + tiering)
— this document is the run's starting page: which issues, in which order, and why the order.
Every issue named here carries its full work enumeration and flip-set tests in its graduation
comment; this page sequences, it does not restate. All items are labeled `bug`. **Check each
issue's current state before starting — the v0.241 layer may be partially done.***

**Status key.** Items marked ⏳ have docs **not yet merged** at this writing (the graduation
program is still landing trains): do not start them until their train's PR merges — the issue
comment will say so. Everything unmarked is live now. No item in this run is soundness-grade
(nothing reclassifies ownership); the run is reject-side conformance + behavior flips +
additive feature catch-up, in that order.

## Phase 0 — start-first (unblockers and recorded sequencing constraints)

1. **[#3046](https://github.com/s8lap/bezel-lang/issues/3046) step 1** — the mechanical
   throw-base sweep (~215 incidental sites / 38 files, Exception bases added) as a **no-op
   commit BEFORE the §31.1 flip**. Recorded sequencing: the flip commit stays small and
   reviewable.
2. **[#3048](https://github.com/s8lap/bezel-lang/issues/3048)** — the 0082 **C3 prototype**
   (the scoped await-bind desugaring behind a flag; verify the implementation is the §28.10
   cleanup-stack slot and nothing else). Small, and it **unblocks docs Train F** — do it early
   and report on the issue; the docs side is waiting on it.
3. **[#3049](https://github.com/s8lap/bezel-lang/issues/3049) token split + migration** — the
   A.1 `Identifier` split and the 128-site `var _ :=` test migration. **Must precede #3056's
   respell** (6 overlapping `var _ := owned x;` lines — the drift-verified stage-2 constraint).
   Note: the v0.241 `ref _` item ([#2679](https://github.com/s8lap/bezel-lang/issues/2679))
   is **subsumed** by the reservation — close it against #3049's landing with a citation.

## Phase 1 — reject-side conformance (breaking, near-empty inventories; each ships its spec-cited flip tests)

4. **[#3044](https://github.com/s8lap/bezel-lang/issues/3044)** — checked enum casts (constant
   + runtime arms), `FromUnderlyingOrNull` synthesis, the `default(E)` escalation incl. the
   instantiation-site check, the §21.7 totality flip.
5. **[#3045](https://github.com/s8lap/bezel-lang/issues/3045)** — shift-count domain (the
   mask→compare swap; four pin files flip, constant pins → compile errors).
6. **[#3046](https://github.com/s8lap/bezel-lang/issues/3046) step 2** — the §31.1
   Exception-derived flip, the arm-type/bound rejection arms, the binding simplification
   (two polarity-flip pins).
7. **[#3052](https://github.com/s8lap/bezel-lang/issues/3052)** — `closed` (parser + sema +
   fix-it for `sealed abstract`; the 86-site test respell), `mod=`, construction-time step
   check.
8. **[#3049](https://github.com/s8lap/bezel-lang/issues/3049) rejection arms** — the
   remedy-naming diagnostics for every reserved-`_` position; re-point `xmod_ctor_out_discard_2658` U3.
9. **The v0.241 rejection arms, where still open** —
   [#2911](https://github.com/s8lap/bezel-lang/issues/2911)/[#2962](https://github.com/s8lap/bezel-lang/issues/2962)
   (`as` at inline-array/unit targets),
   [#2758](https://github.com/s8lap/bezel-lang/issues/2758) (named index arguments),
   [#2994](https://github.com/s8lap/bezel-lang/issues/2994) (builtin constructor surface),
   [#3028](https://github.com/s8lap/bezel-lang/issues/3028) (the #3027 revert-to-diagnostic +
   conformance-case updates),
   [#2765](https://github.com/s8lap/bezel-lang/issues/2765) (the conformance-aware conversion
   predicate).

## Phase 2 — behavior flips (pinned-test re-derivation; schedule as focused lanes)

10. **[#2892](https://github.com/s8lap/bezel-lang/issues/2892)** — `finally` established at
    protected-block entry (Reading A): the `lowerTry` dispatch re-derivation and the
    #1263/#1567/#1598/#2051/#2869 ordering families. **The expensive one** — body-local
    sequences flip, enclosing-resource orderings survive.
11. **[#2934](https://github.com/s8lap/bezel-lang/issues/2934) + [#2661](https://github.com/s8lap/bezel-lang/issues/2661)**
    — qualified-name default rendering + universal `ToString` over stdlib records (the spans'
    positional form; `Range<T>`'s auto-equality body-record form — the v0.241 gate correction);
    the #2311 deferral diagnostics become the tests.

## Phase 3 — additive feature catch-up (docs-ahead lag is harmless; any order)

12. **[#3050](https://github.com/s8lap/bezel-lang/issues/3050)** — `equality skip` (the
    **one-directional** eligibility gate per v0.244's gate-round record).
13. **[#3051](https://github.com/s8lap/bezel-lang/issues/3051)** — pure-body `for` elision
    (flip the `pure_body_22_15` pin for concrete sources).
14. **[#3047](https://github.com/s8lap/bezel-lang/issues/3047)** — both abandonment lints
    (the must-not-fire matrix is the accept-control suite).
15. **[#3054](https://github.com/s8lap/bezel-lang/issues/3054)** — the stage-1 selector
    diagnostic (XS).
16. **[#2741](https://github.com/s8lap/bezel-lang/issues/2741)** — constant-narrowing source
    widening (+ unsigned-aware fold) with
    [#2985](https://github.com/s8lap/bezel-lang/issues/2985) constant-aware applicability as
    **required** co-work and #2722's discriminator widening in step.
17. **[#2662](https://github.com/s8lap/bezel-lang/issues/2662)** `Slice(Range)` lowering ·
    [#2678](https://github.com/s8lap/bezel-lang/issues/2678) dictionary-key lowering ·
    [#2704](https://github.com/s8lap/bezel-lang/issues/2704) getter-face normalization ·
    [#2732](https://github.com/s8lap/bezel-lang/issues/2732) the owned-gated call-site reclaim.
18. ⏳ **[#3055](https://github.com/s8lap/bezel-lang/issues/3055)** — delete the §19.6
    determination machinery (~1,000 sema lines + six spec-derived test files + the §21.7 arm).
    Starts when Train D (v0.245) merges. ([#3053](https://github.com/s8lap/bezel-lang/issues/3053)
    is doc-structural — **zero compiler work**.)

## Phase 4 — the big respell, deliberately last (+ the v0.250 items sequenced behind it, added 2026-08-12)

19. ⏳ **[#3056](https://github.com/s8lap/bezel-lang/issues/3056)** — the `move`/`owned`
    marker unification (~633 test lines + parser/sema). **Only after** Train E (v0.246) merges
    AND phase 0's #3049 migration is in. Resolves the
    [#3060](https://github.com/s8lap/bezel-lang/issues/3060) §30.5/A7 seam (cite it at landing).
    Last of the original R0 set because every earlier phase's tests would otherwise be
    respelled twice; items 20–21 (added 2026-08-12) are sequenced behind it.
20. ⏳ **[#3086](https://github.com/s8lap/bezel-lang/issues/3086)** — the v0.250 owned-overload
    lift (proposal 0091): the four-clause applicability filter, the §11.3 erasure-guard clash
    narrowing, the reworded §22.6 diagnostic family, and the `WhenAll`/`WhenEach` recognition
    re-keying (name → name + parameter classification; 47 name-keyed `src/` sites plus the
    `bz_task_when_all_owned` runtime-symbol surface). **Only after #3056** — resolution keys on
    the marker the respell installs. The test-corpus `*Owned` helper renames (49 gtest lines at
    v0.250) live **here**, not in the docs train: the compiler recognizes the helpers by name,
    so renaming tests ahead of the re-keying breaks them for nothing. Seven-file §22.6 lit-test
    disposition per 0091 §5.
21. ⏳ **[#3087](https://github.com/s8lap/bezel-lang/issues/3087)** — the v0.250 marked named
    slot (proposal 0092): the optional `move` in `NamedSlot`, the per-slot admission checks and
    rejection taxonomy, and the name-keyed payload-transfer lowering. **After #3056
    and with/behind [#1409](https://github.com/s8lap/bezel-lang/issues/1409)** — there is no
    shipped named-slot parser to amend, so this lands as part of (or immediately after) the
    named-destructure implementation itself. [#3094](https://github.com/s8lap/bezel-lang/issues/3094)
    **resolved at v0.251** (2026-08-12): the marked named slot now covers **move-only value
    payload fields** too (condition (ii) widened; the taxonomy's move-only rejection arm
    retired) — implement the admission per spec §19.3/§35.5, v0.251. The positional/borrowing
    move-only-payload semantics ride the owning-match tracker
    [#2045](https://github.com/s8lap/bezel-lang/issues/2045) stage 3, not this item.

## Standing notes for the run

- Every fix ships a spec-cited regression test that fails without it (`tests/README.md`); lanes
  use `scripts/agent_check.sh`, gates are the coordinator's (`CLAUDE.md` build discipline).
- ⏳ items become live as the graduation program's remaining trains merge; watch the issues.
- Docs-side support during the run: spec questions found mid-implementation get issues with the
  `spec-defect` label per house convention — the docs side triages weekly and runs fast rulings
  rounds. Do not resolve a spec ambiguity in code; file it.

*[This record is superseded by the next roadmap refresh once the run completes.]*
