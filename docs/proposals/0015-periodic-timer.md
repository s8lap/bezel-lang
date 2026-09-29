# Proposal 0015 — `PeriodicTimer`

| | |
|---|---|
| **Status** | under-review — draft |
| **Target module** | `System.Threading` (§6) |
| **Written against** | spec v0.177 / stdlib v0.165 |
| **Tracking issue** | [#1443](https://github.com/s8lap/bezel-lang/issues/1443) |
| **Gap-review item** | #13 (2×P1) in [stdlib-gap-review-2026-07.md](stdlib-gap-review-2026-07.md) |
| **Implementation substrate** | #1378 (timer-thread runtime) |
| **Currency note** | Syntax refreshed to v0.228 spellings (2026-07-28 sweep); re-verified clean at v0.306 (2026-09-17 sweep) — no tier-1 respell: `TaskScope` (stdlib §6.10), `ChannelSender<T>.Send` (§6.9), `TimeSpan.From*` (§2.5), `CancellationToken.None` (§6.1) and A.7's `LambdaBody ::= "do" Statement* "end"` all still read as cited. **Pinned-claim records:** §3's "if §6.11 is taken" conditional now applies — §6.11 is `Locked<T>`/`RwLocked<T>` since v0.186; and §4's note that bounding-object self-capture inside a `scoped` `Spawn` body is "in flight" is landed — spec §28.14 states it normatively with `scope.Spawn(lambda() => Work(scope.Token))` as its own example. Written-against pin unchanged. |

---

## 1. The problem

There is no way to run a recurring action at a fixed interval in Bezel today. Polling loops that call `DateTime.UtcNow` and compute `Elapsed >= period` work, but they busy-spin (or require `Task.Delay` reimplementing the same interval logic manually). The `Stopwatch` (§8.1) measures elapsed time but does not yield at an interval. `Task.Delay` (stdlib §6.7 — `Task.Delay(TimeSpan)`) covers a one-shot delay but not a repeating one.

`PeriodicTimer` fills this gap: an owned, disposable timer that yields the calling task once per period. It is designed for the single-consumer async loop — heartbeats, cache-refresh, health-check emission, cleanup sweeps.

---

## 2. Design decisions

### D1 — Fixed-DELAY ticking, not fixed-RATE

**Decision.** The next tick fires `period` after the *previous tick was delivered* (i.e., after the previous `WaitForTickAsync` returned `true`), not `period` after the timer was created.

This is the fixed-delay policy: wall-clock drift accumulates if the consumer is always a little late, but there are no "catch-up" tick bursts.

*Fixed-rate alternative considered and declined.* A fixed-rate timer fires at times `start + n * period` regardless of consumer latency. When the consumer is slow, ticks accumulate in a backlog. In an async loop this is especially surprising: the caller is busy in the tick body, the timer fires again, and the *next* `WaitForTickAsync` returns immediately without ever suspending — or worse, multiple ticks pile up. A fixed-delay timer never produces a queue of pending ticks; the consumer proceeds at whatever rate it can sustain. `.NET 6`'s `System.Threading.PeriodicTimer` uses the same policy and the same rationale.

### D2 — Coalescing: at most one pending tick

**Decision.** If the period has elapsed more than once since the last `WaitForTickAsync` returned, **only one tick is pending**. The next `WaitForTickAsync` call returns immediately (the accumulated ticks collapse to one); subsequent calls wait for the next period.

Consequence: a PeriodicTimer cannot be used to count the exact number of elapsed intervals if the consumer is slower than the period. A caller that needs to know "how many ticks did I miss?" compares `DateTime.UtcNow` to the expected tick time itself.

This is the right policy for Bezel's no-GC ownership model: there is no collection of pending `Task` objects to manage, no queue of ticks to drain, and no path where an unbounded backlog builds up. The timer is a live OS resource with a single consumer.

### D3 — `WaitForTickAsync(token)` returns `Task<bool>`

**Decision.** `WaitForTickAsync(token: CancellationToken := CancellationToken.None): Task<bool>`.

- Returns `true` when a tick arrives.
- Returns `false` when the timer has been disposed (either `Dispose()` was called before the await, or it is called while the await is outstanding).
- Raises `OperationCanceledException` when the cancellation token fires.

`false` is the disposal signal rather than a separate exception, matching the `IAsyncEnumerator.MoveNextAsync(): Task<bool>` convention (§29's async iterators use `false` as the "iteration complete" signal). The caller loop reads:

```bezel
while await t.WaitForTickAsync(scope.Token) do
  // ... tick body ...
end;
```

After `Dispose`, the loop exits cleanly. After cancellation, `OperationCanceledException` propagates to the calling task.

*Alternative: raise `ObjectDisposedException` on dispose.* Declined. The dispose-while-awaiting case is a normal, expected event in a structured-concurrency scope — the scope cancels, the timer disposes, and the loop needs to exit. `false` allows a loop to be written without a `try/catch` for the disposal case.

### D4 — Dispose-while-awaiting

If `Dispose()` is called while a `WaitForTickAsync` call is outstanding, the pending task is **completed with `false`** rather than faulted. The `Dispose()` call itself returns synchronously after completing the pending task.

Only one `WaitForTickAsync` may be outstanding at a time; a second concurrent call raises `InvalidOperationException`.

### D5 — NOT sendable

`PeriodicTimer` holds a live OS timer handle (the #1378 runtime's timer-thread backing). It is **not sendable** (spec §30.14, live-resource family: a handle to a mutable runtime resource cannot be transferred between domains). Passing a `PeriodicTimer` to another domain would be a compile error.

This aligns with `Stopwatch` (§8.1) and `Mutex` / `Semaphore` guards (§6.2–§6.3): synchronized state *can* be sendable, but a live timer resource is domain-local.

### D6 — Relationship to #1378

`PeriodicTimer`'s implementation requires a timer-thread substrate (the ability to schedule a callback after a duration without blocking a domain continuation). Issue #1378 is the tracked runtime work for that substrate. This proposal specifies only the API shape; the implementation depends on #1378.

---

## 3. Proposed stdlib addition (ready-to-land text)

### Proposed new §6.11 — `PeriodicTimer`

*(Current §6.10 is `TaskScope` (v0.175). If §6.11 is taken, adjust numbering at integration time.)*

```bezel
// A timer that yields the caller once per period.  Single-consumer: only one
// WaitForTickAsync may be outstanding at a time.  Domain-local: not sendable.
//
// Ticking policy: fixed-delay — the next tick fires `period` after the previous
// tick was delivered, not from the wall-clock start.  At most one tick accumulates
// if the consumer is slower than the period (coalescing).
//
// Impl substrate: #1378 (timer-thread runtime).
public class PeriodicTimer: IDisposable
  // Create a timer that fires every `period`.
  // ArgumentOutOfRangeException if period <= TimeSpan.Zero.
  public constructor(period: TimeSpan);

  // The configured tick interval (Q1 ruling: read-only, no setter in v1).
  public readonly property Period: TimeSpan;

  // Await the next tick.
  //   Returns true  when a tick fires.
  //   Returns false when the timer has been disposed.
  //   Raises OperationCanceledException when `token` is cancelled.
  // Only one WaitForTickAsync may be outstanding at a time;
  // a concurrent second call raises InvalidOperationException.
  public async method WaitForTickAsync(
      token: CancellationToken := CancellationToken.None): Task<bool>;

  destructor;    // completes any outstanding WaitForTickAsync with false; stops the timer
end PeriodicTimer;
```

**Notes:**

- `TimeSpan.FromSeconds`, `TimeSpan.FromMilliseconds`, etc. (§2.5) are the natural period constructors.
- `CancellationToken.None` (§6.1) is the default; passing no token yields a loop that runs until `Dispose` or the program ends.
- `using var t := new PeriodicTimer(period);` disposes the timer at scope exit, completing any outstanding await with `false`.

---

## 4. Usage examples

**Heartbeat loop inside a `TaskScope`:**

```bezel
import System.Threading;

public async method HeartbeatLoop(sender: ChannelSender<string>, ct: CancellationToken): Task
  using var timer := new PeriodicTimer(TimeSpan.FromSeconds(5.0));
  while await timer.WaitForTickAsync(ct) do
    sender.Send("ping");
  end;
end HeartbeatLoop;
```

**Cache refresh with a structured-concurrency scope:**

```bezel
import System.Threading;

public async method RunWithRefresh(): Task
  await using var scope := new TaskScope();
  scope.Spawn(async lambda() => do
    using var timer := new PeriodicTimer(TimeSpan.FromMinutes(1.0));
    while await timer.WaitForTickAsync(scope.Token) do
      await RefreshCacheAsync();
    end;
  end);
  // ... main work ...
  await scope.Join();
end RunWithRefresh;
```

When `scope.Cancel()` fires (due to a fault or signal), `scope.Token` is cancelled, `WaitForTickAsync` raises `OperationCanceledException`, and the Spawn body exits — cleanly completing the scope drain.

> The Spawn body reads `scope.Token` — the body references its own bounding `TaskScope`. Capturing the bounding object inside a `scoped`-bounded `Spawn` body is admitted by the v0.180 capture train (the fourth bounding form / scoped-construction, #1439, in flight); this example lands with it.

---

## 5. Declined alternatives

1. **Fixed-rate ticking.** Catch-up bursts surprise callers and build up an implicit backlog. Fixed-delay is the safe, self-limiting policy. (§D1)
2. **`IAsyncEnumerable<DateTime>` shape.** A `foreach await` loop over ticks is ergonomic, but `IAsyncEnumerable<T>` introduces an enumerator object and ownership of it (§29, §4.13); the simple `while/await bool` loop is lighter. Addable later.
3. **Raising `ObjectDisposedException` on dispose-while-await.** The dispose-while-awaiting case is expected in normal structured-concurrency teardown; `false` allows a graceful exit without `try/catch`. (§D3)
4. **Sendable `PeriodicTimer`.** A live OS timer handle is a domain-local resource; sending it crosses the §30.14 domain boundary. (§D5)
5. **`new PeriodicTimer(period, autoStart: bool)`.** Every caller wants the timer running immediately; a non-running timer with a separate `Start()` call adds surface without a consumer.

## 6. Open questions for the owner

1. **`Period` property — RESOLVED (delegated ruling, 2026-07-07; owner review pending): yes.** `PeriodicTimer` exposes a read-only `Period: TimeSpan` property (added to the §6.11 surface above). It is useful for logging/diagnostics and costs nothing; the *setter* is a separate question (Q2, deferred).
2. **Period change after construction.** Should the caller be able to change the period after construction? A `Period` setter would allow adaptive intervals (e.g. exponential backoff). Deferred: the use case is real but adds concurrency surface (what happens to an outstanding await when the period changes?).

## Delegated ruling 2026-08-11 (stdlib-wave readiness; owner review in repo)

**Q2 — the `Period` setter is DECLINED for v1, evidence-gated for later.** The setter is not merely additive surface: it must define the outstanding-await interaction — whether an in-flight `WaitForTickAsync` re-arms against the new period, completes on the old schedule, or faults — which is genuine concurrency-semantics design with no current consumer to test it against. The v1 workaround is dispose-and-recreate, which makes the re-arm *explicit* at the call site instead of implicit in a setter's contract. **Evidence bar for reopening:** a named consumer needing adaptive intervals (e.g. an exponential-backoff poller) files the want; the reopening design must specify the outstanding-await rule as its first sentence. Declined alternatives: shipping the setter with an unspecified outstanding-await interaction (the silent-surprise class the defined-fault discipline exists to prevent); a `Restart(period)` method (same question, different spelling). With Q1 already ruled (2026-07-07), this proposal carries no open design question.
