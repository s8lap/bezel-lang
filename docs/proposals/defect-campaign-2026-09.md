# Defect campaign plan — 2026-09 (the memory-safety and crash families)

*Supporting record (not a numbered proposal). Written 2026-09-17 at the close of the
2026-09 spec/stdlib run; main @ `00fb0ea0`, docs at spec v0.306 / stdlib v0.306. This is the
sequenced work-list for the **bug-fixing** mode the project is now in: which open defects are
the same defect, in what order to take them, and what makes a family done. It complements
[`implementation-roadmap-2026-07.md`](implementation-roadmap-2026-07.md) and
[`master-plan.md`](master-plan.md), both of which sequence **features** (proposals, R0–R8
tracks) and say nothing about the bug population. Nothing here is normative; it opens and
closes no issue by itself.*

> ⚠ **No build was available when this was written** (the compiler needs LLVM 21; the
> authoring environment has clang 18 — [#4451](https://github.com/s8lap/bezel-lang/issues/4451)).
> Every claim below is sourced from **issue text** (numbers and dates as the issue states
> them) or from a **source read** cited by file and line at `00fb0ea0`. **None is from a run.**
> Every member listed under a family is a hypothesis about a shared mechanism, to be
> re-verified on HEAD before it is worked (lane-contract rule 6, verify-first): issues in this
> tracker routinely understate the defect, and some have moved since filing.

---

## 0. Why this document exists

The tracker holds **556 open issues** (2026-09-17). **169** carry the `bug` label; there is no
`codegen`, `leak`, `crash` or `soundness` label, and most of the memory and crash issues are
unlabeled — the label does not identify the population, and the tracker's own shape is biased
toward whatever was recently being worked
([#4453](https://github.com/s8lap/bezel-lang/issues/4453)). #4453 measured that **144 of 552**
open issues are self-declared siblings ("sibling of", "pre-existing", "residual", "found while
fixing"), and that the **narrow-fix loop is what produces them**: each narrow fix is correct,
tested, and followed by an issue for the next instance. The cautionary case is
[#3804](https://github.com/s8lap/bezel-lang/issues/3804) — the capture-cell family umbrella,
correctly diagnosed as needing a refactor across seven earlier issues, **closed**, and followed
by ten more open members.

The issue *titles* are already good diagnoses: nearly every one names the file, the function,
the mechanism and the spec section. Implementers are not short of "what should this do". They
are short of **which of these are one bug, which one to fix first, and what proves the family
closed**. That is what this document supplies.

**The clustering rule.** A family is a set of open issues whose bodies name the **same
predicate, emitter, or ordering** as the mechanism — not the same symptom. "Leaks 8 B" is not
a family; "the capture cell is freed by whichever of four disposal kinds fires first" is.
Every family below states its shared code path by file (and line where read), so the
membership is checkable.

**The falsification rule.** A family is wrongly drawn if a fix at its shared code path closes
fewer than half of its listed open members, or if re-verification shows two members have
different shared sites. Either outcome means **split the family and record why**, not "fix
the ones that closed and file the rest as residuals" — that is the loop this document exists
to break.

**What "done" means for every family.** The family closes when **its members close** — not
when the anchor lands (#3804). Each family lists the regression rows it owes; every row is a
`runAotChecked` test that fails without the fix and passes with it, proven by revert
(lane-contract rule 7), and every leak row asserts a **loop-amplified floor**, not an exact
byte count (LeakSanitizer's stack scan makes exact counts flaky; #4301's practice).

---

## 1. Sequencing — four phases

The order is forced by two facts, both measured in the tracker:

1. **A root-cause fix is only verifiable against oracles that fail when the bug is present**,
   and the suite's oracles currently pass for the wrong reasons in measured quantities
   ([#4452](https://github.com/s8lap/bezel-lang/issues/4452)): a compile timeout satisfies 477
   negative assertions, a missing toolchain silently disarms the ASan tier at 1,551 call sites,
   and a bare `EXPECT_FALSE(r.ran)` cannot tell a rejection from a SIGSEGV at 129 rows.
2. **Narrow fixes inside a family interact.**
   [#4438](https://github.com/s8lap/bezel-lang/issues/4438) is the proof: lane-36's release
   discipline fixes four capture-cell leaks standalone, and cherry-picked onto a tree carrying
   lane-31's deferred-cell-free path (#4210) it fixes none of them, with byte-identical
   failures to baseline. Two correct narrow fixes to one family cancelled each other.

| Phase | What | Why first |
|---|---|---|
| **0 — Oracle repair** | The five ratchets specified in `oracle-ratchets-2026-09.md` (a companion record, landing separately; #4452 step 1: #3733, #3682, #3732, #3892/#3895, #3917), plus the disposition of the one known intermittent memory bug on main, [#3460](https://github.com/s8lap/bezel-lang/issues/3460) (§1.1 below). | Until this lands, a family's "proven closed" is not proof. Ratchet first (counts cannot rise), fix second (burn the rows down). |
| **1 — Architectural anchors** | One lane per anchor, each replacing a duplicated or ad-hoc mechanism with one authority: **A** (#2718 refcounted capture cell), **D** (#4146 CFG opacity), **I1** (#4212 one substitution walker), **J** (#4140/#4125 one threaded cycle guard), **K** (#2924 the degrade disposition record), **L** (`NestedBodyScope` coverage ratchet). | Each anchor is filed as the root cause of a named family and is a **design already written down** in its issue body; the lane implements, it does not design. Anchors are sequenced before instances because instance fixes on the old mechanism are the thing #4438 shows interacting. |
| **2 — Surviving instances** | Re-measure every member of the anchored family on the post-anchor tree; fix survivors at the shared site; close the family when the members close. | A member that survives its anchor is the falsification signal (§0): it either has a second mechanism (split) or the anchor is incomplete (finish it). |
| **3 — The remaining families and the isolated remainder** | **B, C, E, F, G, H, M, N** — families with a clear shared site but no single design-document anchor yet — then the isolated crashes in §4. | These are one-lane-each root-cause fixes; several need an owner decision first (§5). |

Phases 0 and 1 are not serialised against each other in calendar terms — the ratchets are
build-free Python and can be written while an anchor lane runs — but **no family is declared
closed before Phase 0's ratchets are in the gate**, because the closure claim would rest on
oracles known to lie.

### 1.1 Phase 0's one code item: #3460

[#3460](https://github.com/s8lap/bezel-lang/issues/3460): an intermittent
`heap-use-after-free in bz$async$Fill$Resume`, **3 of 490 runs on main** (baseline `1e3861a`),
byte-identical stack on two trees, a worker thread reading a 24-byte block `main` already
freed. Not a capture cell (no envdtor in the trace) — async frame teardown racing the caller's
free. The issue names the entry condition as the #2434 degradation (`t.GetAwaiter().GetResult()`
degraded, so `Main` never actually awaits) and asks whether the race exists without it.
[#2434](https://github.com/s8lap/bezel-lang/issues/2434) is now **closed** — so the first step
is a re-measurement, not a fix: if the repro no longer reaches the race, record that and file
the residual question (does the teardown race exist on a program that *does* await?); if it
does, it is a Phase 0 fix, because an unexplained 0.5–4 % ASan failure in one shard is exactly
what will be attributed to whichever family is under test in Phase 1.

### 1.2 Dependency edges harvested from the tracker

These are the explicit orderings the issues themselves state; a lane brief should carry the
ones for its family.

| Edge | Source | Consequence for sequencing |
|---|---|---|
| #3541 (remove the #3500 four-kind exclusion from the sync-iterator abandonment snapshot) **was blocked on #3539** | #3541 body | #3539 closed 2026-09-02 (`state_reason: completed`). #3541 is **unblocked and unworked**; it belongs to family A and must be re-measured on the post-#3539 tree before A's anchor lands, because the anchor changes what the four kinds *are*. |
| #2578: the #2383 source-level repairs (value-typed capture ordering, early-return cell drain, IEvent hop, `scopedUsingBound` misbinding) **need the pre-existing `boxed_cell_free_429` regression repaired first** | #2578 body | Family A. The probes exist on a worktree branch and are uncommitted; A's lane should land them as rows. |
| #4438: lane-36's three-path release discipline is **defeated by** lane-31's #4210 deferred-cell-free path when both are present | #4438 body | Family A **must land as one series on one tree**, never as cherry-picks from parallel lanes. This is the strongest single argument for the #2718 model, which deletes the "who frees the cell" decision the two lanes disagreed about. |
| #3561: the async proportional-veto fix is **blocked behind** `yield return` in an `IOwningAsyncEnumerator` body not being credited as a transfer | #3561 title | Family G; the sema credit comes first. |
| #3460's entry condition is #2434 | #3460 body | #2434 is closed; re-measure (§1.1). |
| #1877 (§28.9 reclamation of plain-var trivial-dispose reference types) is **owner-ruled FOUNDATION requiring staged R1–R4 work**; six narrow sub-fixes already shipped, further narrowing needs a ruling | #3379 body | Family M's outer boundary. Not a lane; an owner decision (§5). |
| #2924 is deliberately **not** proposing to flip the twelve `internal:` Warning sites to Error | #2924 body | Family K's anchor is the *record*, not the flip; the flip is an owner decision (§5). |
| #3493: making `await using`'s `DisposeAsync` dispatch dynamic **needs a dispatch point that does not exist** (a new fixed vtable slot, or an `IAsyncDisposable` itable lookup) — "larger than a leak fix and want their own review" | #3493 body | Family H is gated on that design choice (§5). |

---

## 2. The families — Phase 1 anchors

Format for every family: **mechanism** · **anchor** (the issue whose body is the design) ·
**open members** (this tracker, 2026-09-17; closed members named only where they show the
narrow-fix loop) · **shared code path** · **what the fix must cover** · **rows owed** ·
**done** · **falsification**.

### A — Capture cells and closure environments (§27.5, §28.9)

**Mechanism.** `boxLocalSlot` promotes a captured local to a heap cell; each closure
environment stores the cell pointer; and *which of four disposal kinds frees the cell*
(`boxedCellFree`, `boxedCellFreeDeferred`, `closureRelease`, the envdtor's
`freeCellInEnvdtor`) is decided by a pre-scan (`cellFreeEligible_`, `singleCaptureLocals_`,
`definitelyEscapingLambdas_`) that must predict every capture site, loop iteration, branch,
reassignment and suspension correctly. Every member is one shape the pre-scan gets wrong,
in either direction: a leak when nobody frees, a double-free when two do.

**Anchor.** [#2718](https://github.com/s8lap/bezel-lang/issues/2718) — refcount the cell. Its
body gives the wrong model (an 8-byte cell; envs hold the pointer but `bz$lam$N$envdtor`
releases the cell's *contents*, retained at capture time), why no local patch balances it
(four candidate patches, each unsound), and the clean model: a 16-byte cell `{ptr value; i64
rc}` with the value at offset 0 so existing loads/stores through `boxedStorage` are unchanged;
capture does `rc++`; envdtor does `if (--rc == 0) { release(contents); free(cell); }`. It names
what that touches: `boxLocalSlot`, the envdtor emission (`LlvmBackend_Exprs2.cpp` ~2080–2115
at the time of filing), `boxedCellFree`, and **the machinery that exists only to decide who
frees a cell** — which the model deletes. #2718's own assessment: "plausibly a net
simplification, but it is not a one-lane change."

**Open members (33).**
- Loop-carried and scope-shape leaks: #4130, #4210, #3498, #3532, #4425, #4432, #3459
  (generic bodies), #3824 (intermediate non-escaping lambda).
- Nested and branch-dependent closures: #4214, #3518, #3500, #4230, #4273 (`??=` never
  handled by the lambda-body pre-pass).
- Reassigned closure-typed locals: #4227, #4431, #4433, #4438 (the interaction record).
- Over-release direction (double-free): #2715, #2248, #2249, #2719 (two write paths the
  assignment walk does not see).
- Closure-argument release at call sites: #2225, #2383, #2578, #4077 (constructor chain with a
  lambda argument leaks the constructing object).
- Iterator and async bodies: #2622, #3541 (unblocked, see §1.2), #2626, #3403, #3466, #3928,
  #3940 (`CaptureReclaimSetScope` omits two sets the same call clears).
- Narrowing-related: #2640.

Closed narrow fixes that did not stop the family: #3804 (the umbrella), #3426, #3500's first
half, #4422, #4174, #4226, #2706, #2854, #1864, #1921, #2305.

**Shared code path (read at `00fb0ea0`).** `src/codegen/LlvmBackend_impl.hpp:932` ff.
(`boxLocalSlot`'s per-iteration and GUARDED strategies), `:1073–1107` (the
`cellFreeEligible_` gate and the "skip freeing at an inner scope" rule), `:1833–1857` (the
`boxedCellFree` / deferred / closureRelease disposal kinds and the ordering comment that
explains why each must precede the other). `consumeLastLambdaCapturedCells`,
`collectClosureReleaseEligible` and `emitClosureCall`'s three-path gate live in
`LlvmBackend_Calls2.cpp` (#4438 cites `:6581`); `CaptureReclaimSetScope` is constructed at
five sites (#3940). `grep -c` over `src/` for the five decision-machinery names returns
**219** lines — that is the surface the anchor retires.

**What the fix must cover.** The #2718 model, landed as **one series on one tree** (§1.2,
#4438), with the pre-scan machinery deleted rather than bypassed. Then, on that tree, every
shape above re-measured: loop-carried capture, nested capture of a capturing closure,
if-expression arm lambdas, reassignment of a closure-typed local (the displaced value must be
released — #4227/#4431), branch-dependent escape (#4230), capture after an `await`
(#3403/#3466 — the cell holder needs a frame field), capture inside an iterator body abandoned
by `break` (#3541, on the post-#3539 tree), and **the over-release direction** (#2715, #2248,
#2249) — the lane must show that nothing which used to be freed once is now freed twice.

**Rows owed.** One `runAotChecked` row per open member, loop-amplified; the #2578 probes
(`value_typed_capture_using_var_no_leak`, `user_subscribe_not_misbound_as_scoped`) landed from
the worktree branch the issue names; the #4438 interaction reproduced as a row on the
pre-anchor tree and shown gone on the post-anchor tree; and a **double-free control row** for
each over-release member, so a lane cannot trade the leak for a UAF (lane-contract rule 5).

**Done.** All 33 close; #2718 closes last, when they have. The signal that the model is right
is a **net line-count reduction** in the decision machinery (#2718's "net simplification"), not
just green rows.

**Falsification.** A member that still leaks under the refcounted cell has a second
mechanism; split it out with the measurement. If the anchor lane finds the 16-byte cell breaks
an existing load/store path through `boxedStorage`, the offset-0 premise is wrong and the
model needs a redesign, not a patch — stop and record.

### D — Walkers that cannot see `case` arms or lambda bodies (§28.2, §28.9, §31.1)

**Mechanism.** `src/flow/CFG.cpp` has general edge-based divert handling for
`Return`/`Throw`/`Break`/`Continue`, and **the whole #4116/#4117/#4138/#4141/#4142 family
exists only at the two places that bypass it** — `MethodOwnership::visitStmt`'s `CaseStmt`
handling, and `applyUsingVarBorrowPinInLambdaStmt`, because the CFG "never descends into a
lambda body". The `LocalDecl`-only walker family (hotspot 8 of the 2026-08-30 duplication
census, #3591) is the same shape one level down: hand-rolled statement walkers that enumerate
some statement kinds and silently skip the rest.

**Anchor.** [#4146](https://github.com/s8lap/bezel-lang/issues/4146), which names two
remedies (teach the CFG `CaseStmt` arms and lambda bodies; or route the two bypassing walkers
through the CFG) and states either closes #4141 and #4142 as a side effect.

**Open members (7).** #4146, #4142, #4031 (no `LocalConstDecl` arm — trips
`check_localdecl_only_walkers`), #4033 (owned-target lambda check never reaches a `LocalDecl`
nested in another lambda — measured UAF), #3427 (the sweep's "not applicable" rationale is
wrong for records, value-like choices, view aliases), #3688 (a `LocalDecl`-bare site left
UNCLEAR), #2683 (sema descends into lambda bodies, the ownership pass does not — sound but
untested and the commit claims the opposite).

**Shared code path (read at `00fb0ea0`).** `grep CaseStmt src/flow/CFG.cpp` still returns
**nothing**; `src/ownership/Ownership.cpp:108` handles `CaseStmt` with its own walker (the
file's own header comment at `:41–:67` records the arm list that was missing it);
`applyUsingVarBorrowPinInLambdaStmt` is at `src/ownership/Ownership.cpp:9803`, invoked at
`:9788`. The existing ratchet is `scripts/check_localdecl_only_walkers.py` +
`scripts/localdecl_only_walkers.baseline`.

**What the fix must cover.** Pick one of #4146's two remedies and record why (decline the
other with reasons in the commit body). Then the walker ratchet's baseline goes **down**, not
sideways: every `LocalDecl`-only walker either gains the missing arms or delegates to the
CFG, and #3427's three measured holes get rows.

**Rows owed.** #4142's lambda-body `try` join (the false-reject shape, plus the `finally`
case that makes the naive exclusion unsound), #4033's nested-lambda UAF, #4031's const-local
double-free, #2683's descent (currently untested), one row per #3427 hole.

**Done.** The seven close and `localdecl_only_walkers.baseline` is banked lower with
`--update`.

**Falsification.** If routing through the CFG makes a currently-correct rejection disappear
(a walker was *deliberately* not descending — #4142 records one such deliberate exclusion),
that is a design fact to record in the issue, not a reason to add a special case.

### I1 — Generic substitution: three eager walkers, one authority (§14.1, §27.8)

**Mechanism.** Three eager substitution walkers over the same `TypeRef` kind set on three
classes, plus a fourth partial one, agree today; nothing keeps them agreeing. "The failure mode
is never 'someone wrote it wrong the first time' — it is that one copy is later taught
something the others are not." `fix #4188` exists precisely because one path did not recurse
where another did.

**Anchor.** [#4212](https://github.com/s8lap/bezel-lang/issues/4212): hoist one
`sema::substituteGenericArgs(TypeRef&, const SubstEnv&)` both `Resolver` and `ExprTC` call;
precedent `fix #3434`. Deliberately out of scope: `Emitter::applyMethodGenericSubst` /
`unifyMethodReturnTypeArg` (unification, a different operation) and `common::RecordSubstEnv`
(a lazy environment chain).

**Open members (8).** #4212, #4172 (generic type-alias substitution is a semantic no-op),
#3861 (`unifyArgSubst` unifies by arity, not by implemented instantiation), #2755 (static
access `Type.Method()` in a generic body never substitutes `U` — live caller invokes an
unsubstituted specialization), #3566 (monomorphized comparator installs the *derived* class's
parameter names), #2742 (`checkHashContainerKeys` validates a generic-method param against the
enclosing type's binding), #2735 and #2736 (owning-element sweep misses cross-module
instantiations and generic-method params).

**Shared code path (read at `00fb0ea0`).** `applyGenericSubst` /
`substituteGenericFieldType` / `substituteAliasGenericArgs` are present in
`src/sema/TypeResolution_Checks.cpp`, `TypeResolution_Names.cpp`, `ExprTypecheck.cpp`,
`ExprTypecheck_Exprs.cpp`, the two `_impl.hpp`s, and one codegen file
(`LlvmBackend_Exprs2.cpp`) — seven files for what #4212 says should be one function.

**What the fix must cover.** The hoist, with each of the four callers delegating; then each
member re-measured against the single authority. #4172 (alias substitution) and #2755
(static-access in a generic body) are the two most likely to be *missing arms* in the unified
walker rather than divergences between copies — the lane should expect the authority to need
new arms, not just consolidation.

**Rows owed.** A sema snapshot row per kind (`Named` with `genericArgs`, `InlineArray`
element, `Function` params/return, nullable preservation, alias) proving all callers produce
the same substitution; one row per open member.

**Done.** Eight close; the walker count is one.

**Falsification.** If a caller genuinely needs different substitution semantics (#4212's own
reviewer confirmed `applyGenericSubst` "genuinely cannot serve" one site), that difference is a
named parameter of the one function, not a second function. A second function surviving the
lane is the family re-forming.

### J — Non-terminating and super-linear classification (§35.1, §35.4, §28.1)

**Mechanism.** `payloadFieldTypeIsMoveOnly` recurses back into `classifyChoiceCore` with a
**fresh, empty** cycle guard on every call, severing the `choiceSeen` path guard the rest of
the recursion threads correctly. A mutually-recursive unmarked choice pair therefore recurses
without limit (78,510 frames in #4140's core), and even terminating shapes are exponential in
nesting depth (#4145) or order-dependent across sibling instantiations (#4125 —
soundness-relevant, not just slow).

**Anchor.** [#4140](https://github.com/s8lap/bezel-lang/issues/4140) +
[#4125](https://github.com/s8lap/bezel-lang/issues/4125) together: thread the ambient
`choiceSeen` into the recursive call (mirroring what `payloadFieldTypeIsBorrowCarrying`'s
Choice arm already does two paragraphs below in the same file), and key the sibling-collision
guard on the instantiation, not the declaration. Two guards, one function.

**Open members (7).** #4140 (SIGSEGV), #4145 (exponential), #4125 (order-dependent),
#4112 (over-conservative Owning classification through an unmarked payload), and the
record-chain trio #4255 (bezelc OOMs at ~24.7 GB on a 2000-deep record chain), #4242 (flat
729-field record: 24 s, survives the #4234 memoization), #4436 (residual N^1.9 after #4255 —
in LLVM's `pm.run`, not bezelc IR).

**Shared code path.** `src/common/OwnershipFacts.cpp` — `classifyChoiceCore` (~`:1661`,
`:1704`), `payloadFieldTypeIsMoveOnly` (~`:1559`) per #4140's stack; the record-chain trio is
a different function family (`#4234`'s memoization and the IR the backend emits per field) and
may split out on re-verification.

**What the fix must cover.** Both guards threaded; classification memoized per instantiated
`TypeRef`; the §35.1 self-containment diagnostic reached **before** any further classification
is attempted on an already-rejected type (#4140: "the crash happens on a path that should
simply report the existing diagnostic and stop"). For #4436, the fix is in what bezelc hands
to the pass manager, not in the classifier — measure before assuming it is the same family.

**Rows owed.** The unmarked mutual-recursion pair (FAIL: crash / PASS: clean §35.1 rejection),
alongside the existing owned-marker twin `spec_35_4_mutually_recursive_choice_pair_via_owned_marker_no_crash`;
a depth-N nesting row with a wall-clock ceiling; #4125's sibling-instantiation order row run
in both orders.

**Done.** Seven close, or the record-chain trio is split into its own family with the
measurement that shows it is a different function.

**Falsification.** A choice shape that still blows up with both guards threaded means there
is a third recursion entry into `classifyChoiceCore`; find it by the same stack-walk #4140
used, do not add a depth limit.

### K — Silent degrade: the `internal:` Warning sites and the #2491 mirror (§22.3, §28.3)

**Mechanism.** When a lowering cannot produce a value, codegen emits an `internal:`
diagnostic. #2924 measured **52 sites on `c7404bf`: 36 Error (safe), 12 Warning (ship a
degraded binary), 4 comments**, and listed all twelve Warning sites by file and line. The
Warning severity is deliberate (a bounded wrong value beats an uninitialized read) — the defect
is that there is **no record of which of the twelve degrade to a wrong VALUE and which to an
unsafe POINTER**, and that #2491's guard exists as four hand-written copies. Members are the
programs that reach those sites: a member call swallowed, a value read as zero, a getter
returning the tag byte, a SIGSEGV three statements later.

**Anchor.** [#2924](https://github.com/s8lap/bezel-lang/issues/2924): a disposition snapshot
test modeled on `tests/module/semacontext_ast_side_table_snapshot_test.cpp` — one row per
`internal:` site stating severity and value-vs-pointer disposition, so a new site or a changed
severity fails a test. `grep -c 'internal:' src/codegen/*.cpp` at `00fb0ea0` returns **99
lines** against #2924's 52 sites on `c7404bf`: the population has roughly doubled since the
issue was measured, which is itself the argument for the ratchet.

**Open members (21).**
- Undispatchable member silently no-ops: #4200, #3881, #3882, #2433, #4244 (the #605/#2547
  permissive generic-member-access gap), #4207 (IOrderedEnumerable terminal calls degrade to
  zero), #3576 (dictionary-source terminals lower to a zero-filled entry).
- Degrade then crash: #4249 (`new()` shorthand lambda: degrades via #2491, then SIGSEGVs),
  #4293 (free type var return degrades to zero; IR-verify crash on a further read), #4248
  (spurious #2491 Warning with a correct value — the mirror's copies disagree).
- Builtin member lowers to a wrong value with no diagnostic: #4120 (CString/WString/RawBytes
  `ToString()`), #4126 (CancellationToken/Range/Guid `GetHashCode()`; Stopwatch SIGSEGV),
  #2620 (TCS `SetResult` with a narrowed `T?` delivers the tag byte), #2296 (borrowing `Set.Add`
  with a class element does not deduplicate), #2295 (generic static init lowers `T.Member()` to
  zero, runs once per type not per instantiation), #1765 (`Ptr.Load/Store` is a stub returning
  default — §32.12 soundness), #2267 (a target-typed block lambda's returns are checked against
  nothing), #4094 (alias as pattern type silently fails), #3473 and #3472 (§18.5 alignment
  dropped / formattability check missing — render garbage), #2221 (`array.Sort` on a class
  Array is a heap-use-after-free with no diagnostic).

**What the fix must cover.** The disposition record first (the anchor), then per-row: every
site whose disposition is *unsafe pointer* becomes an Error (this is not the flip #2924
declines — #2924 declines flipping the *wrong-value* sites; see §5 for the owner call); the
#2491 four-way mirror becomes one function; and the "wrong value" members are individually
fixed at their lowering, since they are not fallbacks but wrong code.

**Rows owed.** The snapshot test; one row per member asserting the correct value **and**
the absence of the `internal:` Warning — noting the trap that `runAot` fills
`JitResult::diagnostics` only on a reject, so asserting a Warning's absence on a program that
compiles is vacuous unless the harness is asked for warnings (lane-contract rule 8, #3138,
`scripts/check_vacuous_tests.py`).

**Done.** The snapshot test exists and the 21 close.

**Falsification.** If two of the twelve Warning sites turn out to be reachable only from
programs sema already rejects, they are dead and should be deleted, not recorded — the record
must not fossilise unreachable code.

### L — Lazily-memoized body emitters leaking the caller's state

**Mechanism.** Block getters, indexer accessors, property thunks and disposer cores lower a
user body into a standalone memoized function **from the middle of expression lowering**; any
per-body emitter field they fail to save+reset leaks the caller's context into the callee, and
because the function is memoized the result is **frozen module-wide, decided by whichever
caller was lowered first — in `unordered_map` pointer-hash order**. #2702 (closed) enumerated
the fields and proposed one RAII guard; that guard now exists as `NestedBodyScope`
(`src/codegen/LlvmBackend_impl.hpp:1117`, `:2517`). The open members are emitters **not on
it** and fields **not in it**.

**Anchor.** A ratchet, not an issue: a build-free check that every lazily-memoized emitter
(the six #2702 named, plus `makePropertyThunk` — #4151's "7th, missing from #2702's
registry") constructs `NestedBodyScope`, and that every per-body field the scope saves is the
full list. This is the `check_localdecl_only_walkers` shape applied to emitters.

**Open members (6).** #3936 (`emitDisposerCore` leaves `emittingDisposerBody_` /
`disposerEntryExc_` unrestored — measured codegen divergence), #2723 (`currentReturnTypeNode_`
saved without its #2688 twin at three sites; block getters need the owning `TypeSymbol`),
#2628 (`iterElemIsOwnedClass_` / `iterElemSemaTy_` not saved around nested generic
specialization — silently disables the outer iterator's dispose-previous), #4150
(`currentEnumMethod_`'s stale rationale, residual soft-leak unmeasured), #4151
(`makePropertyThunk`), #2748 (the three unguarded lazy emitters; `ensureExprGetterThunk`'s
`genericSubst_` clear-vs-inherit hazard).

**What the fix must cover.** Every emitter through `NestedBodyScope`; every field in it;
the ratchet in the gate so the eighth emitter cannot appear silently.

**Rows owed.** For each member, the #2702 technique: two callers in the two orders, asserting
the memoized body is identical (the divergence #3936 measured is the failing shape).

**Done.** Six close; ratchet banked.

**Falsification.** A field that must *not* be reset (one the callee legitimately inherits) is
a documented exception in the scope, with the reason, not an omission.

---

## 3. The families — Phase 3 (clear shared site, no single design anchor yet)

### B — `checkRead` poisons before `markMovedByName` can report (§28.2, §28.3 K1.3/K7)

**Mechanism.** `checkRead` runs over the whole RHS before `recordMovesIn`. Its arms recurse
into **consume positions** (a `move x` argument, an index, a receiver) as plain reads; when
`x` is `Disposed` it emits the §28.2 *Warning* and poisons the state; `markMovedByName` has
arms only for `MovedFrom` and `Disposed`, so `Poisoned` falls through silently — the program
compiles with a Warning instead of the §28.3 Error and SEGVs. #4023 traced it; the same
collision has been patched **one arm at a time**: #3931 (loop back-edge), #4003 (structural
wrapper arms), fix-forward `102d29f` (`?.`/`?()` receivers).

**Open members (10).** #4023 (CallExpr args), #4122 (IndexExpr indices), #4124 (nested
call/ctor receiver — fully undiagnosed), #3979 (`f(move x)` after `Dispose()`), #3959 (four
sibling arms in the O3b suppressed phase — `.Dispose()`/`move` of a consumed local in a loop
body), #3999 (move-in-one-arm + capture-in-other-arm of a marker-free if/case expression),
#4055 (test gap on the `await` half), #4450 (the `Disposed` arm over-rejects where v0.303
§28.2 says warn — the same function, opposite direction), and the two runtime-facing siblings
the static gate cannot cover: #3967 (reassigning a maybe-consumed owned local UAFs), #4102
(loop-carried conditional `Dispose()` of a non-nullable owned local, silent SIGSEGV).

**Shared code path (read at `00fb0ea0`).** `src/ownership/Ownership_Checks.cpp` —
`checkRead`'s arms (the comment block at `:966–:1013` already documents the carve-out
reasoning for one arm) and `markMovedByName` (`:944`); `recordMovesIn` in
`src/ownership/Ownership.cpp`.

**What the fix must cover.** One rule, not another arm: **consume positions are skipped by
`checkRead` and validated only by `recordMovesIn`**, at every position (`CallExpr`,
`NullCondCallExpr`, `NewExpr` ctor args, `IndexExpr`, receivers, `await` operands) — and
`markMovedByName` either gains a `Poisoned` arm or the pass guarantees `Poisoned` never
precedes it. #3967 and #4102 are a different question (a *conditionally* consumed local
reaching scope end) and are the candidates to split into family N if the ordering fix does not
reach them.

**Rows owed.** A lit row per position asserting the **diagnostic text** "cannot move from
disposed local" (the load-bearing observation in #4023 is which diagnostic fires, not the exit
code), and a `runAotChecked` row per runtime-facing member.

**Done.** Ten close; #4450 lands in the same series since it is the same arm.

### C — Owned returns that lie, borrow returns that escape (§28.6, §9.7, §31.1)

**Mechanism.** `handleReturn` runs a long list of escape checks, every one rejecting a borrow
escaping through a *non-owning* return; #3993 (closed) added the opposite direction — an
`owned`-declared return whose expression is not an owned production. The fix's exemptions
and reach are the open family.

**Open members (7).** #4007 (the IEnumerable/IAsyncEnumerable exemption's safety claim is
false — same `itmap$` wild-jump SEGV, reachable through any call boundary), #4009 (a relayed
call to a non-owned-returning instance method — same double-free), #4010 (the exemption is
name-literal, not structural — over-rejects a user interface extending IEnumerable), #4428 (a
static method returning a borrow of its own parameter compiles; the caller's span carries no
liveness constraint), #3827 (callee field-store of a record aliases its string fields; scope-end
release would UAF), #3520 (a closure capturing a catch-arm binding may escape the arm —
compiles, then UAF), #3569 (the §31.1 catch-borrow rejection over-rejects an arm-confined lambda
— **ruled v0.297**, now implementation catch-up).

