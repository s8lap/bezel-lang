# Disposal matrix: a completeness audit of the spec's disposal rules (2026-10-09)

*Research record. Non-normative. The spec decides every rule; this audit only checks whether it does. The run files here are the single record of the audit, and the page is a generated view of them. The method is the [borrow-flow matrix](../borrow-flow-matrix/README.md)'s.*

This matrix asks whether every disposal obligation the spec creates is discharged exactly once on every control-flow path, and whether every use after disposal or transfer is rejected or caught by a recorded run-time check. A cell counts as *decided* only when current normative text settles it **and** the auditor could not write a program that escapes. Otherwise the cell is *missing*, *conflict* or *ambiguous*, comes with a counterexample, and is mapped to a root-cause issue.

## Runs

| Run | Spec | Base cells open | Route cells open | Outcome |
|---|---|---|---|---|
| 1 | v0.333 | 51 of 140 | 6 of 16 | 21 root causes, each filed as an issue (listed in the run file's `groups`); all decided in v0.334 (proposals 0159–0169) |
| 2 | v0.334 | 12 of 140 | 6 of 22 | 8 issues: explicitly-disposed reassignment vs 0162 (#5832), the collection window (#5833), borrowed/static `OwningTask` await (#5834), return-slot timing (#5840), `Environment.Exit` in the exit pass (#5841), in-flight async advance (#5842), and existing #5796, #5800 |
| 3 | v0.336 | 0 of 140 | 2 of 26 | targeted re-run (66 cells re-derived, 74 carried): the abandoned drop-on-overwrite's location (#5925, new, soundness) and a derived destructor's bare take of an inherited field read by the base destructor (#5913, second face); editorial residues of 0174 filed in #5926 |

## Files

- `matrix.json`: the page's title, rows and columns.
- `runs/v0.NNN.json`: one file per run: `cells` (the base cells, in the cell contract below), `routes` (route cells: a specific escape inside a cell), `groups` (the root-cause issues, each with the cells it opens).
- The page: `python3 docs/research/matrix-page/build_page.py docs/research/disposal-matrix scratch/disposal-matrix.html`.

## How a run is done

Five lanes, each owning a set of columns across every row, fill the cells; each lane quotes every deciding sentence verbatim (checked mechanically against the spec and stdlib), hunts for an escaping program per cell, and writes a counterexample of at most 25 lines for every open cell. A consolidation pass merges the lanes' findings, across matrices, into root causes, and each root cause not already covered by an open issue is filed. A re-run starts from the latest run file, re-derives every cell at the new revision, and records `prev_status`.

## 1. The question every cell answers

> **For an obligation of holder kind R that reaches path or event C: does current normative text (spec plus stdlib) decide**
> **(a) who discharges the obligation, that it is discharged exactly once, and at what moment on that path (or that the path is rejected, or that the obligation is waived by a recorded rule), and**
> **(b) for every use of the object that the path leaves possible after its disposal or transfer, whether that use is rejected statically or admitted only under a named runtime backstop: K4.1's flag in checked mode, a K4.2 site-announced kill, or a K4.3-recorded trust clause?**
> **And could the auditor NOT write a program that leaks, double-disposes, or uses an object after its disposal outside such a named backstop?**

"Obligation" means K1's disposal obligation: an owned class or interface value, a move-only value (owning inline array, owning choice), or the free obligation of a trivial-dispose instance (K1.6: the ceremony is waived, the tracking is not). A borrow's lifetime belongs to the borrow-flow matrix. Here it matters only as the *use after disposal* half of (b).

Exactly-once is K1.1 and §28.3: "An owned local is disposed exactly once on every control-flow path". A second `Dispose()` that §28.2's idempotence makes a no-op does **not** break exactly-once (the destructor runs once). A program that relies on that no-op to stay sound is still worth a note.

## 2. Rows: kinds of obligation holder (10)

| Row | Holder | Defining sections (read before filling) |
|---|---|---|
| **R1** | **Owned binding.** A plain owned local (fresh-produced, adopted by K1/§28.3's adoption clause, an `out owned` recipient, an owning iteration variable, an owning-match arm binding, a Pattern B `await` result), or an `owned` parameter of a **synchronous** body, lambda or constructor. | K1, K3 table, K5, K7, §28.3 (Ownership state of locals; Owned-local discharge requirement; Captured owned bindings), §22.3/§22.4, §28.8, §28.8.1 |
| **R2** | **Scope-pinned binding.** `using var`, `await using var`, anonymous `using var _`, and compiler-introduced slots of the same kind: the scoped await-bind's hidden slot, and the `for` desugaring's `__source`/`__enumerator` when they hold an owned value. The borrow-pin form (no obligation) is in scope only for (b). | §28.10, A5, A12, §28.12, §30.5 (scoped await-bind; Pattern A kind 2), §29.2 desugaring, §17.13 |
| **R3** | **Unbound owned temporary.** A full-expression temporary, a binding-atom named temporary (K8.5), a `case` scrutinee temporary (K8.3), an `await` or `.Result` operand temporary, an overwrite RHS left unbound by an abandoned store. | K8, §28.3 (Unbound owned temporaries; Deferred-activation callees), §17.15, §19, §28.7 |
| **R4** | **Owned instance field.** A class- or interface-typed `owned` field, an owned primary-constructor capture field, an owning-wrapper field (`Shared<T>`, an owning collection held as a container). The object's cascade is the discharge. | K9, §28.4, §28.5, §28.6 (incl. *Who may call Dispose()*), §28.7, §28.8.1, §22.7–§22.10, §23.3 `set(owned)` |
| **R5** | **Owning-collection slot.** An element of the owning family (`OwningArray`, `OwningDictionary` values, `OwningSet`, stacks and queues, `OwningLinkedList`, `OwningSlotMap`), a class-typed `OwningBuffer<T>`/`Span<T>` owned slot, an owning enumerator's advanced-but-untaken value. | §28.11 (slot policy; owning collections; `move coll[i]`), K3.1, K5.6, stdlib §4.2, §4.5–§4.14, §4.21, §29.1 |
| **R6** | **Move-only value.** An owning inline array `[N owned]T(?)` (local, parameter, instance field, payload) and its slots; an owning-classified choice (incl. `C?`) and its `owned` and move-only payloads. | §15.1.1, §15.3, K3.12, §35.3–§35.5 (owning match, disposal traces), §35.9, §9.4 (move-only receivers; disposed state), §19.3 |
| **R7** | **Runtime-held owned value.** A task's result slot (Pattern A kinds 1–3, Pattern B), the task object a completion source owns, the in-flight thrown exception (K3.8), a faulted task's owned fault exception, a superseded exception. | §30.3–§30.5, §30.9, §30.10, §30.11, §31.1 (throw operand; exception ownership), §31.2 (supersession, record-before-drop), A3, A6 |
| **R8** | **Deferred-activation frame state.** What an iterator, async or async-iterator state machine owns: `owned` parameters transferred into the frame, frame-resident owned locals across suspensions, enumerator-owned yields (minting and adopting), and the frame as an owned value (K6.3: calls mint). | K6, §29.2–§29.8, §29.11, §30.1, §30.6, §30.7, A15, §28.12 (implicit discharge is synchronous) |
| **R9** | **Static owned storage.** A `static owned readonly` field, a `static readonly` owning-inline-array field (note (h); the mutable form (iv) was retired at v0.334, so its cells read n/a from the second run), and, for (a)'s waiver, admitted non-`owned` statics holding fresh values (the §22.13 exemption). | §22.12 (rules 1–5, forms (i)–(iv), notes (g)–(i)), §22.13 (*Disposal of static owned fields*; exemption; abnormal termination), K9.4, §28.3 *Static stores*, §33.3 |
| **R10** | **Control: runtime-managed value.** `string`, `Type`, boxed value types, function values. Expected to be uniformly decided as *exempt* or *n/a* (K1.6, §28.9). A non-trivial cell here means the exemption leaks into an obligation-bearing path. Hunt for that, e.g. a box or closure that keeps an owned referent reachable. | K1.6, §28.1 clauses 2–3, §28.9, §8.6, §27.5 |

Not separate rows, by design (decided by the pilot pass):
- **`Shared<T>`/`Weak<T>` handles** are ordinary owned instances (§28.13: "every rule of this chapter applies to a handle unchanged"), so each handle is filed under the row of its holder (R1/R2/R4/R5). The one novel obligation, the shared underlying value released by the last handle, goes in **route cells** under C12 (last-drop inside a pass) and C13 (cross-domain last-drop).
- **Trivial-dispose** instances are a sub-shape of every row (K1.6: the free is tracked, the ceremony waived). Where the waiver changes a verdict, say so in the note.

## 3. Columns: disposal paths and events (14)

| Col | Path / event | Defining sections |
|---|---|---|
| **C1** | **Normal release.** The holder's ordinary end on the fall-through path: scope end and auto-drop (K3.14), object teardown (field), full-expression end (temporary), task terminal state, frame completion, program exit (static), with the trivial-dispose free at the same point. | K3.14, §28.3, §28.9, K8.1, §28.4, §22.13, §30.1, §29.8 |
| **C2** | **Early structured exit.** `return`, `break`, `continue`, `yield break`, including through nested `try`/`finally` and out of a `catch` arm; K9.2 restoration on those edges. | §28.10 (every exit path), §21.5, §21.7, §31.2, K9.2, §28.8.1 |
| **C3** | **Exception unwind.** Edge parity, the establishment-ordered cleanup stack with `finally`, a raise from inside the holder's scope, supersession by a throwing `finally` or disposer, and handling in a `catch` arm. | K3.15, K8.1, §28.3 (Exception-path discharge), §28.7 (ordering), §31.1, §31.2, §28.4 |
| **C4** | **Failed construction.** Instance construction that throws (base/`this` chain, field initializer, body, captures, `ensures`/invariant, a `using var` scope end after acquisition), an `owned` argument into a throwing constructor, and failed static initialization (failed type state, an initializer that threw, re-entrancy). | §22.7–§22.10, §28.4 (*destructor body … never on construction unwind*), §28.8.1 (raise from the consuming call), §22.13, §31.4 (`TypeInitializationException`) |
| **C5** | **Transfer.** The consume positions that move the obligation (K3.1–K3.4, K3.6, K3.7, K3.12): `owned` argument and return, store into an owned slot or `set(owned)`, `move x`, `move x.f`, `move coll[i]`, the marked arm transfer, `case move`, value-move of move-only values; the moved-from post-state and K9.2 restoration. | K3 table, K5.1, K9.1–K9.2, §28.3 (Move semantics), §28.8, §28.8.1, §28.11, §15.3, §35.5 |
| **C6** | **Displacement.** Reassignment and drop-on-overwrite (K3.11): state match, RHS-first, null and moved-from targets, the indexer-set, whole-value move-only reassignment, `x := null` on an owned `C?`, and the no-drop posture of statics. | K3.11, K5.2, K5.6, K9.5, §28.3 (State match), §28.7, §28.11, §15.3, §35.4, §22.12 (store semantics), §22.13 |
| **C7** | **Join.** Statement merges and expression arm-meets (`if`, `case`, `??`), K7's meet, maybe-consumed, per-path adoption and auto-drop, a conditional kill, `null`/`throw` absorbing arms. | K7, §28.3 (Ownership state of locals; Producers `??`/if rows), A7, §20, §19.2 |
| **C8** | **Loop.** The back edge: a move, kill or reassignment inside a loop body, a loop-carried owned binding, per-iteration temporaries (K8.2), owning iteration variables per iteration, a holder declared in the body versus outside it. | K7 (back edges), K7.4, K8.2, §28.3, §28.8.1 (Loops), §29.2, §21.3–§21.5, §28.14 v1 bound 2 |
| **C9** | **Explicit kill and use-after.** The K4.2 site-announced kills (`Dispose()`, direct `DisposeAsync()`, discard, `throw x`, Pattern B `await`) and what may follow them: the owned-receiver rule, explicitly-disposed post-state, §28.2's use-after-dispose warning versus the maybe-consumed error, early release of fields and slots, the element-borrow use-after-removal warning, and the freeze's site-announced exemption. | K3.9, K4.1–K4.2, K5.3, K5.7, K5.8, §28.2, §28.6, §28.11, §17.13, §30.12 |
| **C10** | **Await and cancellation.** A holder live across `await`: the Pattern A/B result obligation, a faulted or cancelled outcome, a task disposed before settlement, `CancellationToken`/source disposal, implicit discharge in an `async` body (always synchronous). | K3.5, K3.6, K6.4, §30.1, §30.2, §30.4, §30.5, §30.8, §30.9, §30.10, A3, A12 |
| **C11** | **Iteration.** `yield return` and the advance boundary, enumerator-owned versus transferred yields, partial consumption and abandonment (§29.7), `owned` parameters of iterator frames, multiple enumeration, the manual protocol's untaken pending value. | §29.1–§29.8, §29.11, §29.12, §30.7, K6.1, K6.3, K3.7, A15 |
| **C12** | **Multi-value cleanup pass.** Order and a throwing disposer when one event disposes several values: the destructor body then owned fields in reverse declaration order then the base chain, the owning-collection cascade order, inline-array slots in reverse, several locals in reverse declaration, temporaries in reverse production, the exit pass in reverse initialization, frame teardown, `Shared<T>` last-drop. Includes supersession accounting. | §28.4, §28.11 (Dispose order), §15.1.1, §28.3, K8.1, §22.9, §22.13, §29.8, §30.1, §31.2 (record-before-drop) |
| **C13** | **Cross-domain.** `Domain.Spawn` owned arguments and results, channel `Send`, sendable `Shared<T>` handles and the last drop in another domain, statics reachable from every domain, K4.3's domain clause (where no flag is ever set). | §30.13–§30.15, §28.13 (Sendability), §28.3 (domain clause, cl. 4–5), K4.3, §22.12, stdlib §6.8, §6.9 |
| **C14** | **Async disposal.** `await using`, a direct `DisposeAsync()` call, per-level supersession of the destructor body, the synchronous fallback (and its warning), `await using` on early iterator abandonment, the run-time object's `DisposeAsync`. | §28.12, §28.6, K3.9, §29.7, §30.7, stdlib §2.10 |

Every row×column pair is either a real question or an n/a that has a cited reason. Expected n/a clusters: R9×C2/C7/C8 (statics are never scope state, K9/K9.4; check before marking), R10 across most columns (*exempt* rather than n/a where a rule speaks), R3×C14 (implicit discharge is synchronous: decided, not n/a).

## 4. Cell contract

One JSON object per cell. The fields are the borrow-flow matrix's plus `lane`:

```json
{"row":"R8","col":"C11",
 "verdict":"rejected|once|once-backstopped|exempt|conditional|undetermined|n/a",
 "rule":"LABEL: \"verbatim quote ≤ 60 words\" | LABEL: \"…\"",
 "cite":"§x.y[, K…][, stdlib §a.b][, #issue]",
 "status":"decided|missing|conflict|ambiguous|n/a",
 "note":"one to three sentences: who discharges, when, and what backstops a later use — or why the cell is open",
 "counterexample":"for missing/conflict/ambiguous: a minimal Bezel program (≤ 25 lines); else empty",
 "lane":"L1".."L5",
 "rev":"v0.333 @ f6e9003",
 "rc":["issue numbers this cell is filed under, optional"]}
```

**Verdicts** (the text's answer, independent of status):
- **rejected**: a program that puts this holder on this path is a compile error.
- **once**: admitted, with exactly one discharge at a stated moment, and no later use of the object is admitted.
- **once-backstopped**: admitted, with exactly one discharge, and a later use through a surviving alias is admitted only under a named K4.1 flag, K4.2 kill or K4.3 clause, which the `rule` field must quote.
- **exempt**: admitted, and the obligation is waived by a recorded rule that the cell quotes (runtime-managed, trivial-dispose ceremony, the §22.13 non-`owned` static exemption, form (iv)'s displaced slots).
- **conditional**: sub-shapes take different verdicts from the list above. The note enumerates them.
- **undetermined**: the text yields no verdict (only with missing or ambiguous).
- **n/a**: the holder cannot reach the path by construction.

**Statuses**, adapted to the disposal question:
- **decided**: one rule, or a consistent set, settles both (a) the discharger, exactly-once and the moment, and (b) the posture toward later uses. The auditor could not write a leak, a double run of a destructor, or an un-backstopped use-after-dispose.
- **missing**: no text decides who discharges on this path or when, or a use after disposal is admitted with no named backstop (K4.3: "no new admission may be made without recording its clause or its backstop").
- **conflict**: two passages give different dischargers, moments or verdicts. Quote both.
- **ambiguous**: one passage, two defensible readings with different verdicts. Give both readings in the note and the counterexample.
- **n/a**: as the verdict, with the reason cited.

**Route cells.** These are for a specific escape inside a cell, e.g. `Shared<T>` last-drop, trivial-dispose free, `set(owned)` accessor, A15 self-returning enumerator, or the §30.11 completion-source carve. Each route cell carries the base fields plus `"route"` (one-line description) and `"issue"`.
