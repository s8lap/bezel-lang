# Proposal 0023 — `Task.WhenEach`: streaming task completions

| | |
|---|---|
| **Status** | graduated-v0.212 — superseded — graduated v0.212 (owner-accepted 2026-07-21 in-session; D1–D7 as drafted) |
| **Tracking issue** | [#1450](https://github.com/s8lap/bezel-lang/issues/1450) |
| **Target module(s)** | `System.Threading` (stdlib §6) — added as new members on the `Task` static class (§6.7) |
| **Written against** | spec **v0.177**; stdlib companion v0.165 |
| **Origin** | Gap review [#1403](https://github.com/s8lap/bezel-lang/issues/1403) item #15 (2×P1), wave D3 of the [stdlib gap DESIGN plan](papers/stdlib-gap-design-plan-2026-07.md) |
| **Dependency** | None blocking; sits downstream of `TaskScope` §6.10 (v0.175) but is independent of it |

---

## 1. The problem

When multiple tasks run concurrently, `Task.WhenAll` collects all results but forces the caller to wait for the *slowest* task before processing any result. The missing piece is a streaming form: yield each task to the caller as soon as it completes, in completion order, so fast results can be processed immediately without stalling on slow ones.

`Task.WhenEach` fills this gap: given a collection of tasks, it returns an async-enumerable sequence in which each element is an already-completed task. The caller `await for`s the sequence and, for each yielded task, awaits it a second time to observe its result or re-raise its fault.

This is the ungated half of gap-review item #15. `TaskScope` (§6.10, v0.175, #808) shipped without a co-designed `WhenEach`; this proposal designs the missing piece independently and establishes that the two APIs have well-defined, non-overlapping roles.

---

## 2. Design decisions

### D1 — Return type: `IAsyncEnumerable<Task<T>>`, not a channel

**Decision.** `WhenEach<T>` returns `IAsyncEnumerable<Task<T>>`. Each element is an already-completed `Task<T>`; the caller awaits the element to observe the result or re-raise a fault.

*Channel-based alternative considered and declined.* A channel-based shape (`WhenEach` returning a `Channel<Task<T>>`) was considered. It was declined: `Channel<T>` is designed for cross-domain ownership transfer of *sendable* values (stdlib §6.9), and `Task<T>` is not sendable — tasks carry live state tied to a single domain (§30.14). An `IAsyncEnumerable<Task<T>>` is domain-local and reuses the existing `await for` desugaring of §30.7 without any additional machinery. The shape also matches the .NET 9 precedent, which confirms the design is sound in practice.

*Why an already-completed task as the element, not the result directly.* Yielding the completed `Task<T>` (rather than the `T` result) preserves the fault-transport path: a faulted task's exception surfaces when the caller awaits the yielded element, exactly as it would in the direct-await case. Yielding the result directly would require a separate side channel for faults, which contradicts Bezel's "exceptions travel with the task" model (§30.9). It also unifies the no-result `Task` overload (which has no result to yield) and the value-result `Task<T>` overload under the same pattern.

### D2 — Single consumption (affine)

**Decision.** The `IAsyncEnumerable<Task<T>>` returned by `WhenEach` is **single-consumption** (affine): calling `GetAsyncEnumerator` a second time — directly or by passing the value as an `IAsyncEnumerable<T>` argument a second time — raises `InvalidOperationException`.

*Basis.* The §29.11 single-consumption rule governs lazy, single-pass async sources: "a lazy query … is single-consumption. It may be enumerated at most once." The rule and its enforcement mechanism (static compile error where flow-analysis can see the re-use; `InvalidOperationException` at runtime where it cannot) apply to `IAsyncEnumerable<T>` sources by the same principle — the spec §29.13 defers to §30.7 which inherits §29.11's model.

The specific precedent is ruling #1226 (2026-07-01): a bound lazy LINQ query is affine. `WhenEach` is precisely this: a *bound* lazy query over a task-completion stream. Completions are ephemeral and unrepeatable; re-enumerating cannot replay them. Single-consumption is therefore both the correct language-model choice and the only semantically coherent one.

### D3 — Relationship to `TaskScope` §6.10: observer, not lifecycle owner

**Decision.** `WhenEach` is an unscoped **observer** over an explicit task collection. `TaskScope` is the **lifecycle owner** of the tasks it spawns.

`TaskScope` (§6.10) owns its children: it cancels laggards on fault, drains all children at scope exit, and re-raises at `await Join()`. `WhenEach` does none of this: it monitors completions but is not a structured-concurrency nursery. The two APIs are cleanly separated:

- **Use `TaskScope` when** you want structured fan-out with lifecycle management, cancellation propagation, and fault isolation.
- **Use `WhenEach` when** you have an already-existing collection of tasks and want to process results as they arrive without structured scope.

Calling `WhenEach` over tasks spawned by a `TaskScope` is valid: `WhenEach` observes their completions; `TaskScope` still owns their lifecycle. The tasks-outlive-the-enumeration requirement is **compiler-tracked, not a programmer promise**: `WhenEach` borrows the input task array, and its returned `IAsyncEnumerable` is a deferred-activation frame that *carries that borrow* (spec §28.3 "Deferred-activation callees", v0.179 — the returned iterable is a borrow-carrying value whose source is the argument array). The borrow tracker therefore requires the task array (the borrow source) to outlive the `await for` loop, and rejects a program that would dispose it first, exactly as it rejects any borrow that outlives its source.

### D4 — Cancellation: stops enumeration, tasks keep running

**Decision.** The `CancellationToken` passed to `GetAsyncEnumerator(token)` (standard §30.7 / §4.12 shape) cancels the **enumeration** — no more elements are delivered and the iterator stops waiting for further completions — but it does **not** cancel the underlying tasks. The tasks continue to run to completion regardless.

*Why.* `WhenEach` does not own the tasks (see D3). Cancelling tasks on behalf of the caller would be an action on resources the method was only lent, violating Bezel's ownership discipline (§28). Callers who want tasks to be cancelled on enumeration cancellation must pass their tasks a `CancellationToken` separately, which is the standard cooperative cancellation pattern (§30.8).

### D5 — Empty-input behavior

**Decision.** An empty input collection produces an enumerator that completes immediately (yields no elements). `MoveNextAsync()` returns `false` on the first call.

### D6 — Faulted tasks are yielded, not skipped

**Decision.** A task that completes with a fault is yielded as a normal element. The fault surfaces when the caller awaits the yielded element; `OperationCanceledException` surfaces analogously for a cancelled task. The enumeration continues delivering other completions; `WhenEach` never stops because one input task faulted.

### D7 — Owning variant: `WhenEachOwned<T>` for `OwningTask<T>` elements

**Decision.** An owning variant is provided. It takes ownership of the input task array and yields each completed `OwningTask<T>` through the `IOwningAsyncEnumerable<OwningTask<T>>` interface (§4.13). The `await for` desugaring picks `GetOwningAsyncEnumerator()` and `TakeCurrent()`, producing an `owned OwningTask<T>` iteration variable that the caller discharges by awaiting it in an owned-consuming position (Pattern B, §30.5).

Tasks not consumed before enumeration ends (e.g., when cancelled) are disposed by the enumerator during `DisposeAsync` — each uncollected `OwningTask<T>` has a destructor that handles unconsumed results (§30.5, §6.7 `FireAndForget` note).

---

## 3. Proposed stdlib addition (ready-to-land text)

### Addition to §6.7 — `Task` static helpers

*Insert after the `WhenAllOwned` discussion:*

```bezel
// ── WhenEach ──────────────────────────────────────────────────────────────────────
//
// Streaming fan-in over a task collection.  Returns an IAsyncEnumerable whose
// elements are already-completed tasks, delivered in completion order.
// The returned sequence is single-consumption (affine) per spec §29.11 / #1226.
//
// The input array is borrowed (WhenEach does not take ownership of the tasks).
// Cancelling the token stops enumeration but does NOT cancel the tasks.

// No-result form.
public static method WhenEach(tasks: Array<Task>): IAsyncEnumerable<Task>;

// Value-typed or string result (Pattern A).
// Each element is an already-completed Task<T>; await it to obtain the result.
public static method WhenEach<T>(tasks: Array<Task<T>>): IAsyncEnumerable<Task<T>>;

// Owning form for class T (Pattern B).
// Takes ownership of the input array (and all tasks it contains).
// Yields each completed OwningTask<T> as an owned iteration variable; the caller
// discharges it via `using var result := await task` inside the loop body.
// Tasks not consumed before enumeration ends are disposed by the enumerator.
public static method WhenEachOwned<T>(
    owned tasks: OwningArray<OwningTask<T>>
): IOwningAsyncEnumerable<OwningTask<T>> where T: class;
```

**Semantics:**

- Elements are delivered in **completion order**: the first element yielded is whichever task completed first; ties are broken in unspecified order.
- A **faulted** or **cancelled** task is yielded normally. The fault or `OperationCanceledException` surfaces when the caller awaits the yielded element.
- An **empty** input array produces an enumerator that completes immediately.
- The `CancellationToken` argument to `GetAsyncEnumerator(token)` (standard §4.12 / §30.7 shape) fires only at `await MoveNextAsync()` boundaries; when fired, `OperationCanceledException` propagates to the caller's continuation.
- The `IAsyncEnumerable` / `IOwningAsyncEnumerable` returned is **single-consumption**: a second `GetAsyncEnumerator` call raises `InvalidOperationException`.

---

## 4. Usage examples

```bezel
import System.Threading;

// Process results as they arrive, not in submission order.
public async method RunFastestFirst(
    urls: Array<string>, client: HttpClient): Task
  var tasks := new Array<Task<string>>();
  for var url in urls do
    tasks.Add(client.GetStringAsync(url));
  end;
  await for var t in Task.WhenEach(tasks) do
    var content := await t;         // Pattern A: borrow from t; lifetime bounded by tasks
    Process(content);
  end;
end RunFastestFirst;
```

```bezel
import System.Threading;

// Owning form: load configs, process each as it arrives.
public async method LoadAllAsync(paths: Array<string>): Task
  var tasks := new OwningArray<OwningTask<Config>>();
  for var p in paths do
    tasks.Add(LoadConfigAsync(p));
  end;
  await for var t in Task.WhenEachOwned(tasks) do
    using var cfg := await t;       // Pattern B: own the result; t is discharged by await
    await ApplyAsync(cfg);
  end;
end LoadAllAsync;
```

```bezel
// With cancellation: enumeration stops; tasks continue running.
await for var t in Task.WhenEach(tasks) with scope.Token do
  var v := await t;
  Process(v);
end;
```

---

## Rulings recorded at acceptance (2026-07-07, delegated ruling — owner review pending)

No open owner questions. The D1–D7 design decisions are accepted as drafted: `IAsyncEnumerable<Task<T>>` (not a channel — `Task<T>` is not sendable, §30.14); single-consumption/affine per §29.11 and ruling #1226; observer-not-lifecycle-owner split from `TaskScope`; cancellation stops enumeration but not the tasks; faulted tasks yielded not skipped; the `WhenEachOwned<T>` owning variant over `IOwningAsyncEnumerable`. One editorial fix was applied at acceptance (D3): the borrowed-tasks lifetime obligation is now stated as a **compiler-tracked borrow** (the returned iterable is a §28.3 deferred-activation frame carrying the task array's borrow, v0.179) rather than a programmer promise. The new members land as §6.7 `Task`-static-helper amendments through the normal docs workflow.

## 5. Declined alternatives (register)

1. **Channel-based shape** — `Channel<Task<T>>` is for cross-domain ownership transfer of sendable values; `Task<T>` is not sendable; domain-local `IAsyncEnumerable<Task<T>>` reuses existing machinery without new types. (§D1)
2. **Yield the result directly (`IAsyncEnumerable<T>`)** — loses the fault-transport path; faulted tasks would need a separate side channel. (§D1)
3. **`IAsyncEnumerable<T>` as non-affine, re-enumerable** — completions are unrepeatable; re-enumeration cannot replay them; affine is the only sound choice. (§D2)
4. **Cancel tasks when the token fires** — `WhenEach` does not own the tasks; cancelling borrowed resources violates §28. (§D4)
5. **Stop enumeration on first fault** — hides partial results; callers who want to stop on fault can do so in the loop body. (§D6)
6. **A `WhenEach<T>` on `Task<T>` for class T that transfers result ownership** — Pattern A (borrow) is the right choice when the caller holds the task array and its lifetime naturally bounds the borrows; Pattern B (ownership transfer) belongs to `WhenEachOwned`. Two orthogonal overloads are cleaner than one overloaded by element ownership. (§D7)