**Shared code path (read at `00fb0ea0`).** `checkOwnedReturnDirectBorrow`, called at
`src/ownership/Ownership.cpp:1525` (expression bodies) and `:8060`; the exemption list inside
it; `handleReturn`'s check list (#3993 cites `Ownership.cpp:6972` at filing).

**What the fix must cover.** Structural exemption (#4010) that is also *sound* (#4007 — the
exemption cannot be "IEnumerable is safe" because it is not); the relay case (#4009); the
parameter-borrow return with a caller-side liveness constraint (#4428 — this one reaches sema's
span model, not just the ownership pass); and the catch-arm pair (#3520 reject / #3569 admit)
landed together since they are the two sides of one rule.

**Rows owed.** One per member; #4007's and #4009's under ASan with the wild-jump asserted
absent.

### I2 — Erased-template layout at instantiation boundaries (§14.1, §12, §2.5)

**Mechanism.** A generic body is emitted once as an erased template (`T → i32` placeholder
width) and specialized per instantiation; at every boundary where a *substituted-type* object
meets template-emitted code — an inherited base field laid out with the derived class's
name-keyed substitution, an imported generic base's indexer, a base call into a concrete
generic base, a class-`T` `Span` across a module — the placeholder width is used against a
real layout. The symptom is an IR-verifier abort when lucky and **silent data corruption**
when not (#3348).

**Open members (13).** #2289 (inherited base field laid out with the derived class's
substitution — SEGV when the base names its parameter differently), #2291 (three cross-module
base-chain shapes abort in the verifier), #3348 (imported generic base's indexer reads i32
width — silent corruption), #3378, #3524 (`class D(): B("hello")` calls the erased template;
no base substitution built at all), #2650, #2220 (generic choice match-arm payload reads the
unspecialized struct), #2861, #2020, #710, #3655, #3659, #3526.

