# Proposal 0171 — A borrowed Pattern B award spends the task; no static is declared at `OwningTask<T>`

| | |
|---|---|
| **Status** | graduated-v0.335 — superseded — graduated v0.335 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues", the 2026-10-09 direction "resolve all findings autonomously" and the 2026-10-09 "Please proceed as you recommend" after the v0.334 matrix re-runs: options D and G — the award of a borrowed `OwningTask<T>` operand spends the task (§30.4's state) and disposes nothing, the holder disposing it as it disposes any task it holds, finding no result, and the fault instance then; `OwningTask<T>`/`OwningTask<T>?` is a compile error in §22.12's `static readonly` column (note (j)) (#5834); critique round 1 applied, shared with the lane's three rulings (13 fixes), among them two unedited contradicting passages (§30.11's source accounting, §22.13's fixed exit code), note (j)'s category (a class type under rule 4, not the residual), an unsanctioned static `Task<T>` remedy, the superseded v0.190 #1785 parenthetical, and the in-flight-advance pass's synchronous fallback, which would have voided a live `TaskScope`; landed with the v0.335 changelog entry) |
| **Tracking issue** | [#5834](https://github.com/s8lap/bezel-lang/issues/5834) (`spec-defect`, soundness: a Pattern B await of a borrowed `OwningTask<T>` operand disposes it in place, against 0168's retention of an escaping fault and against K9.4's exit-pass-only disposal of static owned storage); one `spec-tracking` compiler tracker is filed at landing (§7) |
| **Target module(s)** | **spec**: §30.5 (the award sentence after the owned-consuming contexts; the borrowed-operand paragraph), §30.9 (the retention paragraph's borrowed-operand list), §30.11 (the source-kind disposal accounting), §28.3 K4.2 (the closed kill set names the owned operand), §22.12 (the `OwningTask<T>?` table row, rule 4's class-type sentence, note (i), a new note (j)), Appendix C.11 (the statics bullet). **stdlib**: none. **rationale**: §12.15 (one dated paragraph). **code-generation reference**: §4.3 (the Pattern B row), §7.6 (one line in the existing *Looks right, is rejected* fence and its note). **`DECLINED.md`**: four `declined-at-proposal` rows (§9) |
| **Written against** | spec/stdlib **v0.334** at `fd8a9a2` (main). Section numbers are v0.334's. |
| **Origin** | The disposal matrix's second run, lane L5 (RC-L5-R2 and RC-L5-R3; cells R7/C10 and R9/C10), consolidated as root NR03 and filed as #5834. It needs a proposal because it changes a stated run-time rule — §30.5's "When the operand is borrowed, the await still disposes the task implicitly" — and narrows a stated admission, the `static readonly` class-type row's admission of `OwningTask<T>` under rule 4's warning. |

*Reference convention: unprefixed §-references are language-spec references at v0.334; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

Pattern B's award is single: "the first await spends the task regardless of outcome" (§30.4). For an **owned** operand — a plain owned local, an `owned` parameter, a drained iteration variable, an unbound temporary — the await is also the operand's discharge, a K4.2 site-announced kill (the consume table's K3.5 row): the task is disposed at the award. For a **borrowed** operand the text kept the kill and dropped the discharge: "When the operand is borrowed, the await still disposes the task implicitly per the runtime semantics, but no discharge is owed for it" (§30.5). A disposal through a borrow — of a `using var`, a plain parameter, an `owned` field read without `move`, an owning-collection slot, static storage — is what neither proposal 0162 (explicit disposal marks; the holder frees; static owned storage is disposed only by the exit pass) nor 0168 (a faulted task retains the operand task whose fault it carries) revisited, and it collides with both.

**Face 1 (R7/C10).** §30.9 times the await's disposal of its operand as "its terminal act, after handling completes", and for a fault that escapes the body "handling completes" at the capture; so the borrowed operand task and the instance it owns are disposed at the capture — while the same section's retention paragraph says the faulted task "carries it as a borrow", "retains the operand task", and that "every awaiter of a multi-awaited faulted task re-raises the same live instance". The language's own await breaks the obligation the paragraph states; the K4.3 flag then reads a released block.

**Face 2 (R9/C10).** K9.4 and §28.6 make static owned storage "a borrow of K2's always-live source, which only §22.13's exit pass disposes"; 0162 withdrew the explicit `Dispose()` of a `static owned` field on exactly that ground. A `static owned readonly _t: OwningTask<Res>`, admitted by the `static readonly` class-type row under rule 4's warning, is disposed in place by `using var r := await _t;` — before the exit pass, on the *success* path too. Nothing leaks within one domain (the spent flag backstops a second await, the exit pass's call is the no-op), but the stated moment is false, and across domains it is the posture #5760's ruling declined for `Dispose()`.

The decision removes the kill rather than patching its collisions: **the award of a borrowed operand spends the task and disposes nothing.** The task stays its holder's, which disposes it as it disposes any task it holds — a `using var` at scope end, a field by the cascade, a slot by the collection, static storage by the exit pass — finding no result to dispose and disposing the fault instance then (§30.9's "at the later of the task's disposal and its settlement"). That is what §30.4's spent state already is — a property of the result buffer, not of the task's life — and what the compiler has done for a bound operand all along (§1.3). With the kill gone, face 1's retention paragraph applies to a Pattern B operand verbatim and face 2 has no soundness defect left. The static face is narrowed anyway, on the owner's standing preference for narrowing a feature of negligible reach over carrying a rule: `OwningTask<T>` leaves the `static readonly` column as note (j), on form (ii)'s own ground — a process-global single-award task is awarded once, to whichever domain's await arrives first, and raises at every other, which is no storage at all. Measured reach: zero sites. No program's verdict moves.

