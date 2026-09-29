# Proposal 0014 — Signal hooks: SIGINT / SIGTERM → `Channel`

| | |
|---|---|
| **Status** | under-review — draft |
| **Target module** | `System` (auto-imported): `SignalKind` enum + `Signals` static class; callers `import System.Threading` to use the returned `Channel<SignalKind>` |
| **Written against** | spec v0.177 / stdlib v0.165 |
| **Tracking issue** | [#1442](https://github.com/s8lap/bezel-lang/issues/1442) |
| **Gap-review item** | #14 (P1) in [stdlib-gap-review-2026-07.md](stdlib-gap-review-2026-07.md) |
| **Unblocked by** | Bounded `Channel<T>` (v0.174, #809, stdlib §6.9) |
| **Currency note** | Syntax refreshed to v0.228 spellings (2026-07-28 sweep); re-verified clean at v0.306 (2026-09-17 sweep) — no tier-1 respell: stdlib §6.9's `Channel<T>`/`ChannelSender<T>` surface (including `TryReceive`'s out shape at a value `T`), §6.10's `TaskScope` (`Token`/`Cancel`/`Spawn`/`Join`), `ChannelFullException`, and spec §30.12/§30.14/§30.15 all still read as cited. **Pinned-claim record:** §4's note that bounding-object self-capture inside a `scoped` `Spawn` body is "in flight" is now landed — spec §28.14 states it normatively, with `scope.Spawn(lambda() => Work(scope.Token))` as its own example; the example needs no change. Written-against pin unchanged. |

---

## 1. The problem

POSIX signals (SIGINT from Ctrl+C, SIGTERM from `systemctl stop`) are the primary mechanism for requesting graceful shutdown of a server or long-running process. With no signal API, a Bezel program cannot intercept these: the OS default handler terminates the process immediately with no opportunity to flush buffers, close connections, or report state.

The central design constraint is §30.14: **domains share no mutable state.** Signal handlers fire on a runtime-managed thread that is *outside* every execution domain; they cannot directly invoke Bezel domain code. Channels are §30.14's authorized door — "the only mutable objects reachable from more than one domain; they are compiler-known, internally synchronized" (§30.15). Delivering a signal as a channel message is the only §30.14-sound mechanism.

---

## 2. Design decisions

### D1 — v1 signal set: Interrupt and Terminate only

`SignalKind.Interrupt` (SIGINT) and `SignalKind.Terminate` (SIGTERM). Rationale:

- **SIGKILL** is uncatchable by design — the OS kernel terminates the process unconditionally. No handler API makes sense for it.
- **SIGUSR1 / SIGUSR2** — application-defined; deferred until a stdlib consumer appears.
- **SIGHUP** — reload-config convention; deferred (narrow use case, easily added later).

### D2 — Return type: `owned Channel<SignalKind>`

`Signals.Subscribe(kind)` returns `owned Channel<SignalKind>` — the *receiver* end of a channel (stdlib §6.9: "`Channel<T>` — the receiving end"). `SignalKind` is a `byte`-backed enum, a sendable value type; the receiving interface is `Receive(): Task<SignalKind>`, `TryReceive(out item: SignalKind?): bool`.

The caller awaits ticks with `await chan.Receive()` and disposes the channel to unsubscribe. No intermediate wrapper type is introduced — using the existing `Channel<T>` directly keeps the surface minimal and compositions with other channel-select patterns natural.

### D3 — Multiple subscribers: fan-out, each gets their own channel

Multiple calls to `Subscribe(kind)` each return a distinct, independently-owned `Channel<SignalKind>`. When the OS delivers the signal, the runtime dispatcher sends to *every* registered channel for that kind. Each subscriber receives one delivery per signal. Consequence: N subscriptions to `SignalKind.Interrupt` each receive a message for a single SIGINT.

*Alternative considered and declined:* a single shared bus channel (callers share one `Channel<T>`, one receiver). This would require either a reference-counted shared channel (fighting the single-owner model of §28) or an external synchronization layer. Fan-out to independent owned channels is the cleaner ownership story.

### D4 — Delivery mechanism from signal handler to channel

*(Corrected in review, 2026-07-04: the original draft had the raw signal handler acquire the subscriber-list lock and call `Send` directly. That is **async-signal-unsafe** — POSIX permits only the async-signal-safe function set inside a handler; a lock acquisition can deadlock against the very thread the signal interrupted, and a channel send may allocate. §30.12 synchronization makes a call safe from a foreign **thread**, not from a signal handler, which is a more restrictive context than any thread.)*

Delivery is two-stage, the standard self-pipe pattern:

1. **The raw handler does one async-signal-safe operation and nothing else**: a `write(2)` of the signal number to a runtime-owned pre-created pipe (on Linux the runtime may instead use `signalfd(2)` and install no handler at all — an implementation choice behind the same surface). `write` is on the POSIX async-signal-safe list; no locks, no allocation, no runtime calls.
2. **A runtime drain thread** reads the pipe in ordinary thread context, and *there* takes the subscriber-list lock and calls `ChannelSender<SignalKind>.Send(kind)` on each registered sender — `ChannelSender` is internally synchronized (§30.12), making that call safe from a foreign *thread*, which the drain thread is. This is the §32 discipline applied where it actually holds.

The pipe is the coalescing point as well: if it is full (signals arriving faster than the drain thread runs), the `write` fails with `EAGAIN` and the delivery coalesces — consistent with D5's lossy contract and with the kernel's own pending-signal merging.

### D5 — Bounded channel capacity; coalescing on full

Each subscription channel is **bounded with a small internal capacity** (implementation default: 4). If the channel is full when a signal arrives, the delivery is **coalesced** (discarded silently). It is the runtime **drain thread** (D4) that catches the `ChannelFullException` from `Send` and suppresses it — never the raw signal handler, which only performs the async-signal-safe `write(2)` and touches no channel.

Rationale: POSIX signal semantics already permit coalescing — multiple identical signals pending to the same process may be merged to one delivery by the kernel before the handler runs. A Bezel program that has four unprocessed SIGINT messages has a slow signal consumer; the fifth is appropriately dropped rather than blocking the signal handler or growing an unbounded buffer. Lossy delivery of signals is the OS contract; this proposal makes it explicit.

*Exact-delivery form considered and declined.* An unbounded buffer (`new Channel<SignalKind>()`) could guarantee every signal delivery reaches user code. Against it: an unbounded buffer allows a backlog to grow without limit; in a no-GC language this is a correctness risk. A slow consumer is most likely a bug, and a bounded lossy channel surfaces the problem (the consumer can check its receive frequency against expected signal rates) instead of hiding it.

### D6 — Default-behavior replacement and restore-on-dispose

`Subscribe(kind)` replaces the OS default signal handler for `kind` (via `sigaction` / `signal`). Disposing the returned `Channel<SignalKind>` deregisters this subscription. When the last subscription for a given `kind` is disposed, the OS default handler is **restored** (`SIG_DFL`). This means:

- While any subscription exists, Ctrl+C does not terminate the process (the application handles SIGINT through the channel).
- When the subscription is disposed (graceful shutdown loop exited), the next SIGINT terminates normally.

Callers that want the OS default to remain active in parallel must re-raise the signal or call `Environment.Exit()` from their handler loop.

### D7 — Windows: deferred

Windows uses `SetConsoleCtrlHandler` (for CTRL\_C\_EVENT / CTRL\_BREAK\_EVENT) rather than POSIX signals. The `SignalKind.Interrupt` semantic is analogous but the delivery mechanism is different. Windows support is a follow-up — the `Signals` class will return an empty channel (no deliveries) on Windows in v1, with a warning comment in the docs.

---

## 3. Proposed stdlib additions (ready-to-land text)

### Proposed new enum `SignalKind` in `System` (auto-imported)

```bezel
// OS signals that can be intercepted by a Bezel process.
// v1 covers SIGINT and SIGTERM only (see Proposal 0014).
// SIGKILL is uncatchable by OS design and is not represented here.
public enum SignalKind: byte
  Interrupt,    // SIGINT  — Ctrl+C, or kill -INT <pid>
  Terminate;    // SIGTERM — kill <pid>, systemctl stop, container runtime shutdown
end SignalKind;
```

### Proposed new class `Signals` in `System` (auto-imported)

```bezel
// Process-level signal subscription.  Each subscription replaces the OS default
// handler for the requested signal kind for as long as the returned channel is live.
// Requires import System.Threading to use the returned Channel<SignalKind>.
public static class Signals
  // Subscribe to signals of the given kind.  Returns the receiving end of a
  // bounded channel (capacity 4); the runtime delivers one SignalKind message
  // per OS signal received, coalescing on a full channel.
  //
  // Multiple subscriptions to the same kind each receive an independent delivery;
  // disposing any one channel unregisters only that subscriber.
  //
  // When the last subscription for a kind is disposed, the OS default handler
  // for that signal is restored (SIG_DFL).
  //
  // Windows: deferred — v1 returns a channel that delivers no messages on Windows.
  public static method Subscribe(kind: SignalKind): owned Channel<SignalKind>;
end Signals;
```

(`Channel<SignalKind>` is a `SignalKind`-typed channel, per stdlib §6.9 — the value-type form:
`Receive(): Task<SignalKind>`, `TryReceive(out item: SignalKind?): bool`.)

---

## 4. Usage example

```bezel
import System.Threading;    // Channel<T>, CancellationToken

public async method RunServer(token: CancellationToken): Task
  using var sigint  := Signals.Subscribe(SignalKind.Interrupt);
  using var sigterm := Signals.Subscribe(SignalKind.Terminate);
  // Both channels receive independent deliveries; we merge via a helper task.
  await using var scope := new TaskScope();
  scope.Spawn(async lambda() => do
    var kind := await sigint.Receive();
    scope.Cancel();    // cancel the scope's token → cooperative shutdown
  end);
  scope.Spawn(async lambda() => do
    var kind := await sigterm.Receive();
    scope.Cancel();
  end);

  // ... normal server work using scope ...
  await scope.Join();
  Console.WriteLine("Graceful shutdown complete.");
end RunServer;
```

> Each spawned body references `scope` itself (`scope.Cancel()`) — a body capturing its own bounding `TaskScope`. Capturing the bounding object inside a `scoped`-bounded `Spawn` body is admitted by the v0.180 capture train (the fourth bounding form / scoped-construction, #1439, in flight); this example lands with it.

---

## 5. Declined alternatives

1. **Callback-based subscription** (`Signals.Subscribe(kind, lambda() => ())`) — the callback runs on the signal thread, outside any domain; touching any Bezel state from it violates §30.14. Channel delivery is the only §30.14-sound shape.
2. **`async` / `Task`-returning subscribe** (`Signals.WhenAsync(kind): Task<SignalKind>`) — returns a task that completes on the first signal. Cannot represent "receive every signal" without repeated calls; also raises the question of task ownership across domains. A channel is the right multi-delivery primitive.
3. **A shared bus channel** (all subscribers share one `Channel<T>`) — fights the single-owner model; fan-out to independent channels is cleaner.
4. **Unbounded internal buffer** — allows a slow-consumer backlog to grow without bound; against the defined-fault / no-silent-surprises discipline. Coalescing is the right behavior for signals, which POSIX itself coalesces under load.

## 6. Open questions for the owner

1. **Windows story timeline.** Should v1 raise `PlatformNotSupportedException` on Windows (fail loudly), or return a never-delivering channel (silent)? The latter is safer for cross-platform code that also has a cancel-token path; the former surfaces the gap earlier.
2. **Default-restore policy.** Should `Dispose` of the last subscriber always restore `SIG_DFL`, or should it restore whatever handler was installed *before* the first `Subscribe` (letting the user set a custom handler before subscribing)? The `SIG_DFL` policy is simpler and covers all stdlib-internal use cases.

## Delegated rulings 2026-08-11 (stdlib-wave readiness; owner review in repo)

Resolving the two §6 questions under the owner's 2026-08-11 delegation ("work autonomously... don't ask me for confirmations or rulings"; review-in-repo) (delegated ruling, 0012-precedent style). Status stays draft.

1. **Q1 — fail loudly: `Subscribe` on a platform without POSIX signal delivery raises `InvalidOperationException` with a message naming the gap.** A never-delivering channel is a structural *silent no-run* — a shutdown path that simply never fires, discovered in production — precisely the CodingKeys-class footgun the project's recorded reasoning treats as decisive (rationale §5.19; 0022 §3.2's identical indictment). The loud fault surfaces the porting gap at first test instead. On the exception type: `PlatformNotSupportedException` does not exist in the stdlib, and the 2026-07-04 owner ruling on 0007 Q2 declined widening the §31.4 taxonomy for the not-supported shape — `InvalidOperationException` with a reason-naming message is the established spelling, reused here. The §3 comment line ("returns a channel that delivers no messages on Windows") is **superseded by this ruling** and reads as the raising form at graduation. A dedicated platform-gap exception type is worth (re)considering only when a Windows port actually lands — evidence-gated, recorded, and its introduction would be that port's decision, not this proposal's.
2. **Q2 — restore the *saved previous* disposition, not unconditional `SIG_DFL`.** On the first `Subscribe` for a kind the runtime saves the prior disposition (`sigaction`'s old-action out-parameter); the last subscriber's dispose restores exactly that. In the common case (default disposition inherited) this *is* `SIG_DFL`, so the simple policy's behavior is fully preserved; the two diverge exactly when an embedder or an FFI-linked native library (§32) installed a handler before Bezel code subscribed — where unconditional `SIG_DFL` would silently destroy another component's handler on dispose, a cross-component interference with no diagnostic. Cost: one stored `sigaction` per subscribed kind. Declined: always-`SIG_DFL` (simpler by one field, silently clobbers embedder state); the §3 comment line reads as save-and-restore at graduation. A user-installed custom handler followed by `Subscribe`/dispose now round-trips correctly by construction.

With these, this proposal carries no open design question; the remaining path is the ordinary acceptance/graduation workflow.