**Shared code path.** `buildSpecializedMethodFn` and the base-substitution construction in
`LlvmBackend_Calls2.cpp` / `emitChainCall`; the per-instantiation descriptor minting (#3655).

**What the fix must cover.** One rule — *no template-emitted code is ever entered with a
substituted-type object* — enforced at the boundaries above, with the cross-module case
(#2291, #3348, #2650) treated as the same boundary one module over, not as a separate family.
This family depends on **I1** landing first: several members are substitution that was never
*built* at the boundary, which is I1's authority missing an arm.

### I3 — The unit type `()` at a generic formal (§4.7, §7.10, §14.1)

**Open members (8).** #3501 (every spelling of a unit argument at a class-generic formal
monomorphized to `()` hard-ICEs #1386), #3525, #3486 (a unit positional parameter lays the
class out with a `void` field), #3474 (a void intrinsic in an interpolation hole), #3495
(every member call but `ToString()` on a narrowed nullable value receiver gets the tagged
struct — malformed IR, bezelc SIGILL), #3485 (`emitChainCall` applies no argument coercion),
#3514 (`emitVirtualGenericDispatch` has two `return nullptr`s *after* it has begun emitting),
#2580 (the sweep for other #1386 shapes).

**Mechanism and fix.** `()` lowers to LLVM `void`, which cannot be a field, an alloca or a
call operand; every site that lowers a *value* of type `()` has to substitute the unit
representation, and each member is one site that does not. The fix is one unit-value lowering
authority (no such helper exists at `00fb0ea0`; the name is the lane's to choose) consulted by
every value-lowering path, with #2580's sweep as the falsification
instrument (any #1386 ICE reachable from source after the fix is a missed site).

### E — Collection and inline-array literal initializers: lowering and element disposal (§15.1, §15.4)

**Mechanism.** A literal in initializer position is lowered by a different path per *face*
(local, field, auto-property, argument, return, `yield`, `as` operand, default parameter), and
the element-disposer stamp is keyed at `lowerNew` on a different head than at the literal
sites (#2648). So an inline array of an interface-typed element ICEs at one face (#4096),
SIGSEGVs at another (#4427, "sibling of #4335, `Setup.cpp` held" — a known fourth call site
left unpatched), and leaks at a third (#4351).

**Open members (13).** #4096, #4427, #4351, #3465 (owned inline-array temporary as receiver
never disposed), #2327 (owned Array field from a fresh literal miscompiles to Count=0; `[N]T?`
literal broken), #3393 (§15.4's own fresh-store archetype does not compile), #4332 (never
target-typed at four further positions — one is an ICE), #2762 (literal on a borrowing Array
field never disposed), #2676 (fresh trivial-dispose elements leak), #2648 and #2645 (the
disposer stamp), #2672 and #2685 (element-read escapes the §28.11 store rule — the sema side
that blocks the reclaim).

