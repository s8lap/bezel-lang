# Proposal 0218 — An owning `WhenAll` for every task family, and what an owning aggregate does with its results when an input fails

| | |
|---|---|
| **Status** | under-review — draft (2026-11-26); no critique pass has run. Owner questions in §10. |
| **Tracking issue** | [#7429](https://github.com/s8lap/bezel-lang/issues/7429) (the missing owning forms) and [#7440](https://github.com/s8lap/bezel-lang/issues/7440) (the call-bounded fan-out with no spelling), answered together |
| **Target documents** | **spec** — §30.10 (the owning overload's bullet, one new bullet, two sentences of *Aggregation contracts*, the closing parenthetical of *Structured fan-out*), §28.14 rule 2 (one sentence replaced by three), §15.4 (one parenthetical). **stdlib** — §6.7 (the declaration block, *Raises*, the subsection *`WhenAll` for class T* renamed and its sketch replaced, the *Building the borrowed input* bullet), §6.9 (the `Pool` example's join loop). **rationale** — §12.12 (one paragraph). **Guide 04** — §2 (one sentence), §4 (one sentence and one example). **code-generation reference** — §5.6 (two index lines). **proposals** — `README.md` (this row); `DECLINED.md` rows at acceptance (§5). No grammar change, no new keyword, no new type. |
| **Written against** | spec, stdlib and rationale **v0.360** at `origin/main` `bd12fe038`, and the **v0.361** text on `spec/v0361-2026-11` at `c85ce271c`. v0.361 touches one section this proposal edits (stdlib §6.7 gains the `Delay` paragraph, which no edit here overlaps) and rules #8640 in spec §31.2 (*The chain is per raise* and the paragraph after it), which §2.4 and §3.1 read. Every anchor quoted in §6 occurs exactly once in the v0.361 text. Section numbers are v0.361's. |
| **Origin** | The v0.346 lane G critique (its questions 4 and 5), filed as #7440 and #7429; the v0.361 triage (`DESIGN`, design change (API)). |
| **Change class** | Design change (`spec-change-process.md` §2): two new standard-library overloads. It also rules a contradiction inside §30.10 (§1.3), which alone would be ruling-class; it rides here because the new overloads need the answer. It reopens no recorded decline (§5.2). |

*Conventions. An unprefixed § is a language-spec section at v0.361; stdlib, rationale, guide and reference sections are marked. ▲ marks a proposed member: the two `WhenAll` overloads of §4 are the only ones, and nothing else here is new API. Every statement of fact carries a label: **MEASURED** (a probe compiled and run with `bezelc 0.0.0-8ccf7ff0`, the r42 prerelease, in checked disposal mode; programs and output in §12), **QUOTED** (repository text at the cited place), **HYPOTHESIS** (a judgement nothing here measures).*

## 0. Summary

Dynamic fan-in over freshly-minted tasks has exactly one spelling, by the #1813 ruling (stdlib §6.7): hold the tasks in an owning array and hand it to an owning combinator. That spelling exists for `OwningTask<T>` only. For a loop that starts `Task`s or value-result `Task<int>`s there is no owning `WhenAll`, so programs await the elements one by one, which does not settle the rest on a fault (#7429). For a call-bounded callback the gap is a soundness obligation with no way to meet it: §28.14 rule 2 requires every task the callback mints to be awaited to settlement, and the loop form `pending.Add(cb())` became an error at v0.346 (#7440).

The owning overload that does exist is specified two ways. §30.10 says every `WhenAll` "completes only after every input has settled — it never abandons laggards", and it also says the owning overload "behaves as its stdlib §6.7 sketch shows". The sketch stops at the first failure and disposes the remaining tasks unsettled. The runtime does the first; the sketch does the second (MEASURED, §2.3).

The proposal:

1. **The owning aggregate settles every input first** (option R1, §3.1). When all inputs have settled, a non-successful aggregate disposes every result produced and every fault but the first non-successful input's, keeps that input, and re-raises its fault. That re-raise continues the input's own raise under v0.361's #8640 ruling, so the chain the input's body recorded arrives whole. The stdlib sketch is withdrawn. This is what the runtime already does.
2. **Two owning overloads are added** (option B, §3.2): ▲`WhenAll(owned tasks: OwningArray<Task>): Task` and ▲`WhenAll<T>(owned tasks: OwningArray<Task<T>>): Task<Array<T>>` for non-class `T`, under the same contract. The call site is `await Task.WhenAll(move pending)`, which is also the spelling #7440 asks for. The compiler's acceptance of `Task.WhenAll(owning)` without `move` (#7429's separable divergence) becomes the ordinary call-site marker error.
3. **Declined** (§5): an owning `WhenAny`; an owning `WhenEach` for `Task` and value tasks; widening the borrowing parameters to `IReadOnlyList<…>`; the owning value-result overload at a class `T`; handing back partial results; recording the dropped faults.

## 1. The questions and the text today

### 1.1 #7440: rule 2's fan-out has a static count only

§28.14 rule 2 (QUOTED): a callee that invokes a call-bounded callback whose invocation mints a task "must consume the frame to its K6.1 bound before returning — a task awaited to settlement", and "the returned tasks may be accumulated in a purely local collection provided every one of them is awaited to settlement before the callee returns on every path — `await Task.WhenAll(pending)` discharges all of its inputs, since the aggregate completes only after every input settles. The collection holds the tasks through named bindings (`var t1 := cb(); var t2 := cb(); var pending: Array<Task> := [t1, t2];`): a minted task stored straight into a borrowing slot, `pending.Add(cb())`, dies at its K8 boundary and is §15.4's fresh-store error, not an accumulation."

Named bindings fix the count in the source text. The issue lists the loop spellings and why each fails; §2.1 traces them.

### 1.2 #7429: the owning family covers class results only

Stdlib §6.7 declares (QUOTED) `WhenAll(tasks: Array<Task>): Task`, `WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>` "for non-class T", and `WhenAll<T>(owned tasks: OwningArray<OwningTask<T>>): OwningTask<OwningArray<T>> where T: class`. Its *Building the borrowed input* bullet records the #1813 ruling: "use the owning overloads (`WhenEach(move tasks)`, `WhenAll(move tasks)`) for dynamically-sized fan-in — these two spellings are the complete input story (#1813, ruled 2026-07-24): dynamic fan-in over freshly-minted tasks is an ownership-transfer scenario by construction, so the owning forms are its spelling, and no borrowing-family dynamic-build facility is planned."

Guide 04 §4 tells the reader the same (QUOTED): "For a dynamically-sized set, use the owning overloads". For `Task` and `Task<int>` there is no owning overload to use. The 2026-11 ergonomics study hit it (QUOTED, `docs/research/ergonomics-study/data/frictions.tsv` row F074, task t30-workers): "v3: OwningArray<Task<long>> and await each in a for loop (WhenAll not usable over an owning array of Task<long>)". Stdlib §6.9's own `Pool` example joins its `OwningArray<Task>` the same way, one element at a time.

### 1.3 The owning overload's fault path is stated twice, differently

§30.10 *Aggregation contracts* (QUOTED): "`Task.WhenAll` (all input forms, the owning overload included) completes only after every input has settled — it never abandons laggards." Further on, in the same paragraph: "The owning `WhenAll` overload behaves as its stdlib §6.7 sketch shows, now as **contract**: inputs are awaited in input order; on the first non-successful input, results already produced are disposed with their tasks and laggards' eventual results are disposed on production (§30.5 disposal continuations), and awaiting re-raises that first fault".

The sketch (stdlib §6.7, QUOTED):

```bezel
using var taskArray := move tasks;
var results := new OwningArray<T>(capacity := taskArray.Count);
for var task in taskArray.Drain() do
  results.Add(await task);
end;
return results;
```

When `await task` faults at index *i*, the loop exits. `taskArray`'s scope ends and disposes the tasks after *i* whether or not they have settled. A task disposed unsettled is abandoned (§30.5: the dispose "does not block waiting for completion"; a continuation disposes the result when it is produced). So the aggregate completes while later inputs still run. That is "laggards' eventual results are disposed on production", and it contradicts "completes only after every input has settled". Rule 2's discharge sentence ("since the aggregate completes only after every input settles") depends on the first reading.

The bullet earlier in §30.10 has the sketch's reading too (QUOTED): "Each task is awaited in turn (sequentially in the implementation, but the underlying operations run concurrently because the tasks were started before being passed in)."

### 1.4 What v0.361 adds: the first awaiter's re-raise continues the input's raise

v0.361 rules #8640 in §31.2 (QUOTED, `spec/v0361-2026-11`): "a task-owned fault escaping the async body that raised it, and its re-raise by the task's first awaiter — an `await`, a `Result` read, or a combinator re-raising it — are one raise, and that re-raise clears nothing: the async body's own raise *is* the raise being reported, and its chain is delivered whole". And: "A re-raise of an instance that is propagating nowhere and bound by no arm, after an earlier raise of it has ended — the second awaiter of a multi-awaited task after the first handled the fault … — begins a new raise and clears first."

The triage note on #8640 (`triage/rulings.md`) records an implementation approximation: the flag is kept per task, so "the WhenAll aggregate approximation (input awaited, then the aggregate, no clear between) diverges from per-instance". With a borrowing `WhenAll`, the caller still holds the inputs and can await one before awaiting the aggregate. With an owning form the caller cannot (§3.1).

## 2. The repros, traced

### 2.1 #7440: a loop over a call-bounded callback

The shape: `public static async method FanOut(cb: (int) -> Task, xs: Array<int>): Task`, one task per element, all settled before return.

| Spelling | Verdict at v0.361 | Why |
|---|---|---|
| `var pending: Array<Task> := []; for … pending.Add(cb(x)); … await Task.WhenAll(pending);` | rejected (§15.4) | A fresh task into a borrowing slot dies at its K8 boundary (QUOTED, rule 2). Pinned: `call_bounded_task_abandonment_1527.async_loop_fanout_add_is_fresh_store_error`. |
| `await using var scope := new TaskScope(); … scope.Spawn(cb …)` | rejected (§28.14) | A call-bounded parameter may not be forwarded into a `scoped` position (QUOTED, the call-bounded default). Wrapping it, `scope.Spawn(async lambda() => do await cb(x); end)`, is rejected too: "closure bound to a `scoped` parameter captures `cb`, a call-bounded function-typed parameter" (MEASURED, p10). |
| `scoped cb` on the static method | rejected (§28.14) | `scoped` on a static method needs an `owned` return (QUOTED). |
| `var pending := new OwningArray<Task>(); … pending.Add(cb(x)); … for var t in pending.Drain() do await t; end;` | not a discharge on the fault path | On the first fault the loop exits and `pending`'s disposal, or the drain enumerator's, disposes the unsettled rest: the escape rule 2 forbids, and the compiler's check rejects it ("not awaited to settlement"). |
| the same array, `await Task.WhenAll(pending);` (no `move`) | rejected by the text, accepted by the compiler | §28.11: "an `OwningArray<Connection>` is not an `Array<Connection>`", and no `WhenAll` takes an `OwningArray<Task>`. r42 accepts it and credits rule 2 (MEASURED, p05: prints `10`, `20`, `30`, `done`). This is #7429's divergence. |
| the same array, `using var view := pending.ToArray(); await Task.WhenAll(view);` | admitted by the text, rejected by the compiler | Stdlib §4.5: `ToArray()` on an owning receiver returns "a **borrowing snapshot**", the array continuing to own the elements. The borrowing aggregate settles every element (QUOTED, §30.10), so rule 2's "every one of them is awaited to settlement" holds. r42 runs the non-callback form (MEASURED, p01, p02) but rejects the callback form: its rule-2 check recognizes only `Task.WhenAll(<the collection local>)` (MEASURED, p03; `src/ownership/Ownership_Checks.cpp`, `whenAllCollection`). |

So the language today has one spelling that is sound and admitted by the text: the snapshot. Neither issue named it, the compiler does not credit it, and no document teaches it.

### 2.2 #7429: value-result fan-in

`OwningArray<Task<int>>` filled in a loop has three spellings at v0.361. Awaiting element by element sums the results but abandons the rest on a fault (§2.1, row 4). `Task.WhenAll(owning)` is the divergence (MEASURED, p04: compiled and printed `10`, `2`). The snapshot works with the borrowing `WhenAll<int>` (MEASURED, p02: `60`, `20`, `40`, in input order). The aggregate's result is a `Task<Array<int>>`, Pattern A at a class type, so it is bound before the await (`using var all := Task.WhenAll(view); var rs := await all;`), or written as the scoped await-bind, which r42 keeps behind `--experimental-scoped-await-bind`.

### 2.3 The owning overload's fault path under the two readings

Three Pattern B inputs: `Make(1)` succeeds at 5 ms, `Fail()` faults at 10 ms, `Make(3)` succeeds at 200 ms. Each `Widget` prints from its destructor. The caller catches and prints, then waits 300 ms.

- **The runtime** (`Task.WhenAllOwned`, the retired name r42 still uses; MEASURED, p06c): `produced 1`, `faulting`, `produced 3`, `dispose 1`, `dispose 3`, `caught boom`, `after`, `end`. The aggregate waits for `Make(3)`, disposes both results, then re-raises. `src/runtime/bezel_runtime_concurrency.cpp` (`bz_task_when_all_owned`) does exactly this: it registers a continuation on every input, and the last one to complete scans in input order.
- **The sketch, compiled as written** (MEASURED, p07): `produced 1`, `faulting`, `caught boom`, `after`, `produced 3`, `dispose 3`, `end`. The catch runs before `Make(3)` has finished. Its result is disposed later by the abandoned task's continuation. `dispose 1` never prints: `Widget 1` leaks. p09 narrows the leak to a separable compiler bug: an `async` body of `OwningTask<OwningArray<Widget>>` whose owned local is returned on the normal path does not dispose that local on the fault edge (MEASURED, three runs). It is not the sketch's fault, but it means the sketch's own disposal clause is not observable on r42 either.

r42 does not implement the merged name `Task.WhenAll(move tasks)` for the owning overload: the borrowing typing takes the call, and corpus cases `s30_10_0003` and `l6_7_0002` are banked `fail divergence` (QUOTED, `tests/conformance/rules/baseline.tsv`; MEASURED, `l6_7_0002` rejected with a Pattern A diagnostic). That gap comes before anything here can be implemented (§9).

### 2.4 Why this is design-level

Choosing R1 over R2 picks one of two sentences §30.10 states as contract. That alone is a ruling. Adding overloads is new API, which the triage classed as a design change, and the #1813 ruling's own sentence ("the owning forms are its spelling") is false for two of the three task families until something is added or declined. The fault semantics and the API question are one decision: a new owning overload must say what it does with its inputs on a fault, and rule 2 needs the answer to be "settles all".

## 3. Options

### 3.1 What an owning aggregate does when an input fails

**R1: settle all, then decide.** The aggregate waits for every input. Then:

- on all-success it completes with the results, in input order;
- otherwise it disposes every produced result and every fault instance but the first non-successful input's (input order), keeps that input until the aggregate is disposed, and re-raises its fault. A cancelled first input raises a fresh `OperationCanceledException`, as §30.10 already says for every form.

Traced through §2.3: `Make(3)` is waited for, both widgets are disposed before the catch runs, and nothing runs after the aggregate settles. Soundness: no input is abandoned, so rule 2's discharge sentence is true of the owning forms and the #7440 spelling is sound on every exit of the `await`. Every result has one owner at every point: the input task until settlement, then the aggregate (dispose or transfer). The kept input owns the re-raised instance, so §30.9's single-owner rule holds without the borrowing forms' residual: the caller holds no reference that could dispose the input early. Latency: the fault surfaces when the slowest input settles. This is the borrowing form's behavior already, and `WhenAll` never cancels, so the slowest input is waited for either way. Choosing input order then costs nothing and keeps the answer deterministic. TaskScope's "first in real time" (stdlib §6.10) exists because a scope cancels its siblings at the first fault; `WhenAll` cancels nothing (rationale §12.12: "`WhenAll` *collects* … `TaskScope` *supervises*"). Implementation cost: none for the existing overload (MEASURED, §2.3). #8640: the input's body raised the fault, the aggregate's continuation is the input's first and only awaiter, and the aggregate's first awaiter continues that raise. So `SupersededChain` arrives whole (§8.2 case `s30_10_0022`). No caller can await the input before the aggregate, because the input was moved. The per-task approximation the #8640 triage recorded is exact for the owning forms. For a `Task`-typed aggregate (multi-awaitable, §30.4) a second awaiter after the first handled the fault begins a new raise and clears, per the ruling. Migration: no test pins the early stop. The sketch is documentation, and the runtime is already R1.

**R2: stop at the first non-success in input order** (the sketch). Traced through §2.3: the catch runs before `Make(3)` settles, and `Make(3)`'s result is disposed by an abandoned continuation. Soundness: the laggards are disposed unsettled, which §30.10's *Structured fan-out* paragraph calls the unsupervised drop, "appropriate only for genuinely unsupervised work whose inputs the task owns". Here a library call makes that choice for the caller. Under R2 the owning forms do not discharge rule 2, so #7440 would stay open: a callback's caller-sourced captures would outlive the call through still-running frames. Behavior would differ between borrowing and owning forms over the same tasks. Cost: the runtime would have to change. Declined.

**R3: hand back what succeeded.** Return the produced results alongside the fault, as an aggregate exception carrying an owned array or a settled-tasks result. This is the aggregation rationale §13.5 declines ("first error wins, not aggregate"; `DECLINED.md`'s *Aggregating handler exceptions* row), and stdlib §6.10 applies the same posture to tasks. The per-task form already exists: `Task.WhenEach(move tasks)` yields each settled `OwningTask<T>` for the caller to await, keeping successes. Declined.

### 3.2 The spelling for `Task` and value-result `Task<T>`

**A: decline the overloads and teach the snapshot.** Hold the tasks in an `OwningArray`, take `ToArray()`, and pass the snapshot to the borrowing combinator. Traced: §2.1 row 6 and §2.2. It runs on r42 for `WhenAll` (p01, p02) and for `WhenEach` (p11). Soundness: the borrowing aggregate's residual stays (§30.10: "the input tasks (and the array) must outlive the aggregate task's settlement and every await or `Result` read of the aggregate"). Here `pending` and `view` are locals that outlive the `await`, so the residual is discharged in the common shape. It is still a residual, and `return Task.WhenAll(view);` would dangle (checked mode backstops it). The #8640 approximation stays reachable: `await pending[0]` before awaiting the aggregate. Ergonomics: two extra concepts at every dynamic fan-in, the snapshot and its lifetime obligation. The guides' and #1813's sentence ("use the owning overloads") stays false for two families. Cost: the compiler's rule-2 check must learn that a snapshot of a purely local owning collection discharges that collection, a provenance fact it does not compute today (`whenAllCollection` matches one identifier). Migration: none. A stays admitted under B as the spelling for `WhenEach` and `WhenAny` over value tasks (§5.1).

**B: add the two owning overloads.** ▲`WhenAll(owned tasks: OwningArray<Task>): Task` and ▲`WhenAll<T>(owned tasks: OwningArray<Task<T>>): Task<Array<T>>`, non-class `T`, under R1. Traced, #7440:

```bezel
public static async method FanOut(cb: (int) -> Task, xs: Array<int>): Task
  var pending := new OwningArray<Task>();     // purely local, owns every task it holds
  for var x in xs do
    pending.Add(cb(x));                       // a fresh task moves into an owning slot (§28.11)
  end;
  await Task.WhenAll(move pending);           // the aggregate settles every input (§30.10)
end FanOut;
```

Every path out of the `await`, normal or fault, comes after every minted task has settled, so rule 2 is met. `pending` is moved-from afterwards, so no task outlives the call. A borrow of an element taken before the `move` and used after it is the consume-site freeze's rejection (§28.3.9), as for any consume of a collection. `pending` cannot be a `using var` (a `using var` is never moved out, §28.10); the diagnostic for that case already names the rule.

Traced, #7429: `var all := Task.WhenAll(move pending);`, then `var rs := await all;` with `all` bound by `using var`, or the scoped await-bind. The result is `Array<int>` in input order.

Soundness: the aggregate owns every input from the call, so the borrowing forms' outlive-the-aggregate residual does not arise. R1 makes the discharge static. At a class `T` the value-result overload would hand out an `Array<T>` of borrows into results whose tasks the aggregate disposes at settlement, a certain dangle. So it is rejected there, with the Pattern B overload named as the remedy (§4, Q5). Elements of an `OwningArray<Task>` may be any `Task` subtype: a `Task<C>` or `OwningTask<C>` element's result is disposed with its task when the aggregate disposes the inputs (§30.5's cascade).

Overload resolution stays unique, because class generics are invariant (§12.9 admits variance on interface parameters only). An `OwningArray<Task<int>>` matches only `OwningArray<Task<T>>` at `T = int`, an `OwningArray<Task>` only the no-result form, and an `OwningArray<OwningTask<C>>` only the Pattern B form (`OwningTask<T>` derives from `Task`, not from `Task<T>`). §22.6's rule that "`owned` never distinguishes overloads" is not engaged: each pair differs in parameter type, the case §22.6 names.

Ergonomics: one line, the same as the class-result form, and the sentence the guides already print becomes true. Cost: §9 (sema dispatch on the container head; one new runtime entry modelled on `bz_task_when_all_owned`; the rule-2 recognizer extended to `move`). Migration: §7 and §8.1. One gtest that passes an `OwningArray<Task>` unmarked flips to reject, and the divergence it relies on is the one #7429 reports.

**C: widen the borrowing parameters to `IReadOnlyList<Task>` / `IReadOnlyList<Task<T>>`.** §28.11's prescription for a read-only consumer of either family ("declares one of the read-only iteration interfaces … and accepts either source"). It would legitimize what r42 does (p04, p05) with no new name. Traced: `await Task.WhenAll(pending)` over an `OwningArray<Task>` borrows it. Rule 2 holds when `pending` outlives the await, as in A. Soundness: the borrowing residual stays, as in A. Ergonomics: no `move`, but a collection literal has no standalone type and targets only a class that conforms to a literal-target interface (§15.4), so `Task.WhenAll([t1, t2])` would lose its target. That shape occurs 13 times in `tests/` and `docs/` outside the research notes (MEASURED, `git grep -E 'WhenAll(<[^>]*>)?\(\['`), and the Guide 04 and reference examples build a named literal first only by habit. Keeping both the `Array` and the interface overloads would make every `Array<Task>` argument match two candidates. Cost: low in the compiler, which already ignores the container head. Declined: it keeps the residual B removes and changes landed signatures to do it.

**D: decline outright (#7440's option (a)).** Await sequentially, or take the callback `scoped` on an instance member. Sequential awaiting is not concurrent fan-out. A `scoped` instance member moves the bound to an object the caller must create as a `using var` (§28.14's bounding forms), which changes the callee's shape to serve a combinator. Neither answers #7429, and the guides' sentence stays false. Declined.

## 4. Recommendation

R1 and B. R1 is the reading that rule 2, §30.10's first sentence, rationale §12.12 ("it never cancels or abandons the laggards") and the runtime already share. B makes #1813's sentence true for every task family, with the smallest surface. The call-bounded fan-out then has a spelling, and the static discharge needs no new analysis. A stays admitted and is the spelling for the two combinators B leaves borrowing-only.

The overload set after the change:

```bezel
public static method WhenAll(tasks: Array<Task>): Task;
public static method WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>;            // non-class T
public static method WhenAll<T>(
  owned tasks: OwningArray<OwningTask<T>>
): OwningTask<OwningArray<T>> where T: class;
public static method WhenAll(owned tasks: OwningArray<Task>): Task;                 // ▲ proposed
public static method WhenAll<T>(owned tasks: OwningArray<Task<T>>): Task<Array<T>>; // ▲ proposed; non-class T
```

## 5. What is declined

### 5.1 Rows for `DECLINED.md` (`declined-at-proposal`, added at acceptance)

Per the index's scope rule (status gates the record), these rows are not added while the proposal is a draft.

| Topic | Class | Authoritative record | Reasons and reopening bar |
|---|---|---|---|
| Stopping an owning `Task.WhenAll` at its first non-successful input and disposing the remaining inputs unsettled (the v0.361 stdlib §6.7 sketch's reading) | `declined-at-proposal` | proposal 0218 §3.1 (R2); spec §30.10 *Aggregation contracts* | The aggregate would abandon running tasks inside a library call, which §30.10 reserves for work whose inputs the task owns and nobody supervises. A call-bounded callback's fan-out would not be discharged (§28.14 rule 2), and the owning and borrowing forms would answer differently for the same tasks. `WhenAll` cancels nothing, so the early answer saves no work. Reopening bar: a cancelling owning aggregate, which is `TaskScope`'s design and would be a new member, not this one. |
| Handing back an owning `WhenAll`'s successful results when an input fails (a partial-result aggregate or a settled-tasks result) | `declined-at-proposal` | proposal 0218 §3.1 (R3); rationale §13.5 | First fault wins, not aggregate (rationale §13.5, the *Aggregating handler exceptions* row). `Task.WhenEach(move tasks)` already yields each settled task with its result or fault. Reopening bar: a program shape that `WhenEach` cannot express. |
| An owning `Task.WhenAny` (over `OwningArray<Task>` or any other owning input) | `declined-at-proposal` | proposal 0218 §3.2, §5.1; rationale §12.12 | `WhenAny` is a selector: it yields an index, and "the losers keep running and remain the caller's to settle". Consuming the array would give the caller an index into tasks it no longer holds, and the losers to an aggregate that settles nothing. A dynamic set is passed as the owning array's borrowing snapshot (stdlib §4.5). Reopening bar: none stated. |
| An owning `Task.WhenEach` over `OwningArray<Task>` or `OwningArray<Task<T>>` | `declined-at-proposal` | proposal 0218 §3.2, §5.1 | The borrowing `WhenEach` over the owning array's snapshot covers the shape, the array outliving the enumeration (MEASURED in the proposal, p11). `WhenEach` settles nothing, so an owning form would not discharge §28.14 rule 2 either. Reopening bar: evidence that the snapshot's lifetime obligation is a measured friction. |
| Widening the borrowing `WhenAll` parameters to `IReadOnlyList<Task>` / `IReadOnlyList<Task<T>>` | `declined-at-proposal` | proposal 0218 §3.2 (C); spec §28.11, §15.4 | It keeps the borrowing aggregators' outlive-the-aggregate residual, which the owning overloads remove. It costs collection-literal arguments their target type (§15.4), and it changes landed signatures. Reopening bar: none stated. |
| The owning value-result `WhenAll<T>` at a class or interface `T` (Pattern A inputs) | `declined-at-proposal` | proposal 0218 §3.2, §4 | The aggregate disposes its inputs when it settles, and a Pattern A result is disposed with its task, so the returned `Array<T>` would hold dangling borrows. Admitting it needs a result slot that retains N source tasks, a new kind beside §30.5's three. Class results that move are `OwningTask<T>`'s. Reopening bar: a proposal for an N-source chained slot. |
| Recording the non-first faults of a failed owning aggregate (appended to the re-raised exception's `SupersededChain`, or reported through the diagnostic channel) | `declined-at-proposal` | proposal 0218 §3.1, Q3 | `SupersededChain` records supersessions of the raise being reported (§31.2), and the other inputs' faults superseded nothing. Reporting them would make the owning form noisier than the borrowing form for the same outcome, which drops nothing only because the caller still holds those tasks. Reopening bar: field evidence of a lost fault that mattered. *(Subject to Q3.)* |

### 5.2 Records this proposal does not reopen

- **#1813** (changelog, the *WhenEach/WhenAll borrowing-input seam resolved-and-closed* entry): declined "a borrowing-family dynamic-build facility", "a borrowed-build facility with compiler-extended temporary lifetime", and "owning-semantics overloads on borrowing types". B adds owning overloads on an owning type, which is the ruling's own spelling. The entry even names "`OwningArray<Task<T>>`" among the owning family.
- **Proposal 0207** (the fresh-store error): unchanged. B gives its rejected loop a one-line rewrite.
- **Proposal 0110** stage 2 (▲`TaskSet<T>`, draft): unaffected. A dynamic set whose membership changes during the wait is its question. The owning `WhenAll` is the fixed-at-the-call case, as `WhenEach` is (0110 §3.4).
- **`DECLINED.md` row 183** (*Reading `TaskScope`'s "first fault" as spawn order*) and **row 757** (re-raising a defect ahead of a failure at `WhenAll`): unchanged.

## 6. The normative text

Every *old* string below occurs exactly once in the named v0.361 document (MEASURED, §12's anchor check). The ▲ markers are not part of the landed text.

### 6.1 Spec §30.10, the helper list

Old (the end of the owning overload's bullet):

> The caller transfers ownership of the array (and its tasks); the result is an owned array of owned elements. Each task is awaited in turn (sequentially in the implementation, but the underlying operations run concurrently because the tasks were started before being passed in). See std lib [§6.7](Bezel_Standard_Library.md#sec-task-static-helpers) for a worked example.

New:

> The caller transfers ownership of the array (and its tasks); the result is an owned array of owned elements. The underlying operations run concurrently, each having started when its task was created; the aggregate observes them all and settles after the last (*Aggregation contracts* below). See std lib [§6.7](Bezel_Standard_Library.md#sec-task-static-helpers) for a worked example.
> - `Task.WhenAll(owned tasks: OwningArray<Task>): Task` and `Task.WhenAll<T>(owned tasks: OwningArray<Task<T>>): Task<Array<T>>` (non-class T) — the owning overloads for tasks with no result and for value results, spelled `await Task.WhenAll(move tasks)`: the same aggregation over tasks the aggregate owns from the call. With the Pattern B overload above they make dynamic fan-in over freshly-minted tasks expressible for every task family. At a class or interface T the second form is a compile error naming the Pattern B overload: the aggregate disposes its inputs when it settles, and a Pattern A result is disposed with its task ([§30.5](#sec-result-lifetime-and-task-disposal)).

### 6.2 Spec §30.10, *Aggregation contracts*

Old:

> the other inputs' faults remain observable on those tasks individually.

New:

> for the borrowing forms, the other inputs' faults remain observable on those tasks individually.

Old:

> The owning `WhenAll` overload behaves as its stdlib §6.7 sketch shows, now as **contract**: inputs are awaited in input order; on the first non-successful input, results already produced are disposed with their tasks and laggards' eventual results are disposed on production ([§30.5](#sec-result-lifetime-and-task-disposal) disposal continuations), and awaiting re-raises that first fault — the first non-successful input task itself is retained by the aggregate and disposed with it, so the re-raised instance remains owned by its own task, transitively, for the aggregate's lifetime; on all-success the returned array owns every result; the owning overload over an empty array returns an already-completed task owning an empty array.

New:

> The **owning overloads** (an `OwningArray` input, passed with `move`) own their inputs from the call, and the first sentence holds for them unchanged: the aggregate settles only after every input has settled, and the first non-successful input is chosen in input order at that point, not at the moment it failed — a failure never stops the aggregate early, so no input is disposed unsettled. When every input succeeded, the spent inputs are disposed and the aggregate completes, the Pattern B overload with an owned array of the results and the value-result overload with the results in input order. Otherwise no result reaches the caller: every result an input produced is disposed with its input, and so is every input's fault instance except the first non-successful input's; that input task is retained by the aggregate and disposed with it, so the re-raised instance remains owned by its own task, transitively, for the aggregate's lifetime, and its re-raise by the aggregate's first awaiter continues the input's raise ([§31.2](#sec-try-catch-finally): a `SupersededChain` the input's body recorded is delivered whole). The other inputs' faults are not observable: the caller handed those tasks over. A caller that wants each input's outcome uses `Task.WhenEach(move tasks)` (stdlib [§6.7](Bezel_Standard_Library.md#sec-task-static-helpers)). An owning overload over an empty array returns an already-completed task — the Pattern B overload's owning an empty array.

### 6.3 Spec §30.10, *Structured fan-out*'s closing parenthetical

Old:

> hold each task in a named binding, or use `OwningArray<OwningTask<T>>` with the owning `Task.WhenAll` overload for dynamic fan-in, stdlib [§6.7](Bezel_Standard_Library.md#sec-task-static-helpers).)

New:

> hold each task in a named binding, or, for dynamic fan-in, hold the tasks in an `OwningArray` — `OwningArray<Task>`, `OwningArray<Task<T>>` or `OwningArray<OwningTask<T>>` — and pass it to the owning `Task.WhenAll` overload, stdlib [§6.7](Bezel_Standard_Library.md#sec-task-static-helpers).)

### 6.4 Spec §28.14, rule 2

Old:

> The collection holds the tasks through named bindings (`var t1 := cb(); var t2 := cb(); var pending: Array<Task> := [t1, t2];`): a minted task stored straight into a borrowing slot, `pending.Add(cb())`, dies at its [K8](#sec-kernel-k8) boundary and is [§15.4](#sec-collection-literals)'s fresh-store error, not an accumulation.

New:

> A borrowing collection holds the tasks through named bindings (`var t1 := cb(); var t2 := cb(); var pending: Array<Task> := [t1, t2];`): a minted task stored straight into a borrowing slot, `pending.Add(cb())`, dies at its [K8](#sec-kernel-k8) boundary and is [§15.4](#sec-collection-literals)'s fresh-store error, not an accumulation. A dynamically-sized fan-out holds the tasks in an owning collection and passes it to the owning overload, which discharges every input the same way: `var pending := new OwningArray<Task>(); for var x in xs do pending.Add(cb(x)); end; await Task.WhenAll(move pending);`. Awaiting an owning collection's tasks one at a time is not such a discharge: on the first fault the rest are disposed unsettled with the collection, which is the escape above.

### 6.5 Spec §15.4, the fresh-store remedies

Old:

> for dynamic task fan-in, `OwningArray<OwningTask<T>>` with the owning `Task.WhenAll` overload, stdlib [§6.7](Bezel_Standard_Library.md#sec-task-static-helpers))

New:

> for dynamic task fan-in, an `OwningArray` of the tasks with the owning `Task.WhenAll` overload, stdlib [§6.7](Bezel_Standard_Library.md#sec-task-static-helpers))

### 6.6 Stdlib §6.7, the declaration block

Old:

>   public static method WhenAny(tasks: Array<Task>): Task<int>;   // index of the first task to SETTLE — success, fault,

New (two declarations inserted before the `WhenAny` line, which is kept):

```bezel
  // For Task and for Task<T> at non-class T: the owning overloads (the parameter
  // type distinguishes them; the call site is Task.WhenAll(move tasks)). The
  // aggregate owns the array and its tasks from the call and disposes them once
  // every input has settled. At a class T the second form is a compile error:
  // use OwningTask<T> and the overload above.
  public static method WhenAll(owned tasks: OwningArray<Task>): Task;
  public static method WhenAll<T>(owned tasks: OwningArray<Task<T>>): Task<Array<T>>;

  public static method WhenAny(tasks: Array<Task>): Task<int>;   // index of the first task to SETTLE — success, fault,
```

### 6.7 Stdlib §6.7, *Raises*

Old:

> `WhenAll` raises nothing of its own: awaiting it re-raises the exception of the first non-successful input in input order, a cancelled input counting as `OperationCanceledException` (language spec §30.10)

New:

> `WhenAll` raises nothing of its own: awaiting it re-raises the exception of the first non-successful input in input order, once every input has settled, a cancelled input counting as `OperationCanceledException` (language spec §30.10)

### 6.8 Stdlib §6.7, the owning subsection

Old (the heading):

> #### `WhenAll` for class T: the owning overload

New:

> #### The owning `WhenAll` overloads

The anchor `sec-whenall-for-class-t-whenallowned` above the heading is kept.

Old (the first sentence after the heading):

> The generic `WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>` aggregates results from non-class T (value types, `string`, runtime-managed reference types). For class T held by `OwningTask<T>`, use the **owning overload**

New:

> Dynamic fan-in — a number of tasks fixed only at run time — holds the tasks in an `OwningArray` and hands it to an owning overload, whatever the task family: `OwningArray<Task>` for tasks with no result, `OwningArray<Task<T>>` for value results (`int`, `string`, runtime-managed T), and `OwningArray<OwningTask<T>>` for class results. The value-result form returns `Task<Array<T>>`, as the borrowing form does:
>
> ```bezel
> public async method TotalSize(paths: Array<string>): Task<long>
>   var pending := new OwningArray<Task<long>>();
>   for var p in paths do
>     pending.Add(SizeAsync(p));               // a fresh task moves into an owning slot
>   end;
>   using var all := Task.WhenAll(move pending);
>   var sizes := await all;                    // Array<long>, in input order
>   var total := 0L;
>   for var s in sizes do total += s; end;
>   return total;
> end TotalSize;
> ```
>
> The holder is a plain `var`: a `using var` is never moved out (language spec §28.10). For class T held by `OwningTask<T>`, use the **owning overload**

(`SizeAsync` is the program's own method returning `Task<long>`, as `LoadConfigAsync` is in the example that follows.)

Old (from the start of this paragraph through the end of the paragraph that closes it — four paragraphs and one code block):

> Implementation sketch (the *behavior* shown — await in input order, first-fault re-raise, disposal of already-produced and laggard results — is now contract, spec §30.10 "Aggregation contracts"; the code itself remains illustrative):

through

> The owning `WhenAll` body awaits sequentially, but the tasks themselves run in parallel because they were started earlier.

New:

> **What the aggregate does with the results** (contract: language spec [§30.10](Bezel_Language_Spec.md#sec-standard-library-task-helpers) *Aggregation contracts*; the same for all three owning overloads). The aggregate owns the array and every task in it from the call. The operations run concurrently because each started when its task was created, before the array was built. The aggregate waits until every task has settled, and then:
>
> - if every task succeeded, the results move into the returned array in input order, the spent tasks are disposed, and the aggregate completes;
> - otherwise no result reaches the caller: each produced result is disposed with its task, and so is each fault instance except that of the first non-successful task in input order. That task is kept by the aggregate until the aggregate is disposed, and awaiting the aggregate re-raises its fault (a cancelled first task: a fresh `OperationCanceledException`).
>
> A failure does not stop the aggregate early. A task still running when an earlier one fails is waited for, and its result is disposed when it arrives. Stopping at the failure would dispose running tasks unsettled — abandoning them (language spec [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)) inside the call — and would answer differently from the borrowing form over the same tasks; `WhenAll` cancels nothing, so it saves no work. A caller that wants each task's outcome, results kept, uses `Task.WhenEach(move tasks)` below and awaits each yielded task. A unit of work whose parts stand or fall together is a `TaskScope` (§6.10), which cancels the rest at the first fault.

The two paragraphs after the removed block (*For producer-consumer pipelines …* and the `OpenFiles` example) are unchanged.

### 6.9 Stdlib §6.7, *Building the borrowed input*

Old:

> or use the owning overloads (`WhenEach(move tasks)`, `WhenAll(move tasks)`) for dynamically-sized fan-in — these two spellings are the complete input story

New:

> or use the owning overloads for dynamically-sized fan-in — `WhenAll(move tasks)` over an `OwningArray` of tasks of any family, `WhenEach(move tasks)` over an `OwningArray<OwningTask<T>>`; for an `OwningArray<Task>` or `OwningArray<Task<T>>`, the borrowing `WhenEach` and `WhenAny` take the array's borrowing snapshot (`using var view := tasks.ToArray();`, §4.5), the array outliving the enumeration or the await — these spellings are the complete input story

### 6.10 Stdlib §6.9, the `Pool` example

Old:

>     using var joins   := new OwningArray<Task>();

New:

>     var joins         := new OwningArray<Task>();           // moved into the join below

Old:

>     for var t in joins.Drain() do
>       using var jt := move t;
>       await jt;                                            // join; faults surface as DomainFaultedException
>     end;

New:

>     await Task.WhenAll(move joins);                        // join all; the first fault (input order) surfaces
>                                                            //   as DomainFaultedException, after every worker settled

The trace under the example ("⇒ the joins are awaited (§6.8)") is unchanged. With the drain loop, a worker's fault disposed the remaining join tasks unsettled. That was harmless only because the senders had already been closed and every worker was draining to completion.

### 6.11 Rationale §12.12

Inserted after the paragraph ending "what was missing was the adjacency." (before the parenthetical on `WhenEach`):

> **Dynamic fan-in has an owning form for every task family, and an owning aggregate settles everything first** (proposal 0218). The #1813 ruling made the owning family the spelling of dynamic fan-in and shipped it for `OwningTask<T>` only; `OwningArray<Task>` and `OwningArray<Task<T>>` now have their `WhenAll` overloads, which is also how a call-bounded callback's loop fan-out discharges §28.14 rule 2. What an owning aggregate does when an input fails follows from the fault fork above: `WhenAll` collects and never cancels, so waiting for every input costs nothing the borrowing form does not already pay, and an aggregate that stopped at the failure would abandon running tasks inside a library call — the decision §17.13 insists be visible. With every input settled, input order is free, and it makes the answer deterministic, where `TaskScope`'s real-time "first" is forced by its cancellation. *Declined:* the early stop the earlier stdlib sketch showed; handing back partial results (§13.5's aggregation, and `WhenEach` already yields each outcome); an owning `WhenAny` (a selector's losers remain the caller's); widening the borrowing parameters to `IReadOnlyList<…>` (it keeps the borrowing residual the owning forms remove, and costs collection-literal arguments their target).

### 6.12 Guide 04

§2, old:

> or use the owning family (`OwningArray<OwningTask<T>>` with the owning `Task.WhenAll`/`WhenEach` overloads — the same names, the parameter type distinguishing them and the `move` marker spelling the transfer: `Task.WhenAll(move tasks)`, spec §22.3/stdlib §6.7) for dynamically-sized fan-in.

New:

> or use the owning family for dynamically-sized fan-in: an `OwningArray` of the tasks — `OwningArray<Task>`, `OwningArray<Task<int>>`, `OwningArray<OwningTask<T>>` — handed to the owning `Task.WhenAll` overload (the same name, the parameter type distinguishing it and the `move` marker spelling the transfer: `Task.WhenAll(move tasks)`, spec §22.3/stdlib §6.7).

§4, old:

> For a dynamically-sized set, use the owning overloads — the same names, the parameter type distinguishing them, the `move` marker spelling the consuming form (spec §22.3): `Task.WhenAll(move tasks)` collects everything, `Task.WhenEach(move tasks)` streams each task as it completes (stdlib §6.7).

New:

> For a dynamically-sized set, use the owning overloads — the same names, the parameter type distinguishing them, the `move` marker spelling the consuming form (spec §22.3). `Task.WhenAll(move tasks)` takes an owning array of any task family, waits for every task, and then re-raises the first failure in input order. `Task.WhenEach(move tasks)` streams each `OwningTask<T>` as it completes (stdlib §6.7).
>
> ```bezel
> public async method SaveAll(paths: Array<string>, report: string): Task
>   var pending := new OwningArray<Task>();             // owns every task it holds
>   for var p in paths do
>     pending.Add(File.WriteAllTextAsync(p, report));   // a fresh task moves into an owning slot
>   end;
>   await Task.WhenAll(move pending);                   // settles ALL, then re-raises the first failure
> end SaveAll;
> ```
>
> The holder is a plain `var`, because a `using var` cannot be moved out.

The guide's pin moves to the landing revision (process §5 item 5).

### 6.13 Code-generation reference §5.6

Old:

>   static method WhenAll<T>(owned tasks: OwningArray<OwningTask<T>>): OwningTask<OwningArray<T>> where T: class;

New:

```bezel
  static method WhenAll<T>(owned tasks: OwningArray<OwningTask<T>>): OwningTask<OwningArray<T>> where T: class;
  static method WhenAll(owned tasks: OwningArray<Task>): Task;                       // dynamic fan-in: `move` a plain-var holder
  static method WhenAll<T>(owned tasks: OwningArray<Task<T>>): Task<Array<T>>;       // non-class T; settles ALL, then the first fault
```

The reference's pin moves to the landing revision.

## 7. Changelog sketch and Migration

> **An owning `WhenAll` for every task family; an owning aggregate settles every input before it fails (proposal [0218](proposals/0218-owning-whenall-for-every-task-family.md); [#7429](https://github.com/s8lap/bezel-lang/issues/7429), [#7440](https://github.com/s8lap/bezel-lang/issues/7440)).**
>
> **The standard library.**
> - **§6.7 (design change: two overloads).** *Decided.* `Task.WhenAll(owned tasks: OwningArray<Task>): Task` and `Task.WhenAll<T>(owned tasks: OwningArray<Task<T>>): Task<Array<T>>` (non-class T; a class T is a compile error naming the Pattern B overload). Dynamic fan-in over freshly-minted tasks now has the owning spelling the #1813 ruling named for every task family. *Declined:* an owning `WhenAny`; an owning `WhenEach` for `Task` and value tasks (the borrowing forms over the owning array's snapshot cover them); widening the borrowing parameters to `IReadOnlyList<…>`; the value-result form at a class T. *Why.* The guides already told readers to use the owning overloads for dynamic sets, and for two of the three families there were none.
> - **§6.7, the owning subsection (ruling).** *Decided.* An owning aggregate settles every input first. Then it completes with the results, or disposes every produced result and every fault but the first non-successful input's (input order) and re-raises that one. The sketch that stopped at the first failure is withdrawn. *Declined:* the early stop; handing back partial results.
> - **§6.9's `Pool` example** joins with `await Task.WhenAll(move joins)`.
>
> **The language specification.** §30.10: the new bullet; *Aggregation contracts* states the owning forms' fault path, replacing the sentence that made the sketch contract (the two sentences contradicted each other). §28.14 rule 2: the dynamically-sized fan-out's spelling, and why element-by-element awaiting is no discharge. §15.4's remedy parenthetical.
>
> **The rationale** §12.12, **Guide 04** §2 and §4, and **the code-generation reference** §5.6.
>
> **Migration.** *Newly rejected:* `Task.WhenAll(x)` with `x` an `OwningArray<Task>` or `OwningArray<Task<T>>` and no `move`. No overload ever took an owning array without one (§28.11), and r42 accepted it as a divergence (#7429). Write `Task.WhenAll(move x)` and make `x` a plain `var`. *Newly admitted:* the same call with `move`. *Behaviour:* none for programs that compiled under the text; the runtime already settles every input of the owning overload. *Spec-ahead-of-impl:* the two overloads, the `move` requirement, the rule-2 credit for `Task.WhenAll(move pending)`, and (prerequisite, already tracked by the banked divergences `s30_10_0003` and `l6_7_0002`) the merged name for the Pattern B overload. One tracker is filed at landing, unless #7429 is kept open as it.

## 8. Conformance and tests

### 8.1 What flips (MEASURED by `git grep` at `bd12fe038`)

- **gtest, verdict flips to reject:** `task_combinators_6_7.when_all_owned_array_faulted_task_caught_855` (`tests/codegen/task_combinators_test.cpp`). It passes a `using var tasks := new OwningArray<Task>()` to `Task.WhenAll(tasks)`, and its comment calls this "the correct idiom". Rewritten to `var tasks := …; await Task.WhenAll(move tasks)`, the expectation (`55`) is unchanged.
- **gtest, rows to add** in `tests/ownership/call_bounded_task_abandonment_1527_test.cpp`: the loop fan-out into an `OwningArray<Task>` discharged by `await Task.WhenAll(move pending)` (accepted), and the same without `move` (rejected with the marker diagnostic). The two existing `…_add_is_fresh_store_error` rows are unchanged, and their comments gain the owning spelling.
- **Rule corpus:** no case changes verdict. `s30_10_0001`, `s30_10_0009`, `s30_10_0010` and `s30_10_0013` (borrowing forms) are re-read and unchanged. `s30_10_0003` and `l6_7_0002` keep their verdicts, and their rule quotes are refreshed if the quoted sentence moved. Swept: `manifest.tsv` for `WhenAll`, `WhenEach`, `laggard`, `first non-successful`, `purely local` and `fan-out` (8 cases: the six `s30_10`/`l6_7` cases above and two `purely local` store cases, `s28_3_0936` and `s28_11_6910`, which this proposal does not touch; none cites the sketch sentence).
- **Documents:** no fenced example other than those in §6 calls `WhenAll` over an owning array. Every fenced example that changes (§6.8's `TotalSize`, §6.10's `Pool`, §6.12's `SaveAll`) uses only declared members (`OwningArray<T>.Add`, `File.WriteAllTextAsync`, the new overloads) and program-defined methods. Each was traced on its normal and fault paths.

### 8.2 Proposed rule-corpus cases

| Case | Expect | Program | Rule sentence cited |
|---|---|---|---|
| `s30_10_0014` | run | three `Task` inputs (delays 30, 10, 20) in an `OwningArray<Task>`, `await Task.WhenAll(move pending)`, then `done`; output `10`, `20`, `30`, `done` | the new bullet (§6.1) |
| `s30_10_0015` | run | `OwningArray<Task<int>>` of `n * 2` at delays 30, 10, 20; results printed `60`, `20`, `40` (input order) | the new bullet; *Aggregation contracts* |
| `s30_10_0016` | run | Pattern B inputs `Fail()` at 10 ms and `Make(3)` at 200 ms; output `faulting`, `produced 3`, `dispose 3`, `caught boom`: the laggard is waited for and its result disposed before the catch | §6.2's replacement sentence |
| `s30_10_0017` | run | owning `Task` inputs: index 0 faults at 50 ms with `A`, index 1 at 1 ms with `B`; output `A` | *Aggregation contracts* (input order, the owning forms) |
| `s30_10_0018` | run | owning `[Task.FromCancelled(), Fail()]`; output `cancelled` | *Aggregation contracts* (a cancelled input) |
| `s30_10_0019` | reject | `Task.WhenAll(pending)` with `pending: OwningArray<Task<int>>`, no `move` (#7429's c03 shape); diagnostic needle `move` | §22.6's call-site discipline; the new bullet |
| `s30_10_0020` | reject | `using var pending := new OwningArray<Task>(); … Task.WhenAll(move pending)`; needle `using var` | §28.10 (a `using var` is never moved out) |
| `s30_10_0021` | reject | `Task.WhenAll(move pending)` with `pending: OwningArray<Task<Widget>>`; needle `OwningTask` | the new bullet's class-T sentence |
| `s30_10_0022` | run | the input's body is #8640's `try throw new ExA(…) finally throw new ExB(…) end try;`; the owning aggregate's awaiter prints `e.SupersededChain.Count` = `1` | §6.2's "continues the input's raise"; §31.2 (v0.361) |
| `s28_14_0033` | run | #7440's repro: `FanOut(cb: (int) -> Task, xs)` with `pending.Add(cb(x))` into an `OwningArray<Task>` and `await Task.WhenAll(move pending)`; output `10`, `20`, `30`, `done` | §6.4's new sentence |
| `s28_14_0034` | reject | the same, awaiting `for var t in pending.Drain() do await t; end;`; needle `settlement` | §6.4's last sentence |

Each is authored from the sentence it cites, not from a compiler's output. `s30_10_0016` deliberately has one successful input, so that the order of disposals among results, which the text leaves unspecified, is not pinned.

## 9. Implementation sketch (non-normative)

Read from `src/` at `bd12fe038`.

- **Prerequisite: the merged name.** `src/sema/ExprTypecheck_Calls.cpp` types `Task.WhenAll` by inspecting the argument's first generic argument only (`elemIsTask` accepts `Task` or `OwningTask`) and ignores the container head. The Pattern B aggregator is reachable only as `WhenAllOwned`. That is the banked divergence, and the c03 acceptance follows from the same code. The fix dispatches on the head (`Array` vs `OwningArray`) and the element (`Task`, `Task<T>`, `OwningTask<T>`). The `move` requirement then comes from the ordinary `owned`-parameter call-site check.
- **Runtime.** One entry, e.g. `bz_task_when_all_owning(arr, elemSize)`, modelled on `bz_task_when_all_owned` (`src/runtime/bezel_runtime_concurrency.cpp`, about 160 lines). It registers a continuation per input, and the last one scans in input order (as both existing forms do). On success it copies value results into a fresh `Array<T>` (as `bz_task_when_all_results` does). It disposes the spent inputs, and takes the first fault as `bz_task_when_all_owned` does (`bz_task_take_fault`/`bz_task_complete_taken_fault`, fix #7954). Estimated at 120–180 lines (HYPOTHESIS).
- **Codegen.** `src/codegen/LlvmBackend_Calls.cpp` (the `m == "WhenAll"` arm) routes the owning heads to the new entry and adopts the moved-in array.
- **Ownership.** The rule-2 recognizer (`whenAllCollection` in `checkCallBoundedTaskSettlement`, `src/ownership/Ownership_Checks.cpp`) matches `Task.WhenAll(<ident>)`. It gains `Task.WhenAll(move <ident>)` for an `OwningArray` local, about ten lines (HYPOTHESIS).
- **Compiler divergences found along the way**, separable and not fixed here: (1) the merged-name gap above (banked); (2) p09, an owned local of an `async` `OwningTask<…>` body that is returned on the normal path is not disposed on the fault edge (a leak, three of three runs); (3) p11, r42 passes an `Array<Task<int>>` to `WhenAny(tasks: Array<Task>)`, although class generics are invariant (§12.9). Items 2 and 3 are not filed. The coordinator files them, since this lane does not comment on GitHub.

## 10. Owner questions

**Q1 — The owning aggregate's fault path: settle all (R1) or stop at the first failure (R2)?** §30.10 states both. Recommendation: R1. Rule 2, rationale §12.12 and the runtime already assume it. R2 abandons running tasks inside a library call and leaves #7440 open.

**Q2 — The spelling for `Task` and value tasks: the two owning overloads (B) or the snapshot idiom alone (A)?** Recommendation: B. It removes the borrowing residual, makes the rule-2 discharge a syntactic fact, and makes the guides' existing sentence true. A stays admitted for `WhenEach` and `WhenAny`.

**Q3 — The non-first faults of a failed owning aggregate: dispose them silently, or report them through the implementation-defined diagnostic channel (as `FireAndForget` reports)?** Recommendation: dispose silently. The borrowing form reports nothing for the same outcome. A caller that wants every outcome uses `WhenEach(move tasks)`. A report would also be the only place a successful program writes to that channel because a sibling failed.

**Q4 — The value-result owning form returns `Task<Array<T>>` (as the borrowing form does) or `OwningTask<Array<T>>`?** Recommendation: `Task<Array<T>>`. Code then moves between the borrowing and the owning form by changing the argument only, and the scoped await-bind already gives the one-line consumer (`var rs := await Task.WhenAll(move pending);`). `OwningTask<Array<T>>` would let `using var rs := await …` own the array, at the price of two result types for one result kind.

**Q5 — The value-result owning form at a class T: a compile error (recommended), or admitted by keeping the inputs alive for the aggregate's life?** The second needs a result slot that chains N source tasks, a new kind beside §30.5's three. Class results that move are `OwningTask<T>`'s, and Pattern A exists for fan-out, not fan-in (§30.5).

**Q6 — Out of scope, for a separate issue?** Rule 2 says "on every path"; the compiler checks normal exits. A `cb(x)` that throws synchronously (a non-`async` function value returning a `Task`) on the third iteration exits the loop and disposes the two tasks already minted, unsettled. This happens under named bindings today and under B alike. Recommendation: file it as its own spec-question. Nothing here makes it worse or better.

## 11. Classifications (process §9)

- **Change class:** design change (two overloads), with a ruling (R1) folded in.
- **Kernel:** no K rule, axiom or register row changes. The discharge is §28.14 rule 2's existing K6.1 obligation, met by a new consuming position, and the owning overloads' `owned` parameter is K3's ordinary consume.
- **Body-locality:** the rule-2 credit stays local to the callee's body.
- **Absence check (process §5 item 3b):** no name is retired. `WhenAllOwned` was already retired in the documents, and only the compiler still uses it.
- **Composition:** v0.361's #8640 sentence is read, not edited. No other train in the v0.361 run edits §28.14 rule 2, §30.10's contracts, or stdlib §6.7's owning subsection (QUOTED, `git diff origin/main spec/v0361-2026-11`).

## 12. Probes and the anchor check

Programs and outputs are under `/var/tmp/bezel-scratch/v361/p-0218/probes/` (scratch, not committed), compiled with `bezelc 0.0.0-8ccf7ff0 --checked-disposal`. The ones the text relies on:

| Probe | Shape | Result |
|---|---|---|
| p01 | `OwningArray<Task>` filled in a loop, `using var view := owning.ToArray(); await Task.WhenAll(view);` | runs: `10`, `20`, `30`, `done` |
| p02 | the same over `Task<int>`, `using var all := Task.WhenAll(view); var rs := await all;` | runs: `60`, `20`, `40` |
| p03 | p01 inside `FanOut(cb: (int) -> Task, …)` | rejected: "the task minted by invoking call-bounded parameter `cb` here is not awaited to settlement" |
| p04 | `Task.WhenAll(owning)`, `owning: OwningArray<Task<int>>`, no `move` | accepted, runs: `10`, `2` (the #7429 divergence) |
| p05 | p03 with `await Task.WhenAll(owning)` (no snapshot, no `move`) | accepted, runs: `10`, `20`, `30`, `done` |
| p06c | the owning Pattern B aggregate (`WhenAllOwned`), inputs `Make(1)` 5 ms, `Fail()` 10 ms, `Make(3)` 200 ms | `produced 1`, `faulting`, `produced 3`, `dispose 1`, `dispose 3`, `caught boom`, `after`, `end` |
| p07 | the stdlib sketch as a user method over the same inputs | `produced 1`, `faulting`, `caught boom`, `after`, `produced 3`, `dispose 3`, `end` |
| p09 | `async` `OwningTask<OwningArray<Widget>>` body: owned local, faulting await, `return results;` | `dispose 1` never printed (three runs) |
| p10 | `TaskScope` child wrapping the call-bounded `cb` | rejected: "closure bound to a `scoped` parameter captures `cb`, a call-bounded function-typed parameter" |
| p11 | snapshot of `OwningArray<Task<int>>` to `WhenAny` and `WhenEach` | runs: `1`, `10`, `20`, `30` |

The p06c program, for the record (p07 replaces the `WhenAllOwned` call with a method whose body is stdlib §6.7's sketch):

```bezel
module M;
public class Widget
  public readonly property Id: int;
  public constructor(id: int)
    Id := id;
  end constructor;
  destructor
    Console.WriteLine("dispose " + Id.ToString());
  end destructor;
end Widget;

public class Program
  public static async method MakeAsync(id: int, delay: int): OwningTask<Widget>
    await Task.Delay(delay);
    Console.WriteLine("produced " + id.ToString());
    return new Widget(id);
  end MakeAsync;

  public static async method FailAsync(delay: int): OwningTask<Widget>
    await Task.Delay(delay);
    Console.WriteLine("faulting");
    throw new InvalidOperationException("boom");
  end FailAsync;

  public static async method Collect(owned tasks: OwningArray<OwningTask<Widget>>): Task
    using var results := await Task.WhenAllOwned(move tasks);
    Console.WriteLine(results.Count);
  end Collect;

  public static async method Main(): Task
    var tasks := new OwningArray<OwningTask<Widget>>();
    tasks.Add(MakeAsync(1, 5));
    tasks.Add(FailAsync(10));
    tasks.Add(MakeAsync(3, 200));
    try
      await Program.Collect(move tasks);
    catch
      var ex: InvalidOperationException => Console.WriteLine("caught " + ex.Message)
    end try;
    Console.WriteLine("after");
    await Task.Delay(300);
    Console.WriteLine("end");
  end Main;
end Program;
```

**Anchor check.** Each *old* string of §6 was checked by a script (`p-0218/check_anchors.py`) to occur exactly once in the v0.361 document it names, read with `git show spec/v0361-2026-11:<path>`.