## 1. The problem

### 1.1 Face 1: the retained operand the await disposes

```bezel
class Res destructor Console.WriteLine("closed"); end destructor; end Res;
class Holder
  private owned _t: OwningTask<Res>;
  public constructor(owned t: OwningTask<Res>)  _t := move t;  end constructor;
  public async method Run(): Task
    var r := await _t;   // borrowed Pattern B operand: v0.334 disposes `_t` in place,
  end Run;               //   "as its terminal act, after handling completes" — at the capture
end Holder;
class Demo
  public static async method Bad(): OwningTask<Res>
    await Task.Delay(10);
    throw new InvalidOperationException("boom");   // Bad's task owns the instance (§30.9)
  end Bad;
  public static async method Main2(): Task
    using var h := new Holder(Bad());
    using var o := h.Run();          // Run's task retains `_t` by borrow and carries the instance (0168)
    for var i in 0..<2 do            // each awaiter re-raises "the same live instance" …
      try await o; catch var e: Exception => Console.WriteLine(e.Message) end try;
    end;                             // … which Run's await already disposed with `_t`
  end Main2;
end Demo;
```

§30.5: "When the operand is borrowed, the await still disposes the task implicitly per the runtime semantics, but no discharge is owed for it." §30.9: "an await that consumed its operand task (§30.5) performs the deferred task disposal as its terminal act, after handling completes", and "for a fault that escapes into a task, 'the exception's handling completes' at the fault." §30.9's retention paragraph, for the same operand: "A **borrowed** operand — a plain task parameter, a task read from a field or static storage, an owning-collection slot, a borrowing-iteration variable — is retained by **borrow**, under §30.1's obligation extended as the borrowing aggregators' is: the operand task must outlive the faulted task's settlement and every await or `Result` read of it". Only the `using var` face carries a sentence — "A `using var`/`await using var` operand's disposal is **deferred** to the faulted task's teardown" — that can be read to cover the in-place disposal; the field, slot, parameter and static faces have none. The expected output under the retention paragraph is two `boom` lines and nothing else (no `Res` is ever constructed, so `closed` never prints); under the await's terminal act the second awaiter reads a released instance — observable only under a sanitizer, or through an exception subclass whose destructor reports its own disposal.