Closed narrow fixes: #4302, #4317, #4331, #4335, #4336.

**Shared code path.** `src/codegen/LlvmBackend_Setup.cpp` (the literal lowering and
`lowerNew`'s stamp), `wrapPropertyArrayAsIEnumerable` and its siblings; sema's §15.4
target-typing in `ExprTypecheck`.

**What the fix must cover.** One literal-lowering entry that every face calls, taking the
target as a parameter; one stamp keyed on one head. #4453 already names this family; it needs
an umbrella issue in the #4146 shape, which this plan recommends filing as the lane's first
act.

### F — Boxes that are minted and never reclaimed (§8.6, §28.9)

**Open members (8).** #3447 (`o.Equals(q)` on two `Object` locals leaks one box per call —
12,000 B / 500 at ×500), #3480 (`o.Equals()` leaks its §8.6 box, 24 B/call), #2760 (a
block-bodied `Object` getter's box never reclaimed — "NOT a spec question, §28.9 settles it"),
#3908 (a non-auto-hashable record's boxed `Equals`/`GetHashCode` bypasses the §9.2 use-site
error), #3905 (generic choice declines to box — needs a choice `specializedBoxVtableFor`),
#3904 (a leak-control row that is unreachable), #2371 (interface-to-generic-class downcast
returns null for a genuine Array; probes segfault), #3357 (cross-module boxed record `is`
discriminator).

