# Family J — measurement table (HEAD b57a712e, 2026-09-18)

Family J per `docs/proposals/defect-campaign-2026-09.md` §2.6. Seven open
members. All measurements on the `worktree-family-J-anchor` worktree at
HEAD `b57a712e9a2cee447328343fe4a64062cfdff3e9`, bezelc built from a
fresh ccache config (no PCH stale), machine: the lane box (NVMe build,
shared dev_heavy slice — but the measurement programs themselves fit
within the slice's memory cap and were not concurrent with other heavy
ops).

## Summary table

| Issue  | Bug shape (per the issue) | Pre-measure HEAD b57a712e | Reproduces on HEAD? | Notes |
|--------|---------------------------|----------------------------|---------------------|-------|
| #4140  | SIGSEGV on mutually-recursive UNMARKED choice pair (78,510-frame alternating stack) | No crash; §35.1 fires first, classifyChoiceType completes with value-like verdict | **NO** | fix #4125 (b06b7f13, reverted before b57a712e) incidentally fixed this by REMOVING the `classifyChoiceCore` delegation; current `payloadFieldTypeIsMoveOnly` does not reach `classifyChoiceCore` at all, so the cycle is bounded by `seen`'s symbol-keyed guard. The structural gap the issue names is GONE. |
| #4145  | Exponential classification in linear choice-nesting depth (2^N classifyChoiceCore calls, ~20s at depth 26) | Polynomial (NOT exponential); depth 50 = 58.9s wall, depth 32 = 28.9s. ~N^2 growth. | **NO (exponential form)** | fix #4125's removal of the delegation also incidentally removed the 2^N call graph; current code has linear `payloadFieldTypeIsMoveOnly` self-recursion. The remaining ~N^2 growth is from `classifyChoiceCore`'s borrow leg still consulting the move-only leg per case, not from the old delegation. Tolerable to ~depth 60. |
| #4125  | Order-dependent classification across sibling instantiations of a shared generic choice (soundness-relevant) | Both orderings (A-first and B-first) classify `Outer` as owning correctly | **NO (visible form)** | The bug is MASKED by #4112's over-conservative classification: `payloadFieldTypeIsMoveOnly` does NOT apply §28.1 erasure, so `D<int>` is wrongly classified as owning via the unmarked-payload walk — and Outer early-returns on case A's owning verdict regardless of order. The DECLARATION-keyed guard (still on `TypeSymbol*`) is real but currently masked. |
| #4112  | Over-conservative Owning through an unmarked payload field (skips §28.1 erasure oracle) | Outer with `A(x: D<int>)` only classifies as Owning — D<int> erases the marker (§28.1) but the unmarked-payload walk returns "owned" unconditionally | **YES** | The exact #4112 symptom reproduces. The `payloadFieldTypeIsMoveOnly` walk does `if (pf.modifier == "owned") return true;` without consulting `ownedMarkerActiveOnInstantiatedTypeImpl`. |
| #4255  | bezelc OOMs ~24.7 GB on 2000-deep record chain | Likely still OOMs (untested at 2000; deep record chains are out of Family J's anchor scope per §2.6) | **PROBABLY** | Different function family: `checkRecordRestrictions`/`checkChoiceSelfContainment` O(N^2) via fresh `seen` per top-level type. Tarjan-SCC memoization is the fix. **SPLIT CANDIDATE** — confirmed different function family per proposal §2.6's parenthetical. |
| #4242  | flat 729-field record compiles in 24s | Likely still slow (untested) | **PROBABLY** | Different function family: the three codegen walkers (`emitRecordToString`/`emitStructEquality`/`emitRecordHashCode`) — already memoized per-`TypeSymbol` by #4234. The remaining cost is per-field emission, not per-type recursion. **SPLIT CANDIDATE.** |
| #4436  | residual N^1.9 in LLVM `pm.run` (not bezelc IR) | Untested (issue is explicit that the cost is in LLVM lowering, not bezelc) | **NOT BEZELC'S** | LLVM's pass manager on linear-size bezelc-emitted IR. RSS stays linear. Fix would be in LLVM's pass scheduling or bezelc's IR-generation shape. **DEFINITELY SPLIT** — different code path (LLVM pm.run, not bezelc sema/codegen). |

## Detail — #4145 timing on HEAD b57a712e

| Depth N | Wall (s) | Peak RSS (KB) |
|---------|----------|---------------|
| 10      |  2.46    |  75,896      |
| 15      |  4.85    |  86,580      |
| 20      |  9.70    | 102,796      |
| 25      | 18.58    | 123,440      |
| 28      | 23.31    | 135,312      |
| 30      | 25.26    | 144,608      |
| 32      | 28.89    | 153,500      |
| 35      | 40.47    | 172,532      |
| 40      | 57.99    | 197,864      |
| 45      | 55.97    | 230,848      |
| 50      | 58.92    | 275,020      |