### 1.2 Face 2: static owned storage the await disposes

```bezel
class Res destructor Console.WriteLine("closed"); end destructor; end Res;
class Cache
  private static owned readonly _t: OwningTask<Res> := Make();   // admitted: rule 4's warning
  private static async method Make(): OwningTask<Res>  await Task.Delay(1); return new Res();  end Make;
  public static async method Take(): Task
    using var r := await _t;   // a `static owned` read is a borrow (§28.6): "the await still disposes" —
  end Take;                    //   in place, before exit; K9.4: "disposed only by the exit pass"
  public static async method Main2(): Task
    await Take();
    await Take();              // InvalidOperationException (spent); the exit pass's call is the no-op
  end Main2;
end Cache;
```

§22.12 note (h): "static owned storage is disposed only by the exit pass, and no per-method window can cover it" (K9.4). §28.6: "a `static owned` field read (a borrow of K2's always-live source, which only §22.13's exit pass disposes — K9.4)". §30.5's borrowed-operand sentence disposes `_t` at the first `Take()`.

### 1.2a The prior reading this supersedes

The v0.190 resolution of [#1785](https://github.com/s8lap/bezel-lang/issues/1785) scoped the static consume to plain owned bindings and recorded the `using var` operand's await as "the borrowed-operand case (implicit disposal at the await; second await faults at runtime; scope-end dispose is an idempotent no-op)" (changelog v0.190). Its scoping stands; its parenthetical's *implicit disposal at the await* and *idempotent no-op* are what this proposal retires, together with their source-side twin in §30.11's *Task-object ownership* paragraph ("an await through the borrow disposes the task per §30.5's borrowed-operand rule, and the source's own later disposal of it is §28.2's idempotent no-op"). No `DECLINED.md` row records a decline of the spent reading.

### 1.3 The compiler (`fd8a9a2`)

- `src/codegen/LlvmBackend_Async.cpp:4886–4925`: at an await, `freeTaskAfter` is true only for an anonymous producer operand (`await Producer()`); "A task bound to a local is left to its binder's disposal." For a bound `OwningTask<T>` operand `markSpentAfter` is set and `takeResultAfter` nulls the result buffer after the award; `:5170–5180` raises `InvalidOperationException` on a spent task; `:5213–5233` marks a bound task's re-raised fault instance borrowed. The compiler therefore already **spends and does not dispose** a bound or borrowed operand — the text, not the implementation, is what this proposal changes for face 1. The evidence is not discriminating, and is stated as such: `freeTaskAfter` is false for an *owned* named operand too (a plain owned local, an `owned` parameter), whose task the lowering likewise leaves to the scope-end drop rather than disposing at the award (`Ownership_Checks.cpp:18265–18300` records the static consume; HYPOTHESIS: the owned-operand disposal moment is the scope end in the implementation, a tracker probe, not a spec question). **Measured**: the behaviour is read off the lowering; no test pins the disposal moment of a borrowed Pattern B operand (grep `tests/` for an exception class with a destructor awaited through an `OwningTask`: `async_test.cpp`, `task_combinators_test.cpp`, neither at a borrowed operand's fault).
- `src/runtime/bezel_runtime_refcounting.cpp:36–96`: the exit pass disposes every registered `static owned` object once; a spent task's `Dispose()` there finds no result.
- `tests/lit/sema/sema_static_domains_1363__rule2_mutable_static_owningtask_rejected.bzl` and rule case `s22_12_0014` pin the **mutable** `OwningTask<T>?` static's rejection; no test, document or corpus program declares a `static readonly` or `static owned readonly` of `OwningTask<T>` type.

## 2. Measurement