**Mechanism and fix.** Boxes are minted at several sites (`Equals`, `GetHashCode`, getters,
argument coercion) and there is no single rule for when a mint is released. The fix is one
box-lifetime rule — mint → release at the end of the full expression unless it escapes — applied
at every mint site, plus the choice vtable (#3905). Whether that rule is "statement-end
release" or "§28.9 runtime-managed box" is a design choice the lane records (CLAUDE.md rule 4).

### G — Iterator and enumerable heads: the representation is chosen per face (§29.2, §29.11)

**Mechanism.** The runtime representation behind an `IEnumerable<T>` /
`IOwningEnumerable<T>` head (a 4-slot lazy pipeline vtable, an array wrapper, an iterator
frame, a dictionary snapshot) is chosen from the *declared head* at one **face** — local decl
(#1911, closed), return (#1916, closed), property read (#3469), argument (#3475), field store
(#1839), `for` over a user class (#3522, #1776) — and each face has its own bridge. Where a
face lacks the bridge, the §29.11 protocol is driven on a vtable whose slot 4 is out of bounds:
`pc` = 1, the wild-jump SIGSEGV that recurs across the family.

**Open members (21).** #3469, #3370, #3519, #3522, #1776, #3479, #3478, #3476, #3475, #2297,
#1839, #2307 (single-use owning iterator returns unimplemented — BAD_REJECT + double-free +
vacuous pin), #3363, #3361 (unverifiable fix), #3374, #3561 (blocked on the `yield return`
transfer credit), #3544, #3563, #2520, #3406 (rests on a v0.195 premise superseded at
v0.258), #3390 (an escaping deferred-activation frame leaks its fresh receiver — needs
frame-level `this` ownership).