Compare to the issue's exponential claim:
- origin/main (102d29f): depth 100 → timeout >20s.
- b06b7f1 (the #4125 fix): depth 26 → timeout >20s.
- HEAD (this measurement): depth 50 → 58.9s — POLYNOMIAL (roughly N^2, not 2^N).

The fix #4125 (b06b7f13) incidentally resolved the exponential growth by REMOVING
the `classifyChoiceCore` delegation that produced the 2^N call graph. But it was
reverted between b06b7f13 and b57a712e. The current code's linear `payloadFieldTypeIsMoveOnly`
self-recursion + `classifyChoiceCore`'s full classification (owning + borrow) over each case
produces the ~N^2 growth observed above.

## Detail — #4112 over-conservative repro on HEAD

```bezel
module M;
choice D<T> Some(owned t: T), None; end D;
choice Outer A(x: D<int>); end Outer;       // <-- D<int> should erase the marker (§28.1)
class Holder public static readonly F: Outer; end Holder;
class Program public static method Main(): int return 0; end Main; end Program;
```

bezelc output:
```
t_4112_only_int.bzl:7:3: static readonly field `F` has owning-classified
choice type `Outer`; §22.12 rule 4's residual paragraph rejects
owning-classified choices as process-lived owned values would have no
disposal site (§22.12 rule 4 residual, v0.265, #3273 / #3290; §35.4; §22.13; §28.5)
```

D<int> should classify as value-like (int erases the marker per §28.1).
Outer (whose only case is `D<int>`) should therefore classify as
value-like and produce only the §22.12 default warning. It instead
classifies as owning — the over-conservative symptom from #4112.

## Decision on Family J's split

Per the proposal §2.6 "Record-chain split: #4255/#4242/#4436 may split to
a separate family on re-verification if they're a different function
family (#4234's memoization + IR the backend emits per field)."

Confirmed by direct inspection:

- **#4255**: `checkRecordRestrictions` and `checkChoiceSelfContainment` in
  `src/sema/TypeResolution_Checks.cpp` (~:2015, :2054) — SEMA-side, not
  the §35.4 classifier. Tarjan-SCC memoization of `recordValueContainsSelf`
  is the fix. **SPLIT.**
- **#4242**: the three codegen walkers `emitRecordToString` /
  `emitStructEquality` / `emitRecordHashCode` in
  `src/codegen/LlvmBackend_*.cpp` — CODEGEN-side, already memoized by
  #4234 on `TypeSymbol*`. The remaining 24s cost on a flat 729-field
  record is per-field emission, NOT classification. **SPLIT.**
- **#4436**: LLVM `pm.run` (`TargetMachine::addPassesToEmitFile`), NOT
  bezelc IR generation. RSS stays linear; the cost is LLVM's lowering on
  bezelc's linear-size IR. **SPLIT.**

Split issue filed at: https://github.com/s8lap/bezel-lang/issues/4458
"Family J — record-chain trio (#4255 / #4242 / #4436) is a separate family:
classification memoization does not reach these code paths".

This reduces Family J from 7 members to 4 — #4140, #4145, #4125, #4112.

## Plan for the 4 remaining members

The proposal's anchor ("thread ambient `choiceSeen` into
`payloadFieldTypeIsMoveOnly`") is structurally sound for #4112 (the one
member still reproducing on HEAD), but does NOT cover #4140, #4145, #4125
in their original forms because those have been incidentally addressed by
fix #4125's revert. The right set of work is:

1. **Restore the `payloadFieldTypeIsMoveOnly` → `classifyChoiceCore`
   delegation** with ambient `choiceSeen` threading (the fix the proposal
   describes). This addresses #4112 directly (the §28.1 erasure oracle is
   consulted through the delegation), and is structurally what the issue
   #4140 stack-walk identifies as the right fix.

2. **Re-key `seen` and `owningSeen` on `const sema::TypeRef*`**
   (instantiation, not `TypeSymbol*` declaration). The revert removed
   this. It is the literal fix #4125's headline and the basis for
   #3933's own precedent.

3. **Add per-`classifyChoiceType`-call memoization cache** on
   `classifyChoiceCore`. Precedent: #4234's memoized codegen walkers.
   The cache lookup happens BEFORE the cycle guard. The cache stores
   only the FINAL verdict (not the cycle guard's `ValueLike`). This
   collapses the residual N^2 of #4145 to O(N) — every nested choice is
   classified at most once per call.

4. **§35.1 self-containment first**: the existing cycle guard (insert on
   entry, erase on exit, return `ValueLike` on cycle) already short-
   circuits re-classification of a type already being walked, which is
   exactly the "report the existing diagnostic and stop" the issue
   describes. No new code needed; the memoization cache + cycle guard
   together satisfy this requirement.

5. **Per-issue regression rows**:
   - **#4112 (unmarked-payload through erasure)**: `Outer { A(x: D<int>) }`
     → value-like classification. Single-row PASS-WITH guard.
   - **#4140 (mutual recursion unmarked)**: `A X(t: B); B Y(t: A);` → no
     crash, classifier returns `nullopt` or `ValueLike` (program is
     rejected by §35.1 first). Single-row PASS-WITH guard.
   - **#4145 (depth-N nesting, wall-clock ceiling)**: chain `Ci Wrap(payload: C{i-1})`
     for i in 1..50. `classifyChoiceType` must return within 5s on
     b57a712e + this fix. Wall-clock ceiling test.
   - **#4125 (sibling instantiation order)**: `Outer { A(x: D<int>), B(y: D<Widget>) }`
     in both orders → both classify as `Owning` (case B is the
     controlling one — and case A is correctly value-like via erasure).
     Order-independent equality test.

Decision: implementation follows above plan with one commit per
member row (per the lane brief's "one commit per member row" rule).
