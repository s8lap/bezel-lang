# Bezel Guide 4: Concurrency — Tasks, Domains, and Channels

*A short, example-first guide. Companion to the Bezel Language Specification — re-validated against v0.331.*

The syntax here is mainstream async: `async` methods, `await`, tasks, cancellation tokens. The model underneath is not, and the habits that come with the syntax elsewhere will fight you. Four ideas carry the whole chapter. A **domain** runs one continuation at a time, so most code needs no locks (§1). A **task is a heap resource you own**, like every other class instance (§2). **No mutable state is ever reachable from two domains** — data crosses by ownership transfer or as immutable copies (§5). And fan-out defaults to a **structured scope**, not loose tasks (§4). If you skim everything else, read the three sections headed **How Bezel does it** and the table in §10.

The threading types this guide uses — `TaskScope`, `Parallel`, `Domain`, `Channel<T>`, the locks and atomics, `CancellationTokenSource` — live in `System.Threading`: `import System.Threading;`. Only `Task`/`Task<T>`/`OwningTask<T>`, `CancellationToken`, `Subscription`, and the event types are auto-imported — spec §2.3.

**Contents:** §1 domains: serial execution · §2 tasks are owned · §3 `Task<T>` vs `OwningTask<T>` · §4 structure first: `TaskScope` and `Parallel` · §5 spawning and sendability · §6 channels · §7 locks that own data · §8 statics under domains · §9 cancellation · §10 choosing.

---

## 1. How Bezel does it: one continuation at a time

All async code runs inside an *execution domain* — a unit of **serial** execution (spec §30.13). At any instant at most one continuation of a domain is running. Once started, a continuation runs uninterrupted until it suspends at an `await`, completes, or throws. A program starts with one domain; more exist only if you spawn them (§5). The consequence is the habit to unlearn first: **state shared between one domain's tasks needs no synchronization.**

```bezel
public class Stats
  private _served: int := 0;

  public async method Track(job: Task): Task
    await job;
    _served := _served + 1;    // no lock, no atomic: continuations of one
  end Track;                   //   domain never overlap (spec §30.13)
end Stats;
```

Two refinements keep this honest:

- **Interleaving still happens — at `await`, and only there.** Other continuations of your domain run while you are suspended. An invariant must hold *before* you suspend, and a check made before an `await` may be stale after it. This is why the compiler never flow-narrows mutable fields at all — spec §7.4 names exactly this interleaving among its reasons — and why it re-widens a mutably-captured local at every `await` (Guide 2 §1 covers both). Partially-written memory is never observed; logical interleaving is the only hazard.
- **No thread affinity, no contexts.** A domain is not a thread. The runtime may run successive continuations on different threads — never two at once — preserving visibility across the migration. There are no synchronization contexts and therefore no `ConfigureAwait`-style opt-out to remember — nothing is captured, nothing needs configuring (rationale §12.3).

**Blocking is fenced off, deterministically.** Inside a domain, a continuation blocked in `Wait()`/`Result` — or a blocking `Mutex.Acquire()` — freezes the very domain that must advance to unblock it. So the blocking forms are a *compile error* lexically inside `async` bodies — `await` is strictly better there (spec §30.3, §30.12). Where the compiler cannot see it — a non-`async` helper reached from a continuation — the runtime raises `InvalidOperationException` instead of parking: a defined fault, not a mystery hang (spec §30.3, §30.12).

```bezel
var cfg := LoadAsync().Result;    // ERROR inside async code: blocks its own domain — await it
```

The blocking forms remain fully legal *outside* any domain: the synchronous prefix of a non-`async` `Main`, exit-time destructors, and foreign threads entering through the FFI (spec §30.3, §22.13).

---

## 2. How Bezel does it: a task is a resource you own

