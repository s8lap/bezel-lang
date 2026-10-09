# Proposal 0168 — An unsettled task's disposal attaches the frame's discharge; a faulted task retains the task whose fault it carries

| | |
|---|---|
| **Status** | graduated-v0.334 — superseded — graduated v0.334 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues" and the 2026-10-09 direction "resolve all findings autonomously": SIMPLIFY C8's keep-and-fix merge — disposing any `Task`-family value before it settles attaches `OwningTask<T>`'s continuation, which performs at the terminal state every disposal the teardown would have performed, the task's block staying live until then; a task faulted by an instance an awaited task owns carries it as a borrow and retains the operand task (#5787 option 2); critique round 1 applied, which covered a `Result` re-raise through a synchronous helper and stated the block's deferred reclamation for composition with proposal 0162; landed with the v0.334 changelog entry) |
| **Tracking issue** | [#5788](https://github.com/s8lap/bezel-lang/issues/5788) (`spec-defect`: a `Task` or class-typed `Task<T>` disposed before settlement has no stated discharger for what its frame produces afterwards), [#5787](https://github.com/s8lap/bezel-lang/issues/5787) (`spec-defect`: a task-owned fault instance that propagates uncaught out of an `async` body faults the enclosing task, which §30.9 also makes its owner); one `spec-tracking` compiler tracker is filed at landing (§7) |
| **Target module(s)** | **spec**: §30.5 (the *Disposing an unconsumed `OwningTask<T>`* subsection retitled *Disposing an unsettled or unconsumed task*, with a new anchor; the continuation paragraph; the faulted-task sentence), §30.1 (one clause on the `owned`-parameters paragraph's last sentence), §30.9 (*Fault ownership*: the opening sentence; the disposal moment; a new retention paragraph replacing the "one exception" lead), §30.10 (the borrowing aggregators' obligation extends through retention). **stdlib**: none (§6.6/§6.7's fault-path comments stay true). **rationale**: §12.10 (one dated paragraph). **code-generation reference**: none (no compile-time rule changes; §4.3's awaiting table stays true). **`DECLINED.md`**: four `declined-at-proposal` rows (§9) |
| **Written against** | spec/stdlib **v0.333** at `429fd72` (main). Section numbers are v0.333's. |
| **Origin** | The disposal matrix's first run, lane L5 (RC-L5-2 and RC-L5-3; cells R7/C10, R2/C10, R8/C10, R7/C11, R3/C10, R1/C10), consolidated as roots RC53 and RC52 and filed as #5788 and #5787; the simplification review's C8 ("give every `Task`-family value `OwningTask<T>`'s continuation rather than forbidding unsettled disposal"). It needs a proposal because it changes two stated rules: §30.9's "disposed … when the task is disposed" moment and its "a faulted task owns its fault exception" ownership for the escaping case. |

*Reference convention: unprefixed §-references are language-spec references at v0.333; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

Two roots share one teardown. §30.1: "Disposing a not-yet-settled task does not cut this short: the underlying operation runs to its terminal state, … and the frame's parameters are disposed then." What the frame delivers into the task *after* its disposal has a stated discharger only for `OwningTask<T>` — §30.5: "the `Dispose()` call attaches a continuation that disposes the produced result whenever it appears". A `Task<C>` (Pattern A) disposed under the named `using var t := call();` drop §30.10 names as legal leaves its kind-1 result, the source task it later chains (an A12 `using var` deferral, or an `owned` parameter chained by `return await`) and its fault instance to a teardown that already ran (#5788; R7/C10, R2/C10, R8/C10, and the iterator face R7/C11 — minted `Task<C>` yields disposed unsettled at the next advance).

§30.9: "A faulted task **owns its fault exception**", and an awaited operand task's disposal is deferred "until the exception's handling completes". When an `async` body awaits a faulting `Inner` and lets the instance escape, it faults `Outer`, which the sentence makes a second owner; and the frame's handling never completes, so a temporary `Inner` has no disposal moment at all (a leak if `Outer` is never awaited), while two awaiters of `Outer` could re-raise an instance the first arm's deferred disposal freed (#5787; R7/C10, R3/C10, R1/C10, R2/C10).

The decision takes the two mechanisms the text already has and makes each the rule:

1. **Disposing an unsettled task attaches the frame's discharge.** Every `Task`-family disposal before settlement attaches a continuation that, at the terminal state, performs every disposal the teardown would have performed: a Pattern B or kind-1 result, the chained source (a late A12 deferral included), the retained operand task, the fault instance. §30.9's moment reads "at the later of the task's disposal and its settlement".
2. **A faulted task retains the task whose fault it carries.** An instance re-raised into a body from an awaited task and let escape stays owned by that task; the body's task carries it as a borrow and retains the operand task — moved in where the frame owned it (a temporary, an owned local, an `owned` parameter), deferred where it was a `using var` (A12's shape), by borrow under §30.1's extended obligation where it was borrowed — exactly as the owning `WhenAll` aggregate retains its first non-successful input. The capture ends the frame's handling.

No program's accept/reject verdict changes. Measured: forbidding unsettled disposal (the alternative) would have rejected 7 rule-corpus cases, 3 Guide 04 examples and about 15 test programs.

## 1. The problem

### 1.1 A `Task<C>` disposed before settlement (#5788)

```bezel
class Res
  public property N: int;
  destructor
    Console.WriteLine("closed");
  end destructor;
end Res;
class Demo
  public static async method MakeAsync(): Task<Res>
    await Task.Delay(50);
    return new Res();              // kind 1: the task owns the result
  end MakeAsync;
  public static async method Run(): Task
    do
      using var t := MakeAsync();  // never awaited
    end;                           // disposed unsettled: no result yet
    await Task.Delay(100);         // the frame returns a Res into a disposed task:
  end Run;                         //   §30.5's continuation is stated for OwningTask<T> only
end Demo;
```

§30.3's destructor "disposes any unconsumed `owned` result" — none exists yet. The kind-2 shapes: a `using var` source whose disposal A12 defers "to the outer task's teardown", and an `owned` parameter chained by `return await <param>` ("the outer task's teardown, not this rule, disposes it") — both formed after that teardown ran. The fault: §30.9 disposes it "when the task is disposed", which already happened; §30.10's "silently swallowed" names the idiom, not a discharger.

### 1.2 A fault that escapes into the enclosing task (#5787)

```bezel
class Demo
  public static async method Inner(): Task
    await Task.Delay(10);
    throw new InvalidOperationException("boom");  // Inner's task owns the instance
  end Inner;
  public static async method Outer(): Task
    await Inner();   // temporary's disposal waits "until handling completes";
  end Outer;         //   the instance escapes and faults Outer's task, which "owns" it too
  public static async method Run(): Task
    using var o := Outer();
    for var i in 0..<2 do                 // §30.4: every awaiter re-raises the same instance
      try
        await o;
      catch
        var e: Exception => Console.WriteLine(e.Message)  // pass 2: disposed already?
      end try;
    end;
  end Run;  // variant: never await o -> Inner's task is never disposed
end Demo;
```

§30.9 already solves the shape for one aggregate: the owning `WhenAll` "owns no fault instance of its own", re-raises "as a borrow" the fault owned by the input, and "the first non-successful input task itself is **retained by the aggregate and disposed with it**, so the re-raised instance remains owned by its own task, transitively, for the aggregate's lifetime". The borrowing aggregators hold their inputs under §30.1's obligation "extended through consumption". Nothing generalizes either to an `async` body.

### 1.3 The compiler (`429fd72`)

- `src/runtime/bezel_runtime_concurrency.cpp:993` (`bz_task_set_result_disposer`), `:1234` (`bz_task_free` runs the result disposer), `:4302` ("Pattern-B kind-1 extraction still carries its own result_disposer"): a result disposer exists for owned results; whether a `Task<C>` disposed unsettled defers its free to the terminal state is unmeasured.
- `:868–876` (`fault_borrowed`, set on a `WhenAll` aggregate that propagated an input's fault), `:880–900` (`bz_mark_exc_borrowed`, the per-object borrowed set for a bound task's re-raise), `:903–930` (`bz_defer_exc_dispose`: a task disposed while its own fault is in flight defers disposal to the catch arm's normal exit). No retention of an operand task by the task it faults exists; what happens to `await Inner()`'s temporary when the instance escapes is unmeasured (HYPOTHESIS: the frame's unwind disposes the temporary, and the deferred-disposal set keeps the instance alive until a handler ends — which no handler in the frame does).

## 2. Measurement

- **The alternative's reach (forbidding unsettled disposal).** `scratch/matrix2/simplify_work/reach_out/inspect_c8_task_never_awaited.txt`, the `await`-initialized rows dropped: rule corpus `s30_10_0001`, `0002`, `0006`, `0007`, `0009`, `0010`, `s30_5_0008` (7); Guide 04 `guide4_0003`, `0004`, `0012` (3); gtest `collection_task_add_*_2999`, `task_combinators` (:748, :764, :1108), `async_lambda_iife_*_4159/4118`, `using_var_direct_closure_chain_double_free_4162`, `buildspecmethodfn_*_4157`, `chained_return_owned_param_source_3126`, `static_class_closed_surface_3024` (about 15 programs); stdlib §6.7's `DoTelemetryAsync` example; spec §30.5's own `using var t := semaphore.AcquireAsync();`.
- **This proposal's reach.** Newly rejected: nothing. Newly admitted: nothing. Run-time semantics newly defined: the late result/chain/fault disposal after an unsettled disposal; the retained operand's disposal moment; the extended obligation on a borrowed operand (a residual already recorded for the borrowing aggregators).

## 3. Options

- **A — status quo.** Unsound (§1).
- **B — forbid disposing an unsettled task** (SIMPLIFY's measured alternative). Rejects the §30.10 idiom (§2).
- **C — frame-side discharge** (#5788 option (b)): a frame completing into an already-disposed task disposes what it would have stored.
- **D — generalize §30.5's continuation to every `Task`-family value** (#5788 option (a); SIMPLIFY C8).
- **E1 — ownership moves to the task the instance faults** (#5787 option (1)).
- **E2 — the faulted task retains the operand task** (#5787 option (2), its recommendation).
- **E3 — the faulted task re-raises a fresh cause-chained instance** (#5787 option (3)).

## 4. Evaluation

| | Soundness | Simplicity | Consistency | Migration reach |
|---|---|---|---|---|
| B | closed by removal | one rule | contradicts §30.10's stated idiom and §30.5's own example | 7 + 3 + ~15 sites rejected |
| C | closed for results; nothing for a chain formed after the teardown or a fault (the frame does not hold them) | two statements of one fact | — | 0 |
| **D** | closed for every kind: the continuation performs exactly the teardown's disposals | one subsection, two mechanisms merged (the `OwningTask<T>` continuation and "a faulted task has no owned result") | the sentence already stated for `OwningTask<T>` | 0 |
| E1 | breaks `Inner`'s other awaiters (§30.4) | one sentence | against the `WhenAll` precedent | 0 |
| **E2** | one owner per link; the retained chain is disposed transitively, exactly once | reuses kind-2's move, A12's deferral and the borrowing aggregators' obligation | the `WhenAll` precedent generalized | 0 |
| E3 | sound | one sentence | changes exception identity across `await`; against §30.9's preserved-stack-trace re-raise | 0 (semantic) |

## 5. Decision (delegated ruling, 2026-10-08)

**D + E2.** Normatively:

1. ▲ §30.5, *Disposing an unsettled or unconsumed task* (new anchor `sec-disposing-an-unsettled-task`, the old anchor kept): disposing any `Task`-family value before settlement attaches a continuation that runs at the terminal state and disposes whatever the frame delivers into the task — a Pattern B or kind-1 Pattern A result, the kind-2 chained source (an A12 deferral among them), the retained operand task, the fault instance; every disposal the teardown would have performed at or after settlement is performed by the continuation instead, exactly once. The frame's `owned` parameters stay the frame's (§30.1). The disposed task's own block stays live until the terminal state — its reclamation at the holder's release point is deferred there, never awaited or blocked on (the shape proposal 0162 gives an in-flight direct `DisposeAsync()`), so the frame's late delivery and the continuation reach a live task. The faulted-task sentence points at §30.9's moment.
2. ▲ §30.1's last sentence of the `owned`-parameters paragraph names the continuation for the frame's late deliveries.
3. ▲ §30.9: a faulted task owns the instance that is its own (a `throw` in its body, an imperative fault path); the disposal moment is "at the later of the task's disposal and its settlement"; a new paragraph: an instance re-raised from another task — by an `await` or a `Result` read, in the body (an async iterator's included, whose fault faults its `MoveNextAsync()` task) or in a synchronous call the body makes — and let escape, uncaught or by a bare `throw;`, stays owned by that task, the faulted task carries it as a borrow and retains the operand task; the capture ends the frame's handling; retention follows the operand's ownership state (owned → moved in as a kind-2 chained source; `using var` → deferred as A12 defers; borrowed → by borrow under §30.1's obligation extended through retention, K4.3, flag-backstopped; held by no frame binding — a task a synchronous callee disposed in its own unwind while the instance was in flight → the in-flight deferral of the instance's disposal runs on to the faulted task's teardown instead of ending at the capture); the retained task and the instance are disposed with the faulted task, transitively, exactly once; a fresh instance constructed over the caught borrow is the body's own fault. The aggregation helpers "state this same rule for their inputs".
4. ▲ §30.10: the borrowing aggregators' inputs must outlive every await of the aggregate "and of every task that retains the aggregate".

Kernel classification: **derivation** — K1 (one owner per link), K2 + K6 (the chained-slot move, as kind 2), K4.3 (the borrowed operand's residual, the recorded family), K6.1 (settlement as the bound); no new clause, no axiom extended (A12's deferral is applied to a second shape with the same ground). Body-locality: no compile-time rule added.

## 6. Edits at graduation

The exact edits are `scratch/v0334/W7.json` ids `W7-0168-1` … `W7-0168-9` (spec §30.5 ×3, §30.1, §30.9 ×3, §30.10; rationale §12.10). Seam: W3 edits §28.12 and the trivial-dispose paragraphs; W7 edits none of them. Composition with W3's proposal 0162 (explicit disposal marks; the holder frees): a `Task` is a class, so 0162's compiler-inserted free would reclaim an unsettled task's block at its holder's release point; `W7-0168-2` states the block stays live until the terminal state (reclamation deferred, never blocking), the same deferral 0162 states for an in-flight direct `DisposeAsync()` — 0162's own text covers only that receiver, not a disposed unsettled task. Order inside the terminal state: the frame disposes its `owned` parameters, the continuation then performs the teardown's disposals, and the block is released last; and `W7-0168-4`'s §30.1 clause sits in the `owned`-parameters paragraph, not the borrow-bearing paragraph W5/W6 may touch.

## 7. Compiler facts and the tracker

- **Tracker (spec-tracking):** (i) a `Task`/`Task<C>` disposed before settlement must defer the teardown's disposals to the terminal state — kind-1 result, late-chained source (A12 `using var`, `return await <owned param>`), fault instance — probe the §1.1 program under ASan (expected `closed`, no leak); (ii) a task-owned instance escaping an `async` body: the body's task retains the operand task (temporary moved, `using var` deferred, borrowed held), the instance disposed once with the retained task — probe the §1.2 program (expected two `boom` lines, one `closed` for an instance with a destructor) and its never-awaited variant under ASan (no leak); (iii) `fault_borrowed`'s shape generalized from the aggregates to every retaining task. Trace the retention through `bz_task_set_chained` (`:1020`) rather than a new slot where it fits.

## 8. Corpus cases to write (after landing)

- `s30_5_*`: a `Task<C>` disposed unsettled, result later produced → disposed (run: `closed`); the kind-2 late chain over an `owned` parameter after the outer task's disposal (run); a faulting task disposed before it faults (run: no leak — an ASan-only property, record the gap).
- `s30_9_*`: the §1.2 program with two awaiters (run: two `boom`); the never-awaited `Outer` disposing `Inner` and the instance (run, output ordering); a `using var inner := Inner();` operand whose fault escapes (run: `inner` disposed at `Outer`'s teardown, not at the block's end).
- Headers unchanged: `s30_9_0001`–`0004`, `s30_10_*`, `s30_5_0008` (they quote sentences this proposal keeps).

## 9. Declined alternatives (one `declined-at-proposal` row each)

- **B** — forbidding unsettled disposal. Reopening bar: —(none stated).
- **C** — frame-side discharge. Reopening bar: —(none stated).
- **E1** — ownership moves to the faulting task. Reopening bar: —(none stated).
- **E3** — a fresh cause-chained re-raise. Reopening bar: —(none stated).

## 10. Self-critique (adversarial pass)

1. **Does the continuation double-dispose with an awaiter?** After an unsettled disposal every further await raises `InvalidOperationException` (§30.5, unchanged), so no awaiter consumes a result the continuation disposes; a Pattern B result awarded before the disposal is already the awaiter's and the continuation finds none.
2. **A retained borrowed operand and the freeze.** A task's carried borrows are not traced by K5 (K6.4); the retained operand rides the same recorded residual as a borrowed `Result` source, extended as the borrowing aggregators' is — the honest limit, flag-backstopped. Stated, not hidden.
3. **An `owned` parameter operand.** §30.1 says a parameter "never moved out" is disposed at the terminal state; retention is a move-out at the fault, the same reading §30.1 gives `return await <param>`. `W7-0168-4` names the retained source there.
4. **A `catch` that handles and then a later `throw;`.** The bare re-raise keeps the same instance in flight (§31.2.4); the retention paragraph names it. A `throw ex;` of the arm's borrow is not admitted (§31.1), so no third route exists.
5. **Cancellation.** A cancelled operand raises a fresh `OperationCanceledException` (§30.3); it is the body's own fault instance and retains nothing — consistent with `WhenAll`'s cancelled-input clause.
6. **An aggregate's fault escaping into `Outer`.** `Outer` retains the aggregate, the aggregate owns no fault, the input owns it; the §30.10 obligation now extends to `Outer`'s awaits through retention. Stated at §30.10.
7. **The iterator face (R7/C11).** A borrowing iterator yielding minted `Task<C>` values disposes each at the next advance; if unsettled, the continuation disposes its late result — the general rule, no iterator clause needed.
8. **Is "at the later of disposal and settlement" compatible with the two timing refinements?** Yes: the in-flight deferral and the consumed-operand terminal act are later still, and the sentence keeps them as refinements of the moment.
9. **Nothing newly rejected, so why a proposal?** Because §30.9's ownership sentence and its disposal moment are settled rules the text changes (process §2's design-change line), and SIMPLIFY ranked the forbidding alternative, which had to be declined on measured reach.
10. **(Critic) An owning task no frame binding holds.** A `Result` read inside a synchronous helper the body calls (`Helper(Inner())`, the helper's own `using var`, or `Inner().Result` as a temporary there) re-raises an instance whose owning task the helper's unwind disposes under the in-flight deferral; the instance then escapes the async body. None of owned / `using var` / borrowed names that operand, and "the capture ends the frame's handling" would end the deferral and free the instance the faulted task carries. The retention paragraph now keeps that deferral to the faulted task's teardown.
11. **(Critic) Async iterators.** An async iterator's body fault faults the task its `MoveNextAsync()` returned; that task is the retaining task, and §29.7's unwind sentence names the exception for an awaited operand whose fault escapes.
12. **(Critic) Composition with 0162.** See §6: the disposed unsettled task's block outlives its holder's release point until the terminal state.