- **Face 1's reach.** Newly rejected: nothing (no compile-time rule changes). Newly admitted: nothing. Run-time semantics changed: the disposal moment of a borrowed Pattern B operand task moves from the award to the holder's own release — a `using var` at its scope end, a field at the cascade, a slot at the collection's disposal, static storage at exit. Observable only through a fault instance with a destructor awaited through a borrowed operand, or a sanitizer's view of the task block; `tests/`, the rule corpus (`s30_5_*`, `s30_9_*`, `s30_4_*`), Guide 04 and the code-generation reference pin none (the corpus headers quote no sentence this proposal rewrites; Guide 04 §3 already describes the `using var t := …; using var tok2 := await t;` retry as "already spent").
- **Face 2's reach.** `grep -rn "static" tests/ docs/ | grep OwningTask`, declarations only: the mutable rejection's lit test and rule case `s22_12_0014` (both stay rejected, now by two rules), the matrix run records (research, non-normative), and nothing else — zero `static readonly`, `static owned readonly` or static auto-property of `OwningTask<T>` type in `tests/` (gtest strings, lit), `tests/conformance/` (rule and doc corpora), the guides, the stdlib or the code-generation reference. The generics matrix's R8/C11 note ("A `static readonly T` at … `OwningTask<X>` is a class-type substitution: rule 4's warning") describes a shape no program writes; it moves to rejected-at-instantiation under note (i).

## 3. Options