Elsewhere a task is garbage-collected plumbing you may freely drop. In Bezel a task is a class instance with a disposal obligation, and one rule from Guide 1 §4 governs where the obligation lands: **calls mint, storage lends** (spec §28.3). Every call whose declared return type is `Task`/`Task<T>`/`OwningTask<T>` produces a *fresh task owned by you* — no `owned` marker needed, the return type is the type announcement (spec §28.3; the full declared-kind × callee-kind matrix is §28.3's announcement table). A read of task *storage* (a `TaskCompletionSource.Task` property, an admitted static cache) lends a borrow.

```bezel
using var task := File.ReadAllTextAsync(path);   // fresh task: yours, disposed at scope end
var text := await task;
```

The everyday shapes all discharge the obligation without ceremony:

- awaiting a bare producer call (`await File.ReadAllTextAsync(path)`) consumes the temporary task as part of the await;
- `using var` pins a task you will await below;
- `return await Producer();` chains it into your own task.

What you may **not** do is silently drop one — by the discard, or by the anonymous binding that reads like a holder and is not one:

```bezel
_ := DoTelemetryAsync();            // ERROR: disposing a task abandons its
                                    //   running operation (spec §17.13, §30.10)
using var _ := DoTelemetryAsync();  // ERROR too: nothing can await an
                                    //   unnamed binding, so the form is the
                                    //   discard's abandonment one scope later
                                    //   (spec §17.13; `await using var _` alike)
```

The two legal ways to *not* await a task (spec §30.10) — and in the second it is the **name** that separates it from the rejection above, because a name is what a later `await` would need:

```bezel
Task.FireAndForget(DoTelemetryAsync());   // supervised: runs to completion, exceptions logged
using var t := DoTelemetryAsync();        // unsupervised: disposed at scope end, without
                                          //   blocking; its exceptions are swallowed
```

Both are abandonment idioms, and abandonment carries the one temporal obligation in this chapter (spec §30.1): a suspended frame holds *borrows* of its receiver and plain arguments until the task **settles**. Once you stop supervising, nothing lexically bounds that moment. Awaiting before your borrows' owners go away discharges the obligation trivially; `TaskScope` (§4) discharges it structurally. Abandon only tasks whose inputs are owned (transferred in), copied, `string`/value-typed, or otherwise guaranteed to outlive the operation. A violation faults as `DisposedAccessException` in checked disposal mode.

**The trap: a borrowing collection slot is not a holder.** A borrowing `Array<Task<T>>` slot takes no ownership, so pushing a freshly-minted task into one compiles — and abandons the task at the end of the statement (stdlib §6.7, the recorded #1813 ruling). The compiler **warns right here** (spec §15.4's fresh-store lint, extended to the task family): the diagnostic names the abandonment and the two sanctioned spellings, so the trap announces itself instead of waiting for checked mode:

```bezel
var tasks := new Array<Task<string>>();
tasks.Add(File.ReadAllTextAsync(path));   // compiles — but WARNS (spec §15.4): the fresh
                                          //   task has no holder and is abandoned when
                                          //   the statement ends
```

The two sanctioned spellings — the ones the warning names: give each task a **named holder** and build the borrowed array over the names (below, §4), or use the owning family (`OwningArray<OwningTask<T>>` with the owning `Task.WhenAll`/`WhenEach` overloads — the same names, the parameter type distinguishing them and the `move` marker spelling the transfer: `Task.WhenAll(move tasks)`, spec §22.3/stdlib §6.7) for dynamically-sized fan-in. Dynamic fan-in over fresh tasks is an ownership-transfer scenario by construction.

---

## 3. Two kinds of task: `Task<T>` borrows, `OwningTask<T>` transfers

For value-typed, `string`, or no results, there is nothing to decide — `var n := await CountAsync();` works in any form. When the result is a **class**, the producer's return type picks one of two patterns (spec §30.5), and the consumer's obligations follow from the signature alone.

`Task<T>` is **Pattern A**: any number of awaits, every awaiter borrowing the one result, whose lifetime is the task's. `OwningTask<T>` is **Pattern B**, the default: one await, which spends the task whatever its outcome, and the awaiter owns the result. The full consumer rules — which operands an await accepts and where its result may flow — are spec §30.5's, tabulated in the *Awaiting a task* table of the code-generation reference's §4.3 ([`Bezel_For_Code_Generation.md`](Bezel_For_Code_Generation.md)); this section walks through the two shapes.

**Pattern B** is the shape you will write most, and it is the one-liner:

```bezel
using var conn := await ConnectAsync(url);     // yours; disposed at scope end
await using var s := await OpenSessionAsync(); // await using for async-disposable results
```

The await is *owned-consuming with respect to its operand*: awaiting an owned task local consumes the task itself, with no separate discharge needed. This is exactly how resource acquisition reads (`using var token := await sem.AcquireAsync();`, stdlib §6.3). The single-award rule is enforced twice over:

```bezel
using var t := sem.AcquireAsync();     // OwningTask<SemaphoreToken>
using var tok := await t;              // first award: the token is yours
using var tok2 := await t;             // runtime InvalidOperationException: already spent
```

With a *plain owned local* operand the compiler catches the retry statically instead — the first await leaves the binding moved-from, and the second await is a use of a moved-from binding (spec §30.5). Either way, one task, one award.

**Pattern A** needs the task pinned, because the result borrows *from* it:

```bezel
using var task := FetchConfigAsync();   // Task<Config>: fan-out
var cfg  := await task;                 // borrow; source: task
var cfg2 := await task;                 // fine — same instance, multi-award
```

The one-line bind of a Pattern A await needs no pin of your own: the compiler writes it (spec §30.5's **scoped await-bind**). A fresh `var` (or `const`) bind whose initializer is exactly the await of a fresh producer desugars to the two-line form above, the task held in a hidden slot no code can name, disposed at the end of the enclosing block (the compiler does not yet apply the scoped await-bind by default, [#3048](https://github.com/s8lap/bezel-lang/issues/3048); the example's first line shows the specified behavior):

```bezel
var cfg := await FetchConfigAsync();    // works: the task is pinned to a
                                        //   hidden slot until the end of this block;
                                        //   cfg borrows the Config through it
Render(await FetchConfigAsync());       // also fine: borrow confined to the statement,
                                        //   the task disposed at its end
```

One await is all the hidden slot gives you — it is unnameable, so to award twice (or hold the task) write the two-line form; it stays the shape to reach for whenever the *task itself* matters, and it is exactly what the one-liner desugars to. Two near-misses reject, each with a diagnostic that teaches (spec §30.5): `using var cfg := await …` (the await produces a *borrow*, which §30.5's may-not-flow list bars from a `using var` bind — you do not need to pin anything; the compiler pins it for you when you write `var`) and `cfg := await …` onto an existing local (`cfg` may outlive this block; bind the task first). The borrow flows like any other (Guide 1 §3): pass it, read it, but no `using var` over it, no store into an `owned` field, no owned return.

**Choosing, as producer** (rationale §12.2): ask *"can a second consumer legitimately need this same result instance?"* If yes — `Task<T>`, and say why at the declaration. If no, or unsure — `OwningTask<T>`. A fan-out façade over a Pattern B core is one line later (`return await FetchOwnedAsync(url);` from a `Task<Config>`-returning body). The reverse migration breaks callers.

**One declaration the compiler refuses outright:** a task over a **move-only value** — an owning `choice` such as `Result<Connection, string>` with a class payload, or an owning inline array — is rejected at the declaration (spec §30.5): multi-award would hand the same must-move-exactly-once value to every awaiter, and `OwningTask<T>` admits class types only. The idiomatic shape for a fallible async acquisition is `OwningTask<Connection>` plus a typed exception; an owned resource's failure payload is exception territory anyway (Guide 2 §7).

Faults are ordinary: an exception in the async body faults the task, and each await of a `Task<T>` re-raises it, as does the first await of an `OwningTask<T>` (a second raises `InvalidOperationException`, spec §30.4). The caught binding is a borrow, per Guide 2 §5 (spec §30.9).

---

## 4. Structure first: `TaskScope`, `Parallel`, and the free-range exception

The default posture for concurrent fan-out is the structured nursery, not a bag of loose tasks (spec §30.10; stdlib §6.10). A `TaskScope` owns its children for a lexical scope: no child outlives the scope, the first fault cancels the siblings, and the fault re-raises at one visible place.

```bezel
public async method RefreshBoth(primary: string, mirror: string): Task
  await using var scope := new TaskScope();
  scope.CancelAfter(TimeSpan.FromSeconds(30));           // deadline for the whole scope
  scope.Spawn(async lambda() => do
    await RefreshOneAsync(primary, scope.Token);         // children observe the scope's token
  end);
  scope.Spawn(async lambda() => do
    await RefreshOneAsync(mirror, scope.Token);
  end);
  await scope.Join();      // mandatory on the normal path: awaits all children,
end RefreshBoth;           //   re-raises the FIRST fault (siblings cancelled)
```

The mechanics worth knowing:

- Consumption is `await using` **only** (a plain `using var` is a compile error — the drain must be awaitable), and that binding is the scope's only owner: a `TaskScope` is never stored, passed or returned `owned`, used as a type argument (a generic call inferring `T` from it included), or converted to `Object` or an interface at an owning position. Pass it as a plain (borrowed) parameter of type `TaskScope` instead.
- `await Join()` is mandatory on the normal path and is the *single* re-raise site.
- The disposal edge (exception, early return) cancels, drains, and swallows — supervision lives at `Join`.

Captures have their own admission test. Child captures are admitted when their sources are declared *before* the scope (parameters always qualify) or derive from the scope itself (`scope.Token`). A **class- or interface-typed** local declared after the scope is rejected at the `Spawn` call, and so is a span or a record holding a borrow whose source is declared after it: such a capture counts by the sources of the borrows it carries. Value-typed captures that carry no borrow are exempt from the declared-before test, so an `int` loop index is fine (spec §28.14). This is the `scoped` admission: `Spawn`'s `body` parameter is declared `scoped`, the scope is the bounding object, and the captured sources stay frozen until it is disposed — the same storage-bounded contract Guide 1 §9 teaches for your own callback-holding classes. For per-element fan-out, declare the source collection *before* the scope and capture a value-typed index, or reach for `Parallel`.

**`Parallel`** is the uniform-work tool: the same capture-free function over every element of a collection, on a process-wide team of worker domains (stdlib §6.12; spec §30.14). It is also your second rehearsal of the Pattern A consumer shape (`Parallel` is not yet implemented, [#3557](https://github.com/s8lap/bezel-lang/issues/3557); the example shows the specified surface):

```bezel
using var job := Parallel.Map(paths, ComputeSize);   // ComputeSize: static (string) -> int;
var sizes := await job;                              //   inputs chunk-copied at the call;
                                                     //   results in input order
```

First fault cancels remaining work and surfaces at the await as `DomainFaultedException` (stdlib §6.12). `Reduce`'s combiner must be associative with the seed as its identity (stdlib §6.12).

**Free-range tasks** — start several, settle them together — are the exception, and the §2 holder discipline is the whole trick: name every task.

```bezel
using var t1 := File.WriteAllTextAsync(pathA, report);   // both start now — named holders
using var t2 := File.WriteAllTextAsync(pathB, report);
var both: Array<Task> := [t1, t2];                       // borrows of the named holders
await Task.WhenAll(both);                                // settles ALL inputs; re-raises the
                                                         //   first non-successful input's fault
```

`WhenAll` never abandons laggards (spec §30.10). `WhenAny` yields the *index* of the first task to settle and never re-raises — you inspect the winner (spec §30.10). For results-bearing named holders, await each after the `WhenAll` — a completed await resumes synchronously. For a dynamically-sized set, use the owning overloads — the same names, the parameter type distinguishing them, the `move` marker spelling the consuming form (spec §22.3): `Task.WhenAll(move tasks)` collects everything, `Task.WhenEach(move tasks)` streams each task as it completes (stdlib §6.7).

---

## 5. How Bezel does it: domains share nothing — data moves or is immutable

Parallelism is more domains (spec §30.14). Domains run simultaneously, and the single discipline that keeps §1's guarantees alive across them is: **every value that crosses a domain boundary is either moved or immutable.** No new type system — the ownership machinery you already have from Guide 1 is the safety story.

```bezel
public record Digest(Rows: int, Bytes: long);        // sendable value record

public sealed class Batch                            // sendable: sealed, wholly-owned state
  public owned Rows: OwningArray<Row>;               // Row: itself a sealed, sendable class
  public constructor(owned rows: OwningArray<Row>) Rows := rows; end constructor;
end Batch;

public static method Summarize(owned batch: Batch): Digest
  using var b := move batch;                         // adopt (Guide 1 §9)
  return new Digest(b.Rows.Count, 0);
end Summarize;

// In the spawning domain:
using var join := Domain.Spawn(Summarize, new Batch(LoadRows()));  // the batch MOVES in
var digest := await join;                            // value result: copied back at the join
```

The compiler enforces two things at every `Spawn` call site (stdlib §6.8):

- **The entry is capture-free** — a static method reference or a non-capturing lambda. Captures are borrows of the origin domain's scopes and cannot cross. Data enters *only* through the explicit argument (aggregate several values into one sendable class, an `OwningArray`, or send the rest over a channel).
- **Argument and result types are sendable** — the derived property that answers "may this value cross?" (spec §30.14). The rules you actually hit:
  - **Value types** are sendable iff **two independent conditions** both hold (spec §30.14) — the *classification* condition, that the type is neither **borrow-bearing** (no field of a non-runtime-managed reference type anywhere, spec §9.7; `string`-, `Type`- and function-typed fields are exempt from that predicate) nor **move-only** (spec §15.3), **and** the *per-component* condition, applied recursively: every field's type and every inline-array element type is itself sendable. Neither implies the other, and the second is what rejects a record with a function-typed field, which the first admits — a function value is never sendable, its closure being one heap cell every copy shares. So: primitives, enums, and those records and value-like choices whose fields and payloads are themselves sendable (§35.12 for choices). They cross as *copies*. `string` and `Type` cross as shared immutable references.
  - **A class is sendable iff it is transfer-closed**: `sealed` (or `closed`, every class of its module-closed hierarchy itself transfer-closed), and every field is a sendable value, `string`, `Type`, an `owned` sendable class, an owning inline array of sendable classes, or an owning-choice field that holds no borrows (spec §35.4's holds-borrows property — no plain class- or interface-typed payload, borrow-bearing record payload, or reference-element inline-array payload, at any depth) and whose every payload of every case is transfer-compatible — obligation-bearing payloads recursively: `owned` class payloads by their class's sendability, `owned` interface payloads never, owning-inline-array payloads by their element class (interface elements never), nested owning choices recursively; every other payload by its own type's sendability, so a function-typed payload disqualifies (spec §35.12) — no interface-, `Object`-, or function-typed fields, and no plain (non-`owned`) class fields other than `Type` ones, because a back-reference aliases something another domain owns. Such an instance crosses as an **owned transfer** of its whole ownership tree.
  - **Never sendable**: open classes, interfaces, function values, tasks, lock guards, `CancellationToken` (§9), and a class deriving from a library class — `sealed class MyError: Exception` included — whose inherited state no library section states transfer-compatible (spec §30.14 rule 2). Most library classes cannot be derived from at all: only `Encoding`, `Stream`, `TextReader`, `TextWriter` and the exceptions are open (spec §12.4). An owning choice does not cross directly (it is move-only, §3) but rides inside a transfer-closed owner's field — provided it carries no borrows and every payload is transfer-compatible (spec §35.4/§35.12).

```bezel
using var j1 := Domain.Spawn(lambda() => Index(repo));   // ERROR: entry captures repo
using var j2 := Domain.Spawn(Work, move sessions);       // ERROR when sessions is a
                                                         //   Dictionary<string, Session>:
                                                         //   borrowing slots of class
                                                         //   elements are not sendable
using var j3 := Domain.Spawn(Bump, counter);             // ERROR: a class argument crosses
                                                         //   only through the owned row —
                                                         //   Bump(owned c: Counter), move counter
```

The diagnostics name the failing rule. The fixes are always the same two moves: own what crosses (`OwningDictionary`, an `owned` field) or copy value-shaped data.

Joins are ordinary tasks of the spawning domain: a class result crosses as a Pattern B award, a value result as a copy. A fault in the entry surfaces at the join as `DomainFaultedException` carrying the original message and stack-trace *text* — exception objects themselves do not cross (spec §30.14). Async entries are the natural form: the runtime awaits the entry's task inside the spawned domain, exactly as it drives an `async Main`.

---

## 6. Channels: ownership in flight

A channel moves sendable values between domains with ownership transfer at both ends: `Send` consumes the item in the sending domain, awaiting `Receive` produces it owned in the receiving domain (spec §30.15; stdlib §6.9). The shape is multiple-producer, single-consumer. The `Channel<T>` object *is* the receiving end — keep it in one domain — and `NewSender()` mints independently-owned, sendable sender endpoints to move wherever producing happens.

The **actor idiom** — a domain that owns its state outright and serializes everything through an inbox — is the shape to reach for when lock *ordering* gets complicated (stdlib §6.11). It is a placement decision, not a default: rationale §12.14's error-kernel doctrine warns that state entangled enough to want serializing is often exactly the state whose loss is unrecoverable, so keep the fallible work out of the unit that holds it:

```bezel
public record Transfer(From: string, To: string, Amount: decimal);   // sendable payload

public static async method LedgerMain(owned rx: Channel<Transfer>): Task
  using var inbox := move rx;                              // adopt the owned parameter
  using var balances := new Dictionary<string, decimal>(); // domain-local state: no lock
  await for var t in inbox.ReceiveAll() do                 // drain; suspends while empty
    balances[t.From] := (balances.GetOrNull(t.From) ?? 0m) - t.Amount;
    balances[t.To]   := (balances.GetOrNull(t.To)   ?? 0m) + t.Amount;
  end;                                                     // completes normally on close
end LedgerMain;

// Elsewhere:
var ledger := new Channel<Transfer>();
using var pay := ledger.NewSender();                       // mint before moving the channel
using var actor := Domain.Spawn(LedgerMain, move ledger);  // the receiving end moves in

pay.Send(new Transfer("ada", "lin", 25m));                 // value payload: copied across
pay.Dispose();                // last sender disposed: the channel closes and drains
await actor;                  // the actor finishes its inbox and exits; faults surface here
```

The semantics to internalize:

- **Ordering and delivery.** Per-sender FIFO; senders interleave in an unspecified consistent order; each item is received exactly once.
- **Closing is graceful, twice over.** Disposing the *last sender* closes the channel: buffered items remain receivable, an active `ReceiveAll` yields the tail and completes normally, and only then do further receives raise `ChannelClosedException`. The receiver can close first-class with `Close()` — same drained state, idempotent. Disposing the `Channel<T>` itself is the abrupt form: undelivered class-typed items are disposed. The worked shutdown above is nothing but these rules composing.
- **Abandonment gives back; failed sends dispose.** Disposing a pending `Receive` task deregisters the waiter and *no item is consumed or lost* — one mid-handoff is returned to the channel's head. A send that faults goes the other way: `ChannelClosedException` (closed) and `ChannelFullException` (sync `Send` on a full bounded channel) both dispose the offered item first. **A channel never retains an item it did not accept**, and there is no hand-it-back failure path in this revision (a recorded deferral, spec §30.15). A producer that cannot afford the loss uses `SendAsync`, which awaits capacity instead of faulting.
- **Back-pressure is a constructor argument.** `new Channel<T>(capacity)` bounds the buffer — capacity ≥ 1, buffered only; there is no rendezvous form in this revision. `SendAsync` awaits space; the synchronous `Send` never blocks on any channel. Bound your inboxes: unbounded channels are an accepted leak under a runaway producer.
- **Class-typed items arrive owned.** `Receive()` is `OwningTask<T>` for class T — award it into a `using var`. In an owning `ReceiveAll` loop, adopt each item on the body's first line (`using var r := move report;`, Guide 1 §9). One consumption at a time: a second `ReceiveAll` or a `Receive` racing one raises `InvalidOperationException`.

Cross-domain *fan-in* is channels; the sanctioned multi-source shape is to merge: one forwarder per source, each sending into a single merged channel (spec §30.15).

---

## 7. Locks that own their data

Within one domain you need none of this (§1). The synchronization types earn their keep in exactly three places: state genuinely reached from several domains, foreign threads entering through the FFI, and code outside any domain (spec §30.12). When you do need a lock over data, Bezel's shape ties the two together: **the lock owns the value, and a guard is the only path in.** (`Locked<T>` and `RwLocked<T>` are not yet implemented, [#1721](https://github.com/s8lap/bezel-lang/issues/1721); the example shows the specified surface.)

```bezel
public static class Sessions
  private static owned readonly _map: Locked<OwningDictionary<string, Session>>
      := new Locked<OwningDictionary<string, Session>>(new OwningDictionary<string, Session>());

  public static async method Renew(id: string): Task
    using var g := await _map.LockAsync();           // guard: owned, Pattern B award
    g.Value[id] := Session.Renewed(g.Value[id]);     // owning indexer-set disposes the old one
  end Renew;                                         // guard disposed ⇒ lock released
end Sessions;
```

Construction consumes an owned interior. `g.Value` is a borrow *whose source is the guard*, so the borrow rules you already know make the `using var` scope the critical section: a reference cannot leave it, and killing a guard under a live borrow is a compile error (spec §30.12). There is no lock-next-to-data convention to forget. The rules around the edges:

- **Blocking discipline is §1's**: `Lock()`/`Read()`/`Write()` are compile errors inside `async` bodies — use the `Async` forms. `TryLockAsync(timeout)` returns `null` on timeout, and a cancelled acquisition never holds the lock (stdlib §6.11).
- **Non-reentrant, all of them** (`Mutex` included): a holder re-acquiring waits on itself. Never nest acquisitions of one lock — including a second `Read()` on the same `RwLocked<T>`, which may queue behind a waiting writer. `Semaphore` is not in this set: a second `Acquire()` by the same holder simply consumes another permit — not reentrancy, and legal while permits remain (spec §30.12).
- **`RwLocked<T>` read guards are write-restricted**: stores, `move`, and `ref`/`out` passes through a `ReadGuard`-sourced borrow are compile errors. Calling a *mutating method* through one, or passing it to a callee that writes through its parameter, is the recorded obligation the compiler cannot see, and no mode detects a violation (spec §30.12) — when in doubt, take `Write()`.
- **Interior must be a sendable class** (the §5 property, reused): own your values — `Locked<OwningDictionary<string, Session>>`, not `Locked<Dictionary<string, Session>>`.
- **Data leaves as values or by transfer**: copy value-shaped data out, compute under the guard, or `PopValue`/`Pop` an entry's ownership out. A guard borrow never escapes.
- **Holding a guard across `await` is legal but linted** — narrow the scope instead. Lock *ordering* is out of scope by design; when ordering gets complicated, that is the actor idiom's cue (§6).

The rest of the toolbox, in one breath: **`Mutex`** for a critical section that isn't guarding one datum (`using var guard := await m.AcquireAsync();`); **`Semaphore`** for permits; **`Atomic<T>`/`AtomicInteger<T>`** for cells — sequentially consistent, and the right spelling for the shared counter you were about to make a mutable static (§8). Cross-domain sharing of any of these rides `Shared<T>`: clone a handle, move the clone, both domains `Get()` (spec §28.13). `Shared<Mutex>` and `Shared<Locked<T>>` are the idiom; a `static owned readonly` lock (above) is the same thing with process-wide reach.

---

## 8. Statics under domains: process-global, so constrained

Static state is process-global — one instance across *all* domains — which makes an unrestricted mutable static a side door through §5's no-shared-mutable-state rule. Bezel closes the door at the declaration (spec §22.12): immutable content is free, synchronized handles are free, and everything else is warned or rejected.

The **type-keyed** half of the taxonomy is four per-type stdlib **facts** — **synchronized**, **immutable**, **self-distributing**, **displacement-unbounded** — and three one-line derivations (spec §22.12/§28.13; stdlib §1): **form (i)** is warning-free when its type is *synchronized and not displacement-unbounded*; the `static readonly` warning is suppressed when it is *synchronized or immutable*; and `Shared<T>` is sendable when its payload is *(synchronized or immutable) and not self-distributing*. One **Facts** line at the type's stdlib section answers all three questions.

- **`static readonly` of sendable-value-category value types, `string`, enums**, and **`const` of a primitive value type or `string`**: unrestricted — shared-immutable.
- **`static readonly` of a class type**: admitted **with a warning** — the singleton is reachable from every domain, and confining it to one (or ensuring its own synchronization) is your obligation. The warning is suppressed exactly where the type's own fact line carries the guarantee — **synchronized** or **immutable**: the synchronized handles below, the data-owning locks, `FrozenArray<T>`, `CancellationTokenSource`, the process-global shutdown source, and `Type`, the interned type token (spec §22.12 rule 4's machine-checked set).
- **A mutable static** is a compile error unless it is one of four admitted forms: **(i)** an internally-synchronized handle — a type declaring **synchronized** and not **displacement-unbounded**: `Mutex`, `Semaphore`, the atomics, `ChannelSender<T>`, `SharedEventSource<T>`; **(ii)** the `Task?`/`Task<T>?` lazy cache (below); **(iii)** a value static of primitive or enum type — or the nullable form of either, or a plain (non-`owned`) inline array of those — admitted *with a warning* and no cross-domain promises; **(iv)** the owning-inline-array slot pool, likewise warned. (`Channel<T>` declares no facts — the channel is the single-consumer receiving end — publish a `ChannelSender<T>` static instead. The locks, `FrozenArray<T>`, and `CancellationTokenSource` are kept out of form (i) by **displacement-unbounded**: a mutable static's overwrite never disposes what it displaces, and stranding a lock's whole interior — or a whole frozen array — is the cost the fact names.)

**The `Task?` cache** is the one admitted warning-free mutable static that holds a value your own code computes — form (i)'s members are all internally-synchronized handles. It exists because async values do not belong in static initializers (`await` is illegal there). Cache the *task*, not the value, and let every consumer await it — Pattern A fan-out through process-lived storage (spec §22.13):

```bezel
private static _cfg: Task<Config>? := null;              // §22.12 rule 2(ii)
public static async method GetConfig(): Task<Config>
  if _cfg = null then _cfg := LoadConfigAsync(); end;
  return await _cfg!;                                    // every caller borrows through the cache
end GetConfig;
```

The caveat is in the check-then-store: it is *not* atomic across domains, so racing first uses may each start the work. Last store wins, every stored task remains awaitable, and work is duplicated — but nothing is unsound. When exactly-once matters, assign the cache eagerly in `Main` before any spawn (spec §22.13 pattern (a)). And the **blocking-wrapper trap**: a synchronous wrapper that waits on the cached task is admitted only in code outside any domain — startup code — and never from a lazily-triggered initializer. From a continuation it is §1's deterministic error (spec §22.13).

**The value-static trap** — branching on a domain's flag without synchronization:

```bezel
private static _warm: bool := false;    // admitted, but WARNED (§22.12 form (iii)):
                                        //   another domain's read may be stale forever —
                                        //   form (iii) stores carry no publication promise
private static owned readonly _ready: Atomic<bool>
    := new Atomic<bool>(false);         // warning-free: the synchronized spelling
```

**The process-wide publication point** is a typed static, not a member kind: `SharedEventSource<T>` — internally synchronized, subscribable from any domain, admitted warning-free as a `static owned readonly` field (spec §22.12 rule 5; stdlib §6.5):

```bezel
public static class App
  private static owned readonly _exiting: SharedEventSource<()> := new();
  public static property Exiting: ISharedEvent<()> => _exiting;
  public static method NotifyExiting() _exiting.Raise(); end NotifyExiting;
end App;

using var sub := App.Exiting.Subscribe(OnExit);   // handler MUST be capture-free:
                                                  //   a static method ref or
                                                  //   non-capturing lambda (§6.5)
```

Handlers run on whichever domain raises — which is exactly why every subscribe position on the shared pair admits only capture-free handlers, the same admission as spawn entries. (Instance `EventSource<T>` fields are the ordinary, unsynchronized, capture-admitting kind — domain-serial per §1.)

---

## 9. Cancellation is cooperative

There is no forcible abort. A `CancellationToken` is passed in, the operation checks it at safe points, and cancellation surfaces as `OperationCanceledException` at an `await` (spec §30.8). The conventions:

```bezel
public async method SyncAll(items: Array<string>,
                            token: CancellationToken := CancellationToken.None): Task
  for var item in items do
    token.ThrowIfCancellationRequested();     // check at each safe point
    await CopyOneAsync(item, token);          // and thread the token inward
  end;
end SyncAll;

using var cts := CancellationTokenSource.WithCancelAfter(TimeSpan.FromSeconds(30));
await SyncAll(paths, cts.Token);              // awaited inside the source's scope
```

Three rules keep it sound:

- **The source outlives its tokens.** A token is a borrow-bearing record observing its `CancellationTokenSource`. It is the *single* stdlib-certified borrow allowed into an async frame, on one normative condition: the source must outlive the settlement of every task observing it (spec §30.1, §30.8). Awaiting inside the source's `using var` scope, as above, discharges it. Disposing a source *cancels* it (cancel-on-dispose), so `using var cts := …WithCancelAfter(d);` **is** a cancellation scope (stdlib §6.1).
- **Compose with linked sources**: "cancel when the caller cancels *or* my deadline fires" is `CancellationTokenSource.CreateLinked(callerToken, timeoutSrc.Token)` — bind both sources, reverse-order disposal does the rest (stdlib §6.1).
- **Tokens do not cross domains; the *source* does, behind a handle.** `CancellationToken` is a borrow-bearing record and is never sendable, and a bare `CancellationTokenSource` is not sendable either — but the source carries the **synchronized** fact, so **`Shared<CancellationTokenSource>` is sendable** (spec §28.13's `Shared`-payload set; §30.14): clone a handle, move the clone, and the receiving domain reads its own token through it and polls at its safe points. The handle, not the borrow, keeps the source alive, which is how the source-outlives obligation above is discharged across domains (stdlib §6.1, §6.8). A **published** source — one consumed by `new Shared<CancellationTokenSource>(…)`, held in a `static readonly`/`static owned readonly` field of the type, or linked as the child of a published parent — is **signal-only**: `Register` on it raises `InvalidOperationException`, and publishing a source that already holds live registrations raises at the publishing operation, so polling (`IsCancellationRequested`/`ThrowIfCancellationRequested`) is the cross-domain idiom (stdlib §6.1). Where no source is shared, cross-domain cancellation is a message on a channel. Intra-domain, `TaskScope.Token` is the structured spelling (§4).

Deadline-shaped stdlib operations signal timeout by returning `null` (`TryAcquireAsync`, `TryLockAsync`), not by throwing — cancellation faults, timeout is a value (stdlib §6.1).

---

## 10. Choosing the right tool

| You want… | Reach for |
|---|---|
| One async result, one consumer (the default) | `OwningTask<T>` — `using var x := await Call();` (§3) |
| One result, many readers | `Task<T>` — pin with `using var task`, awaiters borrow (§3) |
| Subtasks that finish or fail together | `TaskScope` + `await scope.Join()` (§4) |
| The same capture-free function over every element, all cores | `Parallel.Map`/`Reduce`/`ForEach` (§4) |
| A long-lived stateful worker | the actor idiom: `Domain.Spawn` + `Channel<T>` (§5–§6) |
| Streaming work between domains, with back-pressure | a bounded `Channel<T>` + `SendAsync` (§6) |
| Structured shared state | `Locked<T>` / `RwLocked<T>` — the guard is the critical section (§7) |
| A shared counter or flag | `AtomicInteger<T>` / `Atomic<T>` (§7) |
| Cross-domain mutual exclusion | `Shared<Mutex>` — clone a handle per domain (§7) |
| A process-wide notification | a `SharedEventSource<T>` static, capture-free handlers (§8) |
| A lazily-computed process-wide value | the `Task<T>?` static cache, awaited by every consumer (§8) |
| Background work nobody waits for | `Task.FireAndForget(call())` — and only with owned/copied inputs (§2) |
| To stop something | a `CancellationToken` threaded through; across domains a `Shared<CancellationTokenSource>` handle, polled (§9) |

A few habits carry most of it. Trust the domain: inside one, state is just state, and the lock you reach for by reflex is usually noise. Name your tasks: `using var` for anything you will await below. Treat "who settles this task?" as seriously as Guide 1 treats "who disposes this object?" — because it is the same question. Fan out inside a `TaskScope` unless you can say why not. And when state must genuinely be shared, make the sharing carry its own guarantee — a channel, a data-owning lock, an atomic, a shared event — rather than a convention. The compiler rejects most of the shortcuts — blocking waits in async code, capturing spawn entries, non-sendable arguments, mutable statics among them. The few obligations it leaves to you are settlement of abandoned tasks and source-outlives-token, both backstopped in checked disposal mode, and reader purity through a read guard, which no mode detects (spec §30.12). The concurrency chapter is Guide 1's chapter, run in parallel.
