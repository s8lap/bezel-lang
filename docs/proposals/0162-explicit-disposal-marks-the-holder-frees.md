# Proposal 0162 — Explicit disposal marks and the holder frees; static owned storage is disposed only at exit

| | |
|---|---|
| **Status** | graduated-v0.334 — superseded — graduated v0.334 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues" and the 2026-10-09 direction "resolve all findings autonomously": SIMPLIFY C10, C3 and C2 — an explicit `Dispose()` or direct `DisposeAsync()` performs logical disposal only and every class instance is reclaimed once, by the compiler-inserted free at its holder's release point (#5759); a second call after logical disposal has begun is a no-op and checked mode faults from its completion (#5758); a `static owned` field is not an explicit-disposal receiver (#5760); §22.12 form (iv), the mutable static owning-inline-array field, is retired and the `static readonly` form's slots are fixed (#5785; #5782 moot); critique round 1 applied, 8 findings resolved, among them the in-flight window (option H′, the issue's recommendation, where the draft's exemption for the disposal's own accesses could not be told apart by a member-entry flag test), `await using`'s completing `Dispose()`, the runtime-held holders, and a release deferred to the task's terminal state that never blocks; landed with the v0.334 changelog entry) |
| **Tracking issue** | [#5759](https://github.com/s8lap/bezel-lang/issues/5759) (`spec-defect`: a second `Dispose()` on a reclaimed block), [#5758](https://github.com/s8lap/bezel-lang/issues/5758) (`spec-question`: a second disposal call while a direct `DisposeAsync()` is in flight), [#5760](https://github.com/s8lap/bezel-lang/issues/5760) (`spec-defect`: explicit disposal of a `static owned` field), [#5785](https://github.com/s8lap/bezel-lang/issues/5785) (`spec-question`: a slot store into a static owning inline array), [#5782](https://github.com/s8lap/bezel-lang/issues/5782) (`spec-question`: exit-pass re-population of a static owning inline array); two `spec-tracking` compiler trackers are filed at landing (§7) |
| **Target module(s)** | **spec**: §28.2 (*The compiler-inserted free*, generalized), §28.3 (K3.9's disposal-moment column; K9.4; the unbound-temporary sentence), §28.4 (the reclamation sentence), §28.6 (*Who may call `Dispose()`*: the receiver list, the early-release paragraph), §28.12 (step 2; the second-call paragraph; the trivial-dispose paragraph), §22.12 (rule 2's forms, the two table rows, the store-semantics lead, note (h), rule 4's backstop clause), §22.13 (the owning-inline-array clause), §11.7, §15.1.1 (the endings), §15.3 (the static sentence), §28.7 (the static sentence and the scope sentence), §28.11 (the `move coll[i]` receivers), Appendix C.3 and C.11. **stdlib**: none. **rationale**: §5.28 (one dated paragraph), §12.15 (two dated paragraphs; the form-(iv) paragraph replaced), §12.7 (one pointer). **code-generation reference**: §7.2 (two *Looks right, is rejected* pairs). **`DECLINED.md`**: eleven `declined-at-proposal` rows (§9) |
| **Written against** | spec/stdlib **v0.333** at `429fd72` (main). Section numbers are v0.333's. |

---

## 0. Summary

Four roots of the disposal matrix's first run share one mechanism: *who reclaims an object's memory, and whether anything the language itself does afterwards can touch it.* The text today keeps two models — the trivial-dispose free at the holder's release point (§28.2, §28.9), and reclamation "immediately after whichever logical-disposal path completes" for every other class (§28.4, §28.12) — and the second model makes several decided shapes read released memory in an unchecked build: §28.6's early release of an owned field (the cascade's "no-op" second call), §28.7's dispose-then-assign, §15.3's per-slot pass after a discard, and the exit pass over an explicitly disposed static (#5759). The same model leaves a direct `DisposeAsync()` with two reclamation points (#5758). Two further roots are statics: §28.6 admits an explicit disposal of a `static owned` field that every other static rule assumes never happens (#5760), and the mutable static owning-inline-array field, form (iv), has no stated slot-store verdict and can be re-populated by the exit pass after its slots were disposed (#5785, #5782).

The decision takes one model and two subtractions:

1. **Explicit disposal marks; the holder frees.** An explicit `Dispose()` or direct `DisposeAsync()` performs logical disposal only. Every class instance is reclaimed exactly once, by the compiler-inserted free at its holder's release point, after logical disposal has completed. That is §28.9's trivial-dispose model, §5.28's "freed by compiler-inserted code at its statically-known owner's release point", and axiom A15's non-reclaiming disposal, made the rule for every class. The per-instance flag records when logical disposal *begins*; a later `Dispose()` is a no-op and a later `DisposeAsync()` returns a completed task. Checked mode's member-entry fault reads the instance as logically disposed once logical disposal has *completed*: the disposal's own accesses run on a live instance, and an alias's access inside a direct `DisposeAsync()`'s window reaches an intact block (the issue's recommendation for the window, adopted after critique).
2. **A `static owned` field is not an explicit-disposal receiver.** Static owned storage is disposed only by §22.13's exit pass — K9.4's ground, now stated for disposal as it is for `move`. Measured reach: zero explicit disposals of a `static owned` name in every corpus.
3. **Form (iv) is retired.** A mutable static field of owning-inline-array type is rejected under rule 2's closing sentence. The `static readonly` form (h) stays, disposed at exit, with its slots fixed at initialization (no slot store, no `move arr[j]`). Measured reach: two gtest programs, two lit programs and three rule-corpus cases, all the form's own tests; no guide, conformance-doc or code-generation-reference example.

No program's accepted/rejected verdict changes under item 1; item 2 rejects nothing measured; item 3 rejects the seven programs above and names their rewrite.

## 1. The problem

### 1.1 The second `Dispose()` the language issues (#5759, RC19)

§28.2: "the first call performs the disposal; subsequent calls are no-ops", and "A further `Dispose()` call remains a no-op, not an error, in both modes." §28.6: after an explicit `Dispose()` of an owned field "the slot keeps its reference and its K9 obligation, and the containing object's teardown still runs the cascade over it (§28.4 step 2), where the second call is §28.2's no-op". §28.7: "An explicit `field.Dispose();` before the assignment remains legal and behavior-preserving — `Dispose` is idempotent". §15.3's per-slot pass after `_ := arr; arr := …;` and §22.13's exit pass over an explicitly disposed `static owned` field issue the same second call.

But §28.4: "After step 3 has propagated to the root of the hierarchy, the object's memory is reclaimed — immediately, or deferred under checked disposal mode's quarantine latitude"; §28.12 step 2: "physical reclamation runs **exactly once, immediately after whichever logical-disposal path completes**"; rationale §5.30: "`--unchecked-disposal` frees it and elides the test". In an unchecked build the language's own second call reads a released block, in a program that never uses the disposed instance — against §28.2's "identical observable behavior for every program that never uses a disposed instance". Only the trivial-dispose case is carved (§28.6: "the explicit call having performed logical disposal only", the free "stays at the holder's release point").

```bezel
class Res
  destructor
    Console.WriteLine("closed");
  end destructor;
end Res;
class Holder
  private owned _r: Res;
  public constructor()
    _r := new Res();
  end constructor;
  public method Close()
    _r.Dispose();   // §28.6 early release; §28.12: reclamation "immediately"
  end Close;
end Holder;
class Demo
  public static method Run()
    using var h := new Holder();
    h.Close();
  end Run;          // h's teardown cascades over _r: "the second call is §28.2's no-op" —
end Demo;           //   unchecked, a Dispose() on _r's released block
```

### 1.2 A direct `DisposeAsync()` with two reclamation points (#5758, RC18)

A direct `DisposeAsync()` (K3.9) completes logical disposal at the returned task's terminal state, "and reclamation follows there" (§28.6, §28.12). A second disposal call inside the window is admitted — the owner's `c.Dispose()` draws §28.2's warning, an owned field's cascade and the exit pass draw nothing — and §28.12 says a second call is a no-op "after either has **run**" while "the runtime tracks completion". Under that reading the synchronous path runs the destructor and reclaims while `DisposeAsync` is suspended, and the resumed body touches a reclaimed object; when it completes, reclamation "follows there" again. The flag's timing is unstated.

```bezel
class Conn: IAsyncDisposable
  public async method DisposeAsync(): Task
    await Task.Delay(10);
    Console.WriteLine("async close");
  end DisposeAsync;
  destructor
    Console.WriteLine("sync close");
  end destructor;
end Conn;
class Demo
  public static async method Run(): Task
    var c := new Conn();
    using var t := c.DisposeAsync();   // direct call; DisposeAsync suspended
    c.Dispose();                       // no-op, or destructor plus reclamation?
    await t;                           // ...DisposeAsync resumes on that object
  end Run;
end Demo;
```

### 1.3 Explicit disposal of a `static owned` field (#5760, RC20)

§28.6 lists "`static owned` field read by code of the owning object's declaring type" among the owned receivers. K2 calls static storage "the one distinguished *always-live* source (process-lived, disposed only at exit, §22.13)"; the producers' static rows and §28.3's *Static stores* bullet admit static-storage-sourced borrows into any field or static with no residual, on the ground that "the always-live holder holds only what is always live"; K9.4 bars `move` of a static owned field because "static state is observable across method boundaries". An early disposal falsifies all of it, and across domains "the flag is no backstop" (K4.3) — while rule 4 names "the checked-disposal backstop of §28.2". For the warning-suppression set the program is warning-free.

```bezel
class Registry
  private static owned readonly _m: Mutex := new Mutex();   // suppression set: no warning
  public static property M: Mutex => _m;                     // "always-live"
  public static method Shutdown()
    _m.Dispose();   // §28.6: admitted
  end Shutdown;
end Registry;
class Demo
  private static async method Worker(): Task
    using var g := await Registry.M.AcquireAsync();   // domain B
  end Worker;
  public static async method Main(): Task
    using var j := Domain.Spawn(Worker);
    Registry.Shutdown();                   // domain A disposes _m while B may read it
    await j;
  end Main;
end Demo;
```

### 1.4 The mutable static owning inline array (#5785, #5782, RC50, RC47)

§22.12 form (iv) admits a mutable static field of owning-inline-array type, warned, with "slot stores, `move arr[j]`, and per-slot restoration follow §15.1.1 unchanged". §15.1.1 states four endings and no slot-store displacement; §28.7 says "**static** fields never take drop-on-overwrite for *any* type". So a slot store into such a field has two readings — the per-slot drop (reading A) or a plain store that strands the displaced object (reading B). Independently, the exit pass's drain re-enrols only "Static `owned` fields initialized during the pass", so a destructor run by the pass that stores into a slot of a form-(iv) static the pass has already disposed leaves an object that is never disposed and never faults. And `move arr[j]` on a static slot is exactly the per-method window K9.4's ground says cannot cover static state — a tension four matrix lanes found independently (R9/C2, R9/C5, R9/C8, R9/C10, R9/C11).

```bezel
class Res
  destructor
    Console.WriteLine("Res closed");
  end destructor;
end Res;
static class Registry
  public static Slots: [2 owned]Res?;      // §22.12 form (iv): admitted, warned
end Registry;
class Program
  public static method Main()
    Registry.Slots[0] := new Res();        // R1
    Registry.Slots[0] := new Res();        // R1 displaced: slot drop, or static no-drop?
  end Main;
end Program;
```

### 1.5 What the compiler does today

- An explicit `.Dispose()` on an owned field, `owned` parameter or owned local **reclaims the block at the call and nulls the slot**, so the later cascade or scope-end pass "finds null and skips — free happens exactly once" (`src/codegen/LlvmBackend_Calls2.cpp:5772–5810`, `explicitDisposeOwnedReceiverSlot`, #415/#991). Other receivers dispatch only; the owner's release point then runs `emitDisposeAndReclaim` (`LlvmBackend_Disposal.cpp:2738`), which dispatches the idempotent `Dispose()` and reclaims. In a checked build the block is retained after disposal (`LlvmBackend_Disposal.cpp:2860–2869`, the deliberate leak rationale §5.30 records); only an unchecked build frees. The generated `Dispose()` sets the disposed flag **before** running the destructor and the cascade (`LlvmBackend_Disposal.cpp:68–84`: "set it BEFORE running the core so a re-entrant dispose during the core … still short-circuits").
- The exit pass is an `atexit` handler that nulls every registered static slot, then drains the registry LIFO, dispatching `Dispose()` and freeing each block (`src/runtime/bezel_runtime_refcounting.cpp:35–88`).
- The explicit-disposal receiver check reads the receiver's ownership classification; the `static owned` axis is noted as "latent, not live" (`src/ownership/Ownership.cpp:7514–7520`), and no test disposes a `static owned` name explicitly.
- A mutable static owning-inline-array field is admitted with the rule-4 warning (`tests/lit/sema/sema_static_domains_1393__rule2_form4_mutable_static_owning_array_field_warns.bzl`); `move` of a static slot and a slot store through a `static readonly` array are untested.

## 2. Options

**A — Status quo.** Two reclamation models; the trivial-dispose carve alone. Rejected: 1.1's shapes read released memory in an unchecked build, 1.2 reclaims twice.

**B — Explicit disposal marks; the holder frees (adopted).** The trivial-dispose rule for every class: an explicit `Dispose()`/`DisposeAsync()` performs logical disposal and sets the flag; the compiler-inserted free at the holder's release point reclaims, after logical disposal has completed. The flag records when logical disposal begins; a second call after that is a no-op (a `DisposeAsync()` no-op returns a completed task). Checked mode faults from logical disposal's completion.

**C — A per-location "released" bit.** Fields, slots and statics carry a second bit, set by the explicit call and tested by the cascade, drop-on-overwrite and the exit pass. Rejected: a second runtime state for a fact the flag already records, tested at three sites, with the local case (a maybe-consumed join) still reading the flag of a released block.

**D — The repeated call is undefined in unchecked mode; the early-release idioms are checked-only.** Rejected: §28.6's early close, §28.7's dispose-then-assign and the discard-then-reassign shape would be mode-dependent, against §28.2's identical-behaviour claim; and the language itself, not the program, issues the second call.

**E — Vacate at the call (the compiler's shape).** An explicit disposal of a field or slot reclaims at the call and leaves the location vacated — `null` at run time, the §7.9 watermark for a non-nullable field. Rejected: a second run-time vacated state for non-nullable owned fields, read as a null fault where every other posture of the chapter reads the flag (§28.6's "the reader is K4.1's backstop", §28.7's overwritten-field posture); and it does not answer 1.2, where the block is reclaimed under a body still running on `this`.

**F — Reading B of #5758 with a recorded trust clause.** Rejected: a trust clause needs a backstop (K4.3 clause 3), and the resumed `DisposeAsync` body's accesses are the disposal's own, which no flag can fault.

**G — A later `DisposeAsync()` returns a task that completes with the first.** Rejected as the no-op's meaning: a second handle on a task the first caller owns is a new runtime mechanism, purchased for a call that is already §28.2's warning on a local and flag-backstopped elsewhere; the completed task is what "no-op" means.

**H — Fault an alias inside the window, exempting the disposal's own accesses** (the lane's first draft). Every member access through an alias other than the disposal's own faults from the beginning of disposal, while the destructor body's and the `DisposeAsync` body's own accesses are exempt. Rejected after critique: §28.2's checked test is a flag test on member entry, and the callee cannot tell the disposal's own call (`this.Helper()` from the `DisposeAsync` body, or the `DisposeAsync` method's own entry) from an alias's call — the exemption needs a per-call disposal context, a new runtime mechanism; inside the window the block and owned fields are intact (neither the cascade nor the release has run), so the fault buys no memory safety; and the "as during a destructor body" ground is vacuous, a synchronous destructor having no interleaving alias within a domain (§30.13).

**H′ — The window is a live instance; checked mode faults from completion (adopted; #5758's and the L4 lane's recommendation).** The flag records the beginning (idempotence) and the completion (the member-entry fault). An alias's access inside the window is §28.2's program error, memory-safe, and faults from the terminal state on.

**I — Keep the `static owned` receiver and record it** (amend K2, the producers' rows and the Static-stores bullet; a K4.3 static-held clause; rule 4's warning extended to the suppression set; each synchronized type's post-disposal behaviour). Rejected: a clause needs a backstop and across domains there is none (K4.3 clause 3; proposal 0151's ground), and the surface would be purchased for zero measured sites.

**J — Admit early static disposal per type, for types with a defined cross-domain post-disposal fault** (as §30.12 does for the data-owning locks). Rejected: a per-type carve for a use with zero sites; §30.12's detection covers live guards, not a later acquisition.

**K — Keep form (iv) with `move arr[j]` barred.** Rejected: closes neither #5785 (the slot-store verdict) nor #5782 (re-population), for the same migration reach as retirement.

**L — Keep form (iv) and state the per-slot drop** (#5785 reading A as the rule) **and extend the exit drain to re-populated slots** (#5782 option (a)). Rejected: a third overwrite regime (a static that drops at a slot store but not at a whole-field store) and a second drain rule, for a form whose only uses are its own tests.

**M — Retire form (iv); the `static readonly` form's slots are fixed (adopted).** One form of static owning inline array remains, written once and disposed at exit; the replaceable process-global pool is spelled `static owned readonly pool: OwningArray<T>` or a `Locked<T>` registry, both of which §22.12 already names.

## 3. Evaluation

| | Soundness | Simplicity | Consistency | Migration reach (measured, §6) |
|---|---|---|---|---|
| A status quo | fails (1.1, 1.2) | two models | §28.9/§5.28 say one thing, §28.4/§28.12 another | — |
| **B holder frees** | exactly-once reclamation on every shape, both modes | one model; §28.2's quarantine sentence stays (checked mode still retains the flag past the release point) | §28.9, §5.28, A15, §28.6's carve already say it | zero verdict changes; memory timing only |
| C released bit | sound | a second state and three test sites | — | zero |
| D unchecked UB | mode-dependent idioms | — | breaks §28.2's claim | zero |
| E vacate | sound for fields; not for 1.2 | a second vacated state | null fault vs flag fault | zero |
| **B + begin/complete (H′)** | idempotent from the beginning; the window memory-safe (block and owned fields intact), faulting from completion | one sentence; the flag word records two moments | §28.2's idempotence; the checked test keeps its member-entry form | the three direct-`DisposeAsync` rule cases keep their verdicts |
| H fault in-window, own accesses exempt | not implementable by a member-entry flag test | a per-call disposal context | — | — |
| I record | the race has no backstop | five amendments | contradicts 0151's ground | zero |
| **C3 drop the receiver** | K2's invariant restored | one deletion | matches K9.4 | zero |
| K bar `move` only | leaves two questions open | — | — | 7 programs |
| L drop + drain | sound | two rules more | three overwrite regimes | 0 |
| **M retire (iv)** | the three static questions are moot | ~20 lines deleted | K9.4's ground applied uniformly | 7 programs, all the form's own tests |

## 4. Decision

**B, with the flag set at the beginning of logical disposal; C3; M.** Stated as rules:

1. **The compiler-inserted free, for every class.** Every class instance is reclaimed by compiler-inserted code at its holder's release point — scope end for a local, `using var` or `owned` parameter; the K8 boundary for an unbound temporary; the overwrite for a displaced value; the containing object's teardown for an `owned` field or owning-inline-array slot; the container's cascade or removing operation for an owning-collection element; the exit pass for a static; and, where the runtime is the holder, its own release (a `Shared<T>` value at its last handle's disposal, a thrown exception after its catch arm, a killed frame's owned state at the frame's teardown) — and nowhere else. The free is logical disposal followed by reclamation: the idempotent `Dispose()`, a no-op where the flag is set, then the release of the block. An explicit `Dispose()` or direct `DisposeAsync()` performs logical disposal only and does not reclaim its receiver (its cleanup — the cascade over owned fields, a `Shared<T>` handle's release — reclaims what it releases, at those values' own holders' release points). Where a direct `DisposeAsync()`'s task has not reached its terminal state at the holder's release point, reclamation is deferred to that terminal state: the release point neither awaits nor blocks, and the block is released by whichever of the two comes later. (§28.2, §28.4, §28.6, §28.12, K3.9, Appendix C.3.)
2. **The flag records the beginning of logical disposal** — at the `Dispose()` call, at the direct `DisposeAsync()` call, or at `await using`'s scope exit, before the body's first statement. A second `Dispose()` or `DisposeAsync()` after either has begun is a no-op; the `DisposeAsync()` no-op returns a completed task; the `Dispose()` the runtime issues to complete an `await using` or a direct `DisposeAsync()` (step 2) is that disposal's own step, not a second call. An instance is **logically disposed** — the state §28.2's checked test faults on — once logical disposal has completed: the disposal's own accesses (a destructor body's, the `DisposeAsync` body's and its chained `base.DisposeAsync()`'s) run on a live instance, and a member access through another alias while a direct `DisposeAsync()` is in flight is §28.2's program error, memory-safe inside the window (block and owned fields intact) and faulting from the terminal state on. §28.12 step 2's cascade and chain run once, immediately after logical disposal completes, whichever path ran; only the block's release moves to the holder. (§28.12.)
3. **`Make().Dispose()` is K3.9's consume**, not a borrowing use of the temporary; the K8 boundary then reclaims only. (§28.3.)
4. **A `static owned` field is not an explicit-disposal receiver.** Its read is a borrow of K2's always-live source, and `Dispose()`/`DisposeAsync()` through it is the borrow error. Static owned storage — a `static owned` field, a static owning inline array's slots — admits no `move`, no slot store and no explicit disposal; it is disposed only by §22.13's exit pass. (§28.6, K9.4, Appendix C.3; §22.12 rule 4's backstop clause struck.)
5. **Form (iv) is retired.** A mutable static field of owning-inline-array type is a compile error under §22.12 rule 2's closing sentence. Note (h)'s `static readonly` form is the one static owning inline array: disposed at exit, its slots fixed at initialization — a slot store or `move arr[j]` through it is a compile error naming K9.4. The bare-generic-parameter rejection is relabelled (iv). (§22.12, §22.13, §11.7, §15.1.1, §15.3, §28.7, §28.11, Appendix C.3, C.11.)

The deviation from the issues' recommendations: #5758 recommended a later `DisposeAsync()` "returning a task completing with the first"; that is declined in §2 (G). #5758's (and L4's) reading of the in-flight window as not a fault is adopted (H′): the lane's first draft declined it in favour of a flag fault with the disposal's own accesses exempt (H), which the critique found unimplementable by a member-entry flag test. #5785 recommended reading A stated as a fifth ending and named retirement as the alternative; #5782 recommended the drain and said "moot if form (iv) is removed"; retirement is taken under the owner's 2026-10-09 preference and because it closes both.

## 5. The exact normative edits (▲)

The machine-actionable edit set is `scratch/v0334/W3.json` (ids `W3-C10-*`, `W3-C3-*`, `W3-C2-*`); the coordinator copies this file to `docs/proposals/0162-explicit-disposal-marks-the-holder-frees.md`. In outline:

1. **§28.2 *The compiler-inserted free*** — the paragraph's first sentence becomes the general rule of §4 item 1; the trivial-dispose case is its degenerate form. The quarantine-latitude sentence stays (checked mode retains the flag past the release point for surviving borrows).
2. **§28.4** — "reclaimed — immediately, or deferred …" becomes "reclaimed once, at its holder's release point (§28.2's compiler-inserted free), after step 3 …".
3. **§28.3 K3.9 row** — the disposal-moment column separates logical disposal (at the call) from reclamation (at the holder's release point). **§28.3 unbound temporaries** — one clause: an explicit disposal of a temporary is K3.9's consume; the boundary reclaims only. **K9.4** — "static owned storage … admits no `move`, no slot store and no explicit disposal".
4. **§28.6 *Who may call `Dispose()`*** — the receiver list loses "/ `static owned` field"; the borrow list gains "a `static owned` field read"; the disposal-moment sentence names reclamation at the holder; the early-release paragraph's trivial-dispose carve becomes the rule for every class.
5. **§28.12** — step 2 splits the cascade-and-chain (immediately after logical disposal, once) from the block's release (at the holder's release point, or at logical disposal's completion where later); the fault edge and the abandoned-task sentence follow; the second-call paragraph reads "after either has **begun**" with the flag-timing and own-accesses sentences; the trivial-dispose paragraph becomes the compiler-inserted free's description.
6. **§22.12** — rule 2's "(i)–(iv)" twice becomes "(i)–(iii)"; the owning-inline-array row's mutable cell becomes `error (iii)-closed`; the (iv) paragraph is deleted; "(v)" becomes "(iv)" in the row and the paragraph; the store-semantics lead loses "**Ownership** and **Overwrite** to form (iv)"; note (h) states the fixed slots and the two spellings of a replaceable pool; rule 4 loses "(the checked-disposal backstop of §28.2 applies; …)".
7. **§22.13, §11.7, §15.1.1, §15.3, §28.7, §28.11, Appendix C.3, C.11** — "a static field of owning-inline-array type" becomes the `static readonly` form; the form-(iv) sentences go; §15.1.1 gains the slot-store ending (W3's RC05 item, same paragraph); §28.7's scope sentence names every owned location (K3.11); §28.11's `move coll[i]` receivers exclude the `static readonly` array.
8. **Rationale** — §5.28 gains *Explicit disposal marks; the holder frees (v0.334)*; §12.15's form-(iv) paragraph becomes *The mutable owning-inline-array static (v0.334)* and gains *`static owned` storage is disposed only at exit (v0.334)*; §12.7's "Forms (iii)/(iv) …" sentence gains a pointer.
9. **Code-generation reference §7.2** — two pairs: disposing a `static owned` field early; a mutable static owning inline array.
10. **`DECLINED.md`** — the eleven rows of §9.

## 6. Measurement

Every figure below was produced by a grep over the repository at `429fd72` and the hit files read (`scratch/v0334/W3.md` §Measurement names each recipe).

- **Item 1 (B).** Verdict changes: **zero**. 1,419 explicit `Dispose()` sites in `tests/` and the documents (gtest 281 files, lit 75, rule corpus 70), 29 gtest field disposals in 6 files, one dispose-then-assign rule case (`s28_7_0004`) and one proposal sketch (0005): every one keeps its verdict and its observable output; what moves is when a block is freed in an unchecked build. No test asserts a use-after-free crash after an explicit disposal in an unchecked build (`tests/codegen/*dispose*` with `unchecked` lists flag-mode tests only); LeakSanitizer sites are unaffected, the free still happening at a point every holder reaches. Direct `DisposeAsync()` sites (critic re-measure): **0** gtest program sites (the four gtest hits `async_test.cpp:1602,1637`, `…_4326:109`, `…_4328:238` are comments), 0 conformance-doc program sites (`spec_0235`, `spec_0255` are scope-end comments), 3 rule cases (`s28_12_0005`, `s28_12_0006`, `s28_6_0031`) plus the rejected `this.DisposeAsync()` of `s28_6_0033`: none accesses the object between the call and its await other than a second `DisposeAsync()` (`s28_12_0006`, a no-op under "has begun" as under "has run": verdict unchanged).
- **Item 4 (C3).** Explicit `Dispose()`/`DisposeAsync()` on a name declared `static owned` in the same file: **zero** in 104 declaring files across gtest, lit, both conformance corpora and the documents (recipe: per file, every `static owned [readonly] NAME:` name, then `NAME.Dispose(`/`NAME?.Dispose(`/`NAME!.Dispose(`/`…DisposeAsync(` with any receiver prefix). `src/` hits are two comments.
- **Item 5 (M).** Mutable static owning-inline-array fields: gtest **2** (`static_class_enum_owned_field_1557_1564_test.cpp:194`, `static_owned_field_disposal_1212_test.cpp:179`), lit **2** (`sema_static_domains_1393__rule2_form4_mutable_static_owning_array_field_warns.bzl`, `sema_static_class__static_owning_inline_array_field_ok_1557.bzl`), rule corpus **3** (`s15_1_1_0008`, `s22_13_0015`, `s28_7_0008`), conformance-doc 0, guides 0, code-generation reference 0, proposals 0. `move` of a static slot: 0 anywhere. A slot store through a `static readonly` owning array: 0 (the one form-(h) declaration, `static_readonly_residual_3290_test.cpp:189`, only declares). `[N owned]` in general: 474 gtest hits, untouched.

## 7. Compiler facts and the trackers

- **Tracker 1 (item 1, `spec-tracking`).** The explicit-disposal site reclaims at the call and nulls the slot (`LlvmBackend_Calls2.cpp:5772`); the spec now says it marks only and the holder's release point frees, with the slot keeping its reference, and a later read of the field meeting the flag, not a null. Observable delta in a checked build: a later field read after `_r.Dispose()` faults `NullReferenceException` today where §28.6 (unchanged in this) says `DisposedAccessException` — an existing delta this proposal does not create; in an unchecked build the free moves later and nothing is observable. The DisposeAsync-in-flight deferral (reclaim at the later of the holder's release and the terminal state) is to be checked against `emitReclaimBlock`'s `bz_wait_for_borrows` (`LlvmBackend_Disposal.cpp:2869–2885`), which already waits for the async frame's `this` borrow — HYPOTHESIS: it may already implement the deferral. The flag is set before the core runs (`LlvmBackend_Disposal.cpp:68–84`), matching item 2; the member-entry guard is callee-side (`LlvmBackend_Setup.cpp:10884–10900`, every instance method; the accessor guards alike) and reads the one flag set before the core, so a destructor body's call to an instance method of `this` faults in checked mode today (code-read, not probed) — under item 2 the guard reads *completion*, so the flag word needs both moments (begun: idempotence; completed: the guard), and a user `DisposeAsync`, itself an instance method with the entry guard, must set *begun* without tripping its own guard. `bz_wait_for_borrows` (`LlvmBackend_Disposal.cpp:2869–2885`) is a **blocking** wait on the releasing thread: it is not item 1's deferral, which must not block a domain continuation whose task the wait would need (§30.3) — the release belongs to whichever of the holder's release point and the terminal state comes later.
- **Tracker 2 (items 4 and 5, `spec-tracking`).** The explicit-disposal receiver check does not reject a `static owned` field read (`Ownership.cpp:7514–7520` records the axis as latent); sema admits the mutable static owning-inline-array field with a warning (`TypeResolution_Checks.cpp` rule-2 form (iv) arm; the two lit pins) and must reject it, and must reject a slot store or `move arr[j]` through a `static readonly` owning array. The two gtest programs and two lit programs flip (§8).
- No runtime change is needed for the exit pass (`bezel_runtime_refcounting.cpp:35–88` already dispatches the idempotent `Dispose()` and frees once).

## 8. Corpus cases to flip or write (after landing; never edited by this lane)

- `s22_13_0015` (banked `pass`): states form (iv) admitted with a warning — now `expect: reject` naming §22.12 rule 2's closing sentence; the `rule:` header re-quotes the row.
- `s15_1_1_0008` (banked `pass`): the pool becomes `public static readonly Pool: [2 owned]Connection := …`; the verdict and output stay; the `rule:` header re-quotes the `static readonly` sentence.
- `s28_7_0008` (banked `fail divergence`): unwritable — the form it exercises no longer exists; deleted with its reason in the changelog (process §5 3c).
- `s11_7_0011`, `s28_12_0006`, `s28_12_0005`: header refresh only (the §11.7 sentence; "after either has begun"; "the cascade and the chain follow" for "reclamation follows"); verdicts unchanged.
- New cases to write: a slot store through a `static readonly [N owned]T?` rejected; `move T.arr[0]` rejected; `T._m.Dispose()` inside the declaring type rejected; a second `DisposeAsync()` during the first's window returning a completed task (run); the cascade after an explicit field disposal under `--unchecked-disposal` with ASan (run, no fault).
- gtest/lit to flip: `static_class_enum_owned_field_1557_1564_test.cpp:194` and `static_owned_field_disposal_1212_test.cpp:179` (rewrite the pool as `static readonly`), the two lit files above (the first becomes a rejection pin, the second a `static readonly` acceptance). Diagnostic-text renames (verdict unchanged, needles to grep in gtest and lit): `sema_static_domains_1393__rule2_owning_array_autoprop_diagnoses.bzl` and `…_plain_array_autoprop_diagnoses.bzl` pin "(forms (iii)/(iv))" (`TypeResolution_Checks.cpp:15294`); the form-(iv) warning text at `:15306` goes with the form.

## 9. Declined alternatives (one `declined-at-proposal` row each)

1. **C, a per-location released bit.** A second runtime state for the fact the flag records, tested at the cascade, the overwrite and the exit pass; the local join still reads the flag of a released block. *Reopening bar:* none stated.
2. **D, the repeated call undefined in unchecked mode.** The language, not the program, issues the second call, and §28.2's identical-behaviour claim would stop holding for §28.6's and §28.7's idioms. *Reopening bar:* none stated.
3. **E, vacate at the call.** A second vacated state for non-nullable owned fields and a null fault where the chapter reads the flag; silent on the in-flight `DisposeAsync` window. *Reopening bar:* a run-time vacated state adopted for non-nullable owned fields generally.
4. **F, reading B of #5758 with a trust clause.** No backstop can fault the disposal's own accesses. *Reopening bar:* none stated.
5. **G, a joined second `DisposeAsync()` task.** A second handle on a task the first caller owns; the completed task is the no-op. *Reopening bar:* a measured idiom that awaits a second `DisposeAsync()` for completion.
6. **H, faulting an alias inside the in-flight window while exempting the disposal's own accesses.** Not implementable by a member-entry flag test (the callee cannot tell the disposal's own call from an alias's), and the fault buys no memory safety inside a window whose block and owned fields are intact. *Reopening bar:* none stated.
7. **I, keeping the `static owned` receiver and recording the race.** A trust clause needs a backstop; across domains there is none (K4.3 clause 3, proposal 0151). *Reopening bar:* a cross-domain backstop for a static's disposal.
8. **J, per-type admission of early static disposal.** A per-type carve for zero sites; §30.12's detection covers live guards only. *Reopening bar:* a measured idiom that must dispose a global before exit and cannot spell it through `Locked<T>`, `Shared<T>` or a `Close()` method.
9. **K, form (iv) kept with `move arr[j]` barred.** Leaves the slot-store verdict and the re-population open. *Reopening bar:* none stated.
10. **L, form (iv) kept with a stated per-slot drop and an extended exit drain.** A third overwrite regime and a second drain rule for a form with no use outside its tests. *Reopening bar:* a real program needing a mutable process-global owning inline array that `OwningArray<T>` or `Locked<T>` cannot spell.
11. **Rejecting the `static readonly` owning inline array as well (removing note (h)).** It is disposed at exit, holds only what it was initialized with, and one gtest program uses it. *Reopening bar:* none stated.

## 10. Self-critique (adversarial pass)

1. **Does item 1 change checked-mode cost?** No. A checked build already retains every disposed block (rationale §5.30); the proposal moves nothing there. In an unchecked build the only move is later frees; the quarantine-latitude sentence of §28.2 stays because surviving borrows past the release point still need the flag in checked mode — SIMPLIFY C10's "the quarantine sentence can go" is wrong on this point and is not taken.
2. **Is "the holder's release point" defined for every receiver §28.6 admits?** Local, `using var`, `owned` parameter: scope end (the explicitly-disposed binding still has a release point; only its obligation is discharged). Unbound temporary and `move` result: the K8 boundary. `owned` field and owning-inline-array slot: the containing object's teardown or the overwrite. Owning-collection slot read by the declaring type: the container's cascade or removing operation. `static owned` field: no longer a receiver. The list in §28.2 names each.
3. **The maybe-consumed join.** `if c then x.Dispose(); end;` then scope end: the scope-end free dispatches the idempotent `Dispose()` (a no-op on the disposed path) and reclaims — the flag it reads is live on both paths. Under the status quo it read a released block on one path in an unchecked build; this is the local face of #5759, closed by the same rule.
4. **A long-lived holder keeps a dead block.** An owned field disposed early in a process-lived object holds its block until the object's teardown. The destructor has already released the real resource; what lingers is the block. The remedy for a hot path is the existing one — a smaller scope, or `_f := null` on a nullable field (drop-on-overwrite is a release point). Recorded as the cost.
5. **Begin versus completion, and the destructor body's own calls.** The flag records both moments: the beginning drives idempotence, the completion drives checked mode's fault, so the disposal's own accesses need no exemption and no caller context. The compiler sets its one flag before the core (§1.5) and guards callee-side, so a destructor body's `this.Helper()` faults in checked mode today (code-read) — a delta named in tracker 1. (The lane's first draft faulted aliases from the beginning and exempted the disposal's own accesses; the critique found that unimplementable and it is option H.)
6. **Does item 4 strand a real idiom?** A `Close()`/`Flush()` method on the static's type, a `Locked<T>` registry whose guarded value is replaced, or a `Shared<T>` handle spell early shutdown; zero sites measured.
7. **Does item 5 break the `static readonly` form's own uses?** The one gtest declaration only declares; `static readonly` already admits no whole-field reassignment, and the spec's readonly-field rule (Appendix C.5) names no slot write as an assignment of the field — which is why the fixed-slots sentence is stated, not derived. Rationale §12.15 already recorded that "restoration is per-slot assignment … so `readonly` is not this form's alternative spelling", i.e. that a `readonly` array admits no per-slot restoration.
8. **Composition with W4 (C6, C7, RC30).** W4's narrowing of the K9.2 window and the slot-move index rule touch §28.8.1 and §28.11's conservative-state paragraph; this proposal touches §28.11's receiver sentence only. RC30 (reassignment of an explicitly-disposed local) composes: the overwrite is the holder's release point and reclaims there. W4's runtime-null sentence for a vacated slot (L2 RC1) is a `move`-side rule and does not contradict item 1, which keeps the reference in an explicitly disposed slot.
9. **History narration.** The normative sentences state the rule as it now stands; the options and the issues' recommendations live here and in the rationale.
10. **Matrix.** Items 1–2 close R1/C9, R3/C9, R4/C9, R6/C9, R1/C14, R2/C14, R4/C14; item 4 closes R9/C9, R9/C13, R9/C14; item 5 closes R9/C6 and the R9/C12 route, and returns R9/C2, R9/C5, R9/C8, R9/C10, R9/C11 to the pilot's n/a for the slot leg (the whole-field legs were decided). The cell-by-cell record is `scratch/v0334/W3.md`.