**Shared code path (read at `00fb0ea0`).** `wrapAsIEnumerableForTarget`'s gate (#3469:
`if (ownLazyPipeline && isLazyEnumerableType(actualTy))`), `lowerIEnumerableFor`,
`wrapArrayAsIOwningEnumerator`, `registerIterableSourceDisposal` — spread across
`LlvmBackend_Assign.cpp`, `_Calls2.cpp`, `_Iterators.cpp`, `_Setup.cpp`, `_Stmts.cpp`.

**What the fix must cover.** #3469's own instruction, generalised: **route every face at the
existing authority rather than add another lazy arm** — the defect is in the authority's
gate, not in the routing. So: one bridge, consulted at every face, keyed on the *runtime*
representation not the declared head; then the owning-head twins (#3478/#3476/#3479) and the
async-iterator credit (#3561) on top.

**Rows owed.** A face × representation grid — the same pipeline at local, return, property,
indexer, argument, field, `for` — under ASan, each cell either correct or a **rejection**,
never a wild jump. The grid is the family's falsification instrument: a new cell that fails
after the bridge is a new face, and the fix is to route it, not to special-case it.

### H — `await using` and §28.12 keyed on the declared type

**Mechanism.** `registerUsingDisposal`'s `asyncDispose` arm resolves `DisposeAsync` — and,
since fix #3470, the step-2 physical reclamation — **statically from the declared type of the
binding** (`classOfLocal(ld)` → a direct `llvm::Function*`), while every other disposal path
dispatches dynamically through slot 2. So a derived object behind a base-typed
`await using` gets the base's cleanup (#3493), an interface-typed head runs the destructor
instead of awaiting (#3483), an abstract head does the same (#3516), a derived level's owned
fields are stranded (#3515), and a generic-parameter head reads word 0 as a vtable (#3482).

**Open members (10).** #3483, #3493, #3515, #3516, #3482, #1603, #1531 (direct `DisposeAsync()`
physical reclamation incl. the fault edge), #2623 (owned async-enumerator field not disposed
with its owner), #2624 (explicit `.Dispose()` plus scope-end leaks the pending Task), #3471
(the arm carries two spellings of the builtin-collection disposal build).

**Gated on (§5).** A dispatch point that does not exist: #3493 lays out the two options (a
new fixed vtable slot, reaching every vtable, every imported module and `bz$vtable$table`; or
an `IAsyncDisposable` itable lookup at the disposal site). The choice is an owner ruling; the
family is one lane once it is made.

### M — Release-eligibility predicates that answer a different question (§28.3, §28.9, §9.7)

**Mechanism.** The caller-side release of a string or record argument, element or field is
suppressed by predicates that test *declaration kind* rather than *content*:
`isRecordOrChoiceDeclared` fires on any record/choice (#4301 — "content-blind"), the
trivial-dispose predicate disqualifies a holder for an `owned` field of a trivial-dispose type
(#3632 — "one predicate, three consumers, measured"), the eligibility sets are cleared by a
`currentFn_` sentinel that necessarily fails inside a nested body (#2702's "a block getter body
gets no scope-end release analysis at all"). The family's stated bias is **leak over UAF**,
which is correct, and which is why every member is a measured leak rather than a crash.

**Open members (17).** #4301, #4277 (non-sealed classes and ctor-body-assigned fields still
leak), #4274, #4291 (a sealed condition that may be redundant — re-verify, not rubber-stamp),
#4360 (instance method returning a class value without `: owned` leaks 24 B/call), #2615,
#2636 (erased `IReadOnlyList` owned-field free assumes an Array backing), #2705, #2690, #1933,
#2577, #2569, #2551, #2570, #3632, #2671, #2740.

**Outer boundary (§5).** #1877 / #3379: §28.9 reclamation of plain-var trivial-dispose
reference types is unimplemented as a mechanism (heap-collection spines and closure transients
leak their backing), owner-ruled FOUNDATION requiring staged work; six narrow sub-fixes shipped;
any further narrowing needs a ruling. Family M's lane works *inside* that boundary — the
content-blind predicates — and stops at it.

**Shared code path (read at `00fb0ea0`).** `calleeReturnMayAliasArgUnretained` and
`isRecordOrChoiceDeclared` in `src/codegen/LlvmBackend_Setup.cpp` (`:23869` carries the
fix-#4254 provenance comment); the trivial-dispose predicate (census hotspot 2, #3588); the
`*ReleaseEligible_` sets in `LlvmBackend_impl.hpp`.

**What the fix must cover.** Each predicate re-asked as the question it is actually
answering ("does this record's own constructor embed a raw string the caller must keep
alive"), with the **mixed control row** #4301 names (a record with both a string field and a
class field) so the fix does not swing into a UAF.

### N — Ownership-pass admission gaps beyond B (§28.3, §28.8.1, §30.5, §21.1)

Consume/exception-edge soundness holes in the ownership pass that are not the `checkRead`
ordering: #2215 (§28.8.1 WrongOrder silently accepted — implicit exception edges from
synchronous argument evaluation missing), #2255 (no producer row admits a move-only value task
result; the impl checks head names only — soundness), #4429 (§28.14 capture reduction's
`ParameterSymbol` branch lacks the `isBorrowableType` filter), #4231 (probe: `checkAssignable`'s
nullable-blind free-type-variable carve-out), #3709 and #2745 (§21.1 const rejections keyed on
the wrong predicate / unenforced), #2749, #4053, #3873, #4262, #4215 (a codegen invariant
resting on another pass's gap), #1772 (async bodies hard-reject plain-`var` owned locals
instead of §28.3 warn+auto-drop), #3967 and #4102 if B's ordering fix does not reach them.
**No shared site** beyond "the ownership pass"; this is a holding family to be split by
re-verification, and the one place where the plan expects the census method (read all the
bodies, cluster by predicate) to be re-run rather than trusted from titles.