Face 1:
- **A — status quo.** Unsound (§1.1).
- **B — defer the await's in-place disposal** of a borrowed Pattern B operand to the faulted task's teardown when the fault escapes, the task spent at the await (#5834 option (1), the issue's recommendation).
- **C — reject** a Pattern B await of a borrowed operand in an `async` body unless a `try`/`catch` handles every fault (#5834 option (3)).
- **D — the award spends; the holder disposes.** The borrowed-operand await is not a kill: it spends the task and disposes nothing; the holder disposes the task at its own release, finding no result, and the fault instance with it (this proposal). (#5834 option (2), a fresh cause-chained re-raise, is 0168's declined E3 and is not re-weighed.)

Face 2:
- **E — a compile error at a Pattern B await whose operand is static owned storage**, naming K9.4 (#5834 option (b)).
- **F — record the await as K9.4's one admitted in-place disposal** of static storage, single-domain under rule 4 (#5834 option (c)).
- **G — `OwningTask<T>` leaves the `static readonly` column** (#5834 option (a), the issue's recommendation).
- **H — D alone**: with the kill gone, face 2 has no soundness defect; keep the admission.

## 4. Evaluation

| | Soundness | Simplicity | Consistency | Migration reach |
|---|---|---|---|---|
| B | closes the escaping-fault face; the success path still disposes through a borrow (face 2 stays contradicted), and the faulted task disposes a task it does not own, ahead of or after the holder's cascade | one clause, but a third disposal moment for one operand kind | keeps a K4.2 kill that K9.4 and §28.6 say cannot exist for static storage | 0 |
| C | closed by rejection | a new rule | rejects §30.5's own canonical two-liner `using var task := …; var x := await task;` wherever a fault may escape | every borrowed Pattern B await in an `async` body without a total handler |
| **D** | closed on every path: one owner, one disposal, the retention paragraph applying to a Pattern B operand as to any borrowed operand | removes a rule (the borrowed-operand kill) and its no-op story | §30.4's spent state is the only state the award changes; K3.5 (the owned-operand kill) unchanged; what the compiler does | 0 |
| E | closed for statics only | a new rejection at the use | rejects the use and keeps a declaration with no coherent meaning | 0 |
| F | a carve in K9.4 for a form with no sites | a new K9.4 clause | reopens for the await the cross-domain posture #5760's ruling declined for `Dispose()` | 0 |
| **G** | — (D already closes it) | removes an admission; one table cell, one note | form (ii)'s own ground, now stated once for both columns; (g)'s shape for the bare-parameter substitution | 0 (measured) |
| H | sound | nothing added | keeps a process-global single-award task admitted under a warning whose obligation ("confine to one domain") is the opposite of what a static is for | 0 |

## 5. Decision (delegated ruling, 2026-10-08)

**D + G.** Normatively:

1. ▲ §30.5, the sentence after the owned-consuming contexts: "An **owned** operand task is disposed implicitly by the award, without disposing its result; a **borrowed** one is spent and stays its holder's (both below); either way the task cannot be awaited again, and a second await raises `InvalidOperationException`."
2. ▲ §30.5, the borrowed-operand paragraph: "When the operand is **borrowed** — a `using var` binding, a plain task parameter, an `owned` field read without `move`, an owning-collection slot, a borrowing-iteration variable, static storage — the await **spends** the task (§30.4: the first award, whatever its outcome) and disposes **nothing**: the award is not a kill, no discharge is owed for the operand, and the task stays owned by its holder, which disposes it as it disposes any other task it holds — a `using var` at its scope end, a field by the object's cascade, a slot by the collection, static storage by §22.13's exit pass alone (K9.4). A spent task's later disposal finds no result to dispose; its fault instance, where it has one, is disposed then (§30.9's moment), which is what lets a fault that escaped the awaiting body stay retained by borrow (§30.9 *Fault ownership*). The `using var task := SomeFactory(); var x := await task;` shape is one common form: the await awards the result, and the `using var`'s scope end disposes the task."
3. ▲ §30.9, the retention paragraph's borrowed-operand list names the Pattern B operand "whose award spends it and disposes nothing (§30.5)".
4. ▲ §22.12: the table row `OwningTask<T>` / `OwningTask<T>?` reads **error — 4(j)** in the `static readonly` column; note (j) states the rule and its ground (single-award, process-global; the award through a borrow spends; the shapes: a local or an owned instance field for a once-awarded acquisition, `Task<T>` for a shared process-lived result); rule 4's class-type sentence names the exception (`OwningTask<T>` is a class type, so the exception belongs to rule 4's class-type admission, not to the residual's value-kind count, which stays "Three kinds"); note (i)'s class-type clause excepts an `OwningTask<X>` substitution of a bare-parameter static, rejected at the instantiation on (j)'s ground; note (j)'s closing parenthetical states that a task an admitted class-typed static holds *inside* it (a field of a `static owned readonly` object, a slot of a static owning collection) is not (j)'s — its holder is that object, which the exit pass disposes, and an await of it is the borrowed award. Appendix C.11's statics bullet names the rejection at its class-type clause.
5. ▲ Rationale §12.15: one dated paragraph (the grounds, the measurement, the declines).
6. ▲ Code-generation reference: §4.3's Pattern B row gains "a **borrowed** operand (a `using var`, a plain parameter, a field, a slot) is only **spent** — its holder still disposes it"; §7.6's existing *Looks right, is rejected* fence gains the static's line and its note the gloss (the right-hand fence's `Task<Config>?` cache line is already the remedy).
7. ▲ §30.11 *Task-object ownership*: an await through an `OwningTaskCompletionSource<T>`'s `Task` borrow spends the task and disposes nothing, and the source's own later disposal is the task's single disposal, finding no result.
8. ▲ K4.2's closed kill set: "Pattern B `await` of an owned operand (a borrowed operand's award spends the task and kills nothing)".

Kernel classification: **derivation** — K1 (one owner per link: the holder), K3.5 unchanged (the owned-operand kill), K4.2's closed set unchanged (the Pattern B `await` row is the owned-operand row; the borrowed award was never a discharge and is now not a kill), K9.4 restored for static storage without a clause. Body-locality: no compile-time rule added; the (j) rejection is a declaration-site rule keyed to the declared type, and the (i) extension an instantiation-site rule in §3.6 rule 2's existing shape.

## 6. Edits at graduation

The exact edits are `scratch/v0335/X2.json` ids `X2-0171-5834-*` (spec §30.5 ×2, §30.9, §30.11, K4.2, §22.12 ×4 — table row, rule 4, note (i), note (j) — Appendix C.11; rationale §12.15; code-generation reference §4.3, §7.6 ×2; README row; four DECLINED rows). Seams: none with lanes X1, X3 or X4 (§30.5's borrowed-operand paragraph, §30.9's retention list and §22.12's tables are touched by no other v0.335 lane as briefed; X1's #5799 touches §28.5, not §22.12). The critic's K4.2 edit sits in §28.3's kernel (item 2, *Site-announced kills*), X1's section family; its `old` is that item's own closing clause, and X1's roots (§28.5/§28.8.1/§28.10/§28.11 windows) do not name it — flagged for composition. *(Composed at v0.335: lane X4's #5844 item 8 adds the terminal-state order to §30.5's *Disposing an unsettled task* — the frame's `owned` parameters, the continuation's disposals, then the block — and its #5801 item 2 extends §30.9's consumed-operand refinement with the escaping-instance exception; both are other sentences of the two sections and read with this proposal's: the consumed-operand refinement is the owned operand's, the borrowed award disposing nothing.)* Composition with 0168: unchanged — the retention paragraph's `using var` deferral (A12's shape) still governs a `using var` Pattern B operand whose fault escapes, and the borrowed clause now governs the field, slot, parameter and static faces without a competing disposal. Composition with 0162: the holder frees, exactly as for any task it holds; a spent task's block is released at the holder's release point.

## 7. Compiler facts and the tracker

- **Tracker (spec-tracking, one):** (i) §22.12 note (j): reject a `static readonly`/`static owned readonly` field or auto-property of `OwningTask<T>`/`OwningTask<T>?` type at the declaration, and a bare-parameter static's `OwningTask<X>` substitution at the instantiation (today both are admitted with rule 4's warning — `src/sema`'s statics verdict, the row the mutable rejection already reads); (ii) probe §1.1's program under ASan: expected two `boom` lines and no `closed` (no `Res` is constructed), no use-after-free, no leak — the lowering already spends without freeing (`LlvmBackend_Async.cpp:4886–4925`), so this is a conformance probe, not an expected failure; (iii) probe §1.2's program rewritten over §22.13 pattern (b)'s `Task<Res>?` cache — the shape note (j) names — to confirm the exit pass disposes the cached task and its result once; and a `static owned readonly` `OwningArray<OwningTask<Res>>` whose slot is awaited (a borrowed award: spent, the exit pass's cascade disposing it once).

## 8. Corpus cases to write (after landing)

- `s30_5_*`: a `using var t := Factory();` awarded once, then `t` disposed at scope end (run: a second await in the same scope raises `InvalidOperationException`, the result's destructor line after the block's last statement, not at the award); a borrowed `owned` field operand awarded in a method, the holder's cascade disposing the task (run).
- `s30_9_*`: the §1.1 program (run: `boom`, `boom`; no `closed` — the use-after-free this closes is a sanitizer-only property, a gap to record).
- `s22_12_*`: `private static owned readonly _t: OwningTask<Res> := Make();` (reject; diagnostic: `22.12`); `class Cell<T>` with `public static readonly V: T` named at `Cell<OwningTask<Res>>` (reject at the instantiation); `s22_12_0014` unchanged (the mutable form, now rejected by two rules — its header stays on form (ii)).
- Headers unchanged: `s30_4_*`, `s30_5_0008`, `s30_9_0001`–`0004` (none quotes the rewritten sentences).

## 9. Declined alternatives (one `declined-at-proposal` row each)

- **B** — deferring the borrowed-operand await's in-place disposal to the faulted task's teardown. Reopening bar: a shape in which a holder must not see its spent task disposed at its own release point.
- **C** — rejecting a borrowed Pattern B await in an `async` body unless every fault is handled. Reopening bar: —(none stated).
- **E** — a compile error at an await whose operand is static owned storage. Reopening bar: —(none stated).
- **F** — recording the await as K9.4's one in-place disposal of static storage. Reopening bar: real code holding an `OwningTask<T>` in static storage, with a stated cross-domain award protocol.

(**H**, D alone, is not a decline row: it is the status quo of the `static readonly` cell, superseded by G on the owner's narrowing preference and recorded in rationale §12.15's paragraph.)

## 10. Self-critique (adversarial pass)

1. **Does a holder's disposal before the faulted task's settlement break the retention?** A `Holder` disposed while `Run`'s task is still unsettled disposes `_t` and the instance it owns; the retained borrow then dangles. That is the K4.3 obligation the retention paragraph already states for every borrowed operand ("the operand task must outlive the faulted task's settlement and every await … of it"), flag-backstopped in checked mode; this proposal adds no new residual and removes the competing disposal. In §1.1 `h` is declared before `o`, so `o` is disposed first.
2. **A `using var` Pattern B operand whose fault escapes.** 0168's `using var` clause defers that binding's scope-end disposal to the faulted task's teardown (A12's shape); with the award no longer disposing, that deferral is the one disposal, exactly once. Unchanged.
3. **A plain parameter operand awarded in a callee.** Through v0.334 the callee's award disposed the caller's task; the caller's `using var` scope end was then the no-op. Now the caller's scope end disposes it. The caller sees the same spent state, a second await in the caller raising as §30.4 states; nothing the caller could observe before is lost.
4. **Does the owned-operand kill still need "disposed at the award"?** Yes, and it is kept: K3.5's post-state is moved-from, and the kill's flag backstops stale borrows (§30.5's site-announced-kill paragraph is untouched). The two operand kinds now differ exactly as they should — an obligation the frame holds is discharged at the award, an obligation someone else holds is not.
5. **`OwningTask<T>.Result` on a borrowed operand.** §30.3: the read "transfers ownership of the result to the reader exactly as `await` does — the result buffer is consumed, and a second read or `await` raises". The spent state is already the stated effect; no disposal was stated there, so no edit is needed and the two forms agree.
6. **Is G a design change on its own?** Yes — it narrows a stated admission (the class-type row's warning) — which is why it rides this proposal rather than a ruling; it is the owner's preferred direction for a feature of zero reach, and it is not what closes face 2's soundness defect (D is), so a critic who prefers H loses nothing sound by dropping G. The reason to take G is stated in §4 (a warning whose obligation is confinement, on a value whose only use is cross-domain first-come award).
7. **The bare-parameter substitution.** Note (i) already rejects an owning-choice substitution of a `static readonly T` on (g)'s ground; (j)'s is the same shape, so `Cell<OwningTask<Res>>` is rejected at every naming (§14.1's instantiation site), as the generics matrix's R8/C11 note now records. A `static readonly T` at `Task<X>` stays admitted with rule 4's warning.
8. **Appendix C.6.** Its Pattern B bullet already says "a second await a compile-time moved-from read on an owned operand and `InvalidOperationException` on a borrowed one"; it describes the spent state and needs no edit.
9. **The draining-iteration operand.** `for var task in taskArray.Drain() do results.Add(await task); end` is an owned operand (K3.5), unchanged; a *borrowing* iteration over `OwningArray<OwningTask<T>>` is the borrowed case: through v0.334 the award disposed the slot's task in place, leaving the collection holding a disposed task until its own cascade; now the collection disposes a spent task. Cleaner, and what the compiler does.
10. **Why not fold face 1 into a ruling and leave the proposal to face 2?** Because face 1 changes a stated run-time rule (the disposal moment and the kill), which §2 of the process makes a design change whether or not a verdict moves — the same ground 0168 gave for its own proposal.
11. **Statics reached through a holder (critic).** Note (j) bans the declared type only; a `static owned readonly` object with an owned `OwningTask<T>` field, or a static `OwningArray<OwningTask<T>>`, stays admitted under rule 4's warning, and an await of its task is the borrowed award — spent, disposed once by the exit pass's cascade (K9.4 intact). So face 2's soundness rests on D, not on G, on every path; (j) narrows the direct form only, and its parenthetical says so.
12. **§30.11 (critic).** The completion source's *Task-object ownership* paragraph restated the retired disposal ("an await through the borrow disposes the task … the source's own later disposal of it is … the idempotent no-op"); it is rewritten with the rest, so the source's disposal is the task's one disposal for both source kinds.