---

## 4. Not in a family: the isolated crashes worth naming

These have a measured crash or UAF and no family above; they are single-lane fixes for Phase 3
and are listed so they are not lost behind the families.

| Issue | Shape |
|---|---|
| #3562 | `storePrimaryCtorPositionalArgs`' blind index-pairing reaches a wild pointer through a §22.10-conforming convenience constructor |
| #2590 | Reading a function-typed field of an *imported* record segfaults (bad closure pointer across `--import`) |
| #4144 | §26.2's closed extension-target list is unenforced: extension on a choice reaches the "does not have terminator" codegen crash |
| #1882 / #1878 | A throwing destructor's exception escapes or is swallowed depending on surrounding code (§28.4) |
| #3491 | `Set` with a class element re-appends a present element; `TryAdd` lowers no value (its borrowing-`Add` twin #2296 is in K) |
| #4420 | `char` classification is lowered onto a routine whose behaviour is undefined above U+00FF (title as filed; the routine's name did not survive the tracker export — read the body) |
| #1748 | Checked-disposal-mode `NullReferenceException` on a watermark read of a non-nullable reference field |
| #2877 | The shared AOT bincache served a wrong product under a matching build identity (test-infra, HIGH) |
| #3460 | The intermittent async teardown UAF (§1.1 — Phase 0) |

---

## 5. Owner decisions this plan needs (in the order the phases hit them)

None of these is a lane. Each is a ruling the tracker already asks for, restated here with what
it unblocks.

1. **#4453's standing triage step** — adopt "before fixing an instance, ask whether it has a
   family; a family umbrella closes when its members do." This document is the first output
   of that step; without the rule it will decay like #3804.
2. **#2924 → which `internal:` Warning sites flip to Error.** The plan recommends: every site
   whose disposition is *unsafe pointer* flips; *wrong value* sites stay Warning with the
   record. #2924 explicitly declines to propose the flip; someone has to.
3. **#3493's dispatch point** — new vtable slot vs `IAsyncDisposable` itable lookup. Family H
   waits on this.
4. **#1877 / #3379's FOUNDATION ruling** — which §28.9 mechanism (refcount, region, other) for
   plain-var trivial-dispose reference types. Note that family A's anchor (#2718) *is* a
   refcount for one such type (the capture cell); landing A first produces evidence for this
   ruling, which is another reason A is Phase 1.
5. **#4454's spec-ahead-of-impl policy** (already filed as `needs-owner-decision`): whether
   the catch-up backlog keeps growing by default. Not a defect-campaign item, but it decides
   how much of the tracker this plan is allowed to ignore (§6).

---

## 6. What this plan deliberately excludes, and why

- **Spec-catch-up trackers** (`impl catch-up vX`, `Spec-ahead-of-impl`, `impl tracker`,
  `[v0.184]`…): roughly a third of the open population. They are feature work sequenced by
  `implementation-roadmap-2026-07.md` and the per-revision landing comments; the current mode
  is bug-fixing, and #4454 asks the owner whether that backlog is policy or drift.
- **Stdlib proposal trackers** (#1376–#1461, "Proposal 00NN"): sequenced by `master-plan.md`.
- **Gate, build and infrastructure issues** (#2100, #2117, #2323, #2441, #3222, #3673,
  #3678, #3691, #3698, #3715, #3734, #3737, #3739, #3781, #4252 and the like): real, and some
  are Phase 0-adjacent, but they do not change what the compiler does. The oracle holes
  (#4452's five) are the exception and are Phase 0.
- **Citation, comment and cleanup issues** (#3822, #3646, #4131, #4098, #2920, #4203, #3101,
  #3432, #2494…): the `check_src_citations` phase now guards the citation class
  ([#4455](https://github.com/s8lap/bezel-lang/issues/4455), landed); the rest are one-line
  fixes a lane picks up in passing, per CLAUDE.md's no-while-we're-at-it rule, as separate
  commits.
- **Design and study proposals** (#3047–#3087, #2034–#2041, #545, #1414, #1904, #1908,
  #2092): not defects.
- **The rule corpus's first classification** (#4383): 2,148 cases, 918 executable, zero
  divergence data. It is the single best future *input* to this document — it will show which
  families cost the most conformance rather than which got filed — and it cannot run without a
  build. When it runs, its divergence list should be folded into §2–§3 by family, and any
  family with no corpus divergences should have its priority questioned.

---

## 7. How a lane uses this document

1. Take **one family**. Read its anchor issue in full — the body is the design; do not
   re-derive it. Read `docs/lane-contract.md`.
2. **Verify-first, on HEAD, every listed member** before changing anything. Record which
   still reproduce, which have moved, which were fixed by something else. Members that no
   longer reproduce close with the commit that fixed them named, not "cannot reproduce".
3. Fix at the **shared code path**, once. If the fix needs a second site, that is either the
   family splitting (§0's rule — stop and record) or the anchor being incomplete (finish it).
4. Land **one row per member**, `runAotChecked`, loop-amplified floor, proven by revert; plus
   the family's control rows (the over-release control in A, the mixed record in M, the face
   grid in G).
5. Close the members. Close the umbrella **only when the members are closed** (#3804).
6. Update this document's family table in the same commit: members closed, members split out,
   the falsification observed if any. This record is only useful while it is current.
