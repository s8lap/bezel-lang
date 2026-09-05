# Proposal 0011 — Child processes (`System.Diagnostics.Process`)

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-07, delegated ruling — owner review pending) |
| **Target module** | `System.Diagnostics` (stdlib §8; the type is `System.Diagnostics.Process`, the "open shape" deferred in stdlib [§11](../Bezel_Standard_Library.md#sec-modules-deferred-to-future-revisions)) |
| **Written against** | spec/stdlib **v0.177** |
| **Tracking issue** | [#1440](https://github.com/s8lap/bezel-lang/issues/1440) |
| **Gap-review item** | #5 (child process spawning), wave **D2** of the [stdlib gap DESIGN plan](stdlib-gap-design-plan-2026-07.md) |
| **Proposed-dependencies** | **0006** (concrete stream family — `StreamReader`/`StreamWriter`/`MemoryStream`/`Stream.CopyToAsync`, [#1433](https://github.com/s8lap/bezel-lang/issues/1433)); everything drawn from 0006 is marked **(0006)** where it appears |
| **Design influences** | .NET `Process`/`ProcessStartInfo` for the surface *shape*; Go `os/exec` for argv-array-not-shell and the drain-both-pipes discipline; Rust `std::process` for owned-handle lifecycle; **not** .NET's synchronous `WaitForExit`, **not** any shell-string spawn |
| **Currency note** | Syntax refreshed to v0.228 spellings (2026-07-28 sweep); refreshed 2026-08-11 to v0.246: caller-side transfer markers respelled `move` (declaration-side `owned` unchanged). Written-against pin unchanged. |

The stdlib names `System.Diagnostics.Process` as an anticipated module whose "shape is open" (stdlib [§11](../Bezel_Standard_Library.md#sec-modules-deferred-to-future-revisions)). This proposal settles that shape against the v0.177 language: an **owned class** wrapping the OS child handle, with stdio expressed as the **0006 stream family**, **async-only** exit waiting (the blocking-wait form is already declined — [gap review](stdlib-gap-review-2026-07.md) §Declined #5), a `Process`-that-is-**not-sendable** classification following 0007's `TcpConnection` precedent, and a typed spawn-failure taxonomy rooted under `IOException`.

Every snippet was checked against the v0.177 grammar (Appendix A) and against the actual stdlib surface. No type or method is used that is not either already in `Bezel_Standard_Library.md` (cited), introduced by proposal **0006** (marked), or introduced here as **proposed** (marked). This is a document-only design pass: no compiler behaviour is asserted, only what the surface *should* be.

---

## 1. What exists today (the substrate this builds on)

Everything below is already in the language/stdlib or in proposal 0006; this proposal composes over it and invents only the process-specific surface.

- **`Stream`** (abstract, stdlib [§7.1](../Bezel_Standard_Library.md#sec-stream)) — the byte-oriented base with `ReadAsync`/`WriteAsync` (windowed `Array<byte>` and `Span<byte>` forms), `ReadAll`/`ReadAllAsync`, `Flush`. A pipe is a `Stream`.
- **(0006)** `StreamReader: TextReader` / `StreamWriter: TextWriter` (`constructor(owned stream: Stream, encoding := Encoding.UTF8)`), `MemoryStream` (`FromBytes`/`ToArray`/`TakeBytes`), and `Stream.CopyToAsync(destination, bufferSize := 81920, token := CancellationToken.None): Task`. These are the *text* and *in-memory-capture* wrappers over a pipe. They take the wrapped stream **`owned`** (0006 §3 Q1) — a fact that decides Q4 below.
- **`Task`, `Task<T>`, `OwningTask<T>`** (spec [§30.3](../Bezel_Language_Spec.md#sec-the-task-taskt-and-owningtaskt-types)/[§30.5](../Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)); `async`/`await`, `await for` async iteration (spec [§30.7](../Bezel_Language_Spec.md#sec-async-iterators)).
- **`CancellationToken`**, **`CancellationTokenSource`** with `WithCancelAfter` and `CreateLinked`/`CreateLinkedAll` (stdlib [§6.1](../Bezel_Standard_Library.md#sec-cancellationtoken-and-cancellationtokensource), v0.173); **`TaskScope`** (stdlib [§6.10](../Bezel_Standard_Library.md#sec-taskscope), v0.175) for concurrent-drain; **`Channel<T>`/`ChannelSender<T>`** (stdlib [§6.9](../Bezel_Standard_Library.md#sec-channel)); **`Domain.Spawn`** (stdlib [§6.8](../Bezel_Standard_Library.md#sec-domain)).
- **`Environment`** (stdlib [§2.4](../Bezel_Standard_Library.md#sec-environment)) — `GetEnvironmentVariables(): owned Dictionary<string, string>`, `GetEnvironmentVariable`, `CommandLineArgs`. The child-env design (Q1) is built to compose with this.
- **`Array<string>`**, **`Array<byte>`** (stdlib §4.4), **`Dictionary<string, string>`** (stdlib §4.8), **collection/dictionary literals** (spec [§15.4](../Bezel_Language_Spec.md#sec-collection-literals)).
- **Exceptions**: `IOException` and the §7.5 IO family (stdlib [§7.5](../Bezel_Standard_Library.md#sec-io-exception-types)); `InvalidOperationException`, `OperationCanceledException` (spec [§31.4](../Bezel_Language_Spec.md#sec-standard-exception-types)).
- **`owned`/`using var`, the destructor, owned-field cascade and borrow-out** (spec [§28.4](../Bezel_Language_Spec.md#sec-the-destructor)–[§28.6](../Bezel_Language_Spec.md#sec-leak-prevention)); **sendability** (spec [§30.14](../Bezel_Language_Spec.md#sec-sendable-types)); **domains/run-to-completion** (spec [§30.13](../Bezel_Language_Spec.md#sec-runtime-model)).

The types live in `System.Diagnostics` and require `import System.Diagnostics;` (like `Stopwatch`, §8). `import System.IO;` is assumed wherever `Stream` / the 0006 wrappers appear, and `import System.Threading;` wherever `CancellationToken`/`TaskScope` are named.

## 2. Tour by example

Snippets are method-level fragments; the enclosing `module`/class is elided and the imports above are assumed.

```bezel
// ── Run a command, capture stdout as text, read the exit code. ──
// The Process OWNS the child handle and the (redirected) pipes until disposal;
// a single `using var` guarantees the child does not outlive the scope (Q2).
public async method GitHeadAsync(): Task<string>
  using var p := Process.Start("git", ["rev-parse", "HEAD"], stdout := Redirect.Pipe);
  using var reader := new StreamReader(p.TakeStandardOutput());  // (0006) wraps the owned pipe
  var sha := await reader.ReadToEndAsync();
  var code := await p.WaitForExitAsync();                       // async-only wait (Q3): Task<int>
  if code <> 0 then throw new ProcessException("git failed"); end;
  return sha.Trim();
end GitHeadAsync;

// ── Feed stdin, then signal EOF so the child stops reading. ──
public async method FormatAsync(source: string): Task<string>
  using var p := Process.Start("clang-format", [],
      stdin := Redirect.Pipe, stdout := Redirect.Pipe);
  do
    await using var stdin := new StreamWriter(p.TakeStandardInput());   // (0006)
    await stdin.WriteAsync(source);
    // inner scope end disposes stdin: flush + close the write end ⇒ EOF to the child
  end do;
  using var reader := new StreamReader(p.TakeStandardOutput());
  var formatted := await reader.ReadToEndAsync();
  await p.WaitForExitAsync();
  return formatted;
end FormatAsync;

// ── Capture BOTH stdout and stderr without the full-pipe deadlock (Q4). ──
// The two pipes are drained CONCURRENTLY on this domain via a TaskScope while
// the exit is awaited; draining only one while the child fills the other would
// deadlock (child blocked on a full pipe, parent blocked on the drained one).
public async method RunCapturedAsync(exe: string, args: Array<string>): Task<int>
  using var p := Process.Start(exe, args,
      stdout := Redirect.Pipe, stderr := Redirect.Pipe);
  using var outBuf := new MemoryStream();                       // (0006)
  using var errBuf := new MemoryStream();
  using var outPipe := p.TakeStandardOutput();                  // owned pipe Streams
  using var errPipe := p.TakeStandardError();
  await using var scope := new TaskScope();                     // §6.10
  scope.Spawn(lambda() => outPipe.CopyToAsync(outBuf, token := scope.Token));   // (0006) CopyToAsync
  scope.Spawn(lambda() => errPipe.CopyToAsync(errBuf, token := scope.Token));
  var code := await p.WaitForExitAsync(scope.Token);
  await scope.Join();                                           // both drains hit EOF at child exit
  // outBuf.ToArray() / errBuf.ToArray() now hold the captured bytes.
  return code;
end RunCapturedAsync;

// ── Give the child a deadline; kill it if it overruns (Q2/Q3). ──
public async method RunWithDeadlineAsync(exe: string): Task<int>
  using var p := Process.Start(exe, []);
  using var deadline := CancellationTokenSource.WithCancelAfter(TimeSpan.FromSeconds(10));
  var code := 0;
  try
    code := await p.WaitForExitAsync(deadline.Token);
  catch
    OperationCanceledException => do
      p.Kill();                                                 // forceful; SIGKILL (Q2)
      code := await p.WaitForExitAsync();                       // now reaps the killed child
    end
  end try;
  return code;
end RunWithDeadlineAsync;

// ── A hermetic spawn: empty base environment plus explicit overrides (Q1). ──
using var p := Process.Start("./build.sh", ["--release"],
    workingDirectory := "/src/project",
    environment := EnvironmentMode.Empty,
    environmentOverrides := ["PATH": "/usr/bin", "CI": "1"]);
await p.WaitForExitAsync();
```

## 3. Design decisions

Each states the choice, the alternatives considered, and why the choice wins under Bezel's ownership, async, and domain rules.

### Q1 — Spawn shape: argv array, no shell; options as named parameters

**Decision.** One factory, taking the executable and an **argv array**, with the rest of the knobs as **named default parameters** (spec [§22.5](../Bezel_Language_Spec.md#sec-default-arguments), the `DateTime`-constructor / `CopyToAsync` precedent):

```bezel
public static method Start(
    fileName: string,
    args: Array<string>,
    workingDirectory: string? := null,                     // null ⇒ inherit parent cwd
    stdin: Redirect := Redirect.Inherit,
    stdout: Redirect := Redirect.Inherit,
    stderr: Redirect := Redirect.Inherit,
    environment: EnvironmentMode := EnvironmentMode.Inherit,
    environmentOverrides: Dictionary<string, string>? := null): owned Process;
```

- **`fileName`** is the executable. It is resolved on `PATH` when it contains no directory separator, else used as a path (the `execvp` convention) — stated so callers know `"git"` searches `PATH` and `"./build.sh"` does not.
- **`args`** are `argv[1..]`; the runtime sets `argv[0]` to `fileName`. Each element is passed to the child **verbatim** — no globbing, no word-splitting, no quoting. `Process.Start("ls", ["*.txt"])` passes the literal `*.txt`.
- **Synchronous, `owned`-returning, throwing.** `Start` is not `async`: `fork`+`exec` (or `posix_spawn`) is a fast non-blocking syscall, so a synchronous factory returning `owned Process` — the `Stopwatch.StartNew` shape (stdlib §8.1), and `FileStream`'s throwing constructor (§7.2) — is right. Spawn failure raises (Q6). The only operation that *waits* is `WaitForExitAsync` (Q3).

**Decline — a shell-string overload (`Start("rm -rf " + dir)`).** Not offered, ever. A shell string is the command-injection vector: any `fileName`/argument derived from untrusted input can inject shell metacharacters. The argv array is injection-proof by construction (arguments are not re-parsed), and this matches Go's `exec.Command(name, args...)` and Rust's `Command::new().args()`, both of which deliberately have no shell form. A caller who genuinely wants a shell writes `Process.Start("/bin/sh", ["-c", script])` explicitly and owns that decision. **This decline is recorded in [DECLINED.md](DECLINED.md)-adjacent form here and is not revisited without new evidence.**

**Decline — a `ProcessStartInfo`-style mutable options record.** .NET bundles ~15 knobs into a mutable `ProcessStartInfo`. Bezel does not, for two structural reasons:
1. **A positional record cannot carry per-field defaults** (Appendix A `PositionalField ::= Identifier ":" Type SerializedClause?` — no default). So a `record ProcessOptions(...)` would force callers to supply *every* field, or accept the coarse "pass `null` ⇒ all defaults" (0007's `TlsOptions` shape) with no way to tweak a single field. Named default parameters (§22.5) give **per-option** defaults with per-call override — strictly more ergonomic for the v1 knob set.
2. **A mutable builder fights the record grain.** Records are value types whose mutable form is "inadvisable" (spec [§9.4](../Bezel_Language_Spec.md#sec-mutability)); a stateful `ProcessStartInfo` you mutate then hand to `Start` is a heap object for a one-shot call. If the knob count later grows past what a parameter list carries comfortably, the migration is to an *immutable* record built with `with` — revisit then, not now.

**Environment — inherit by default, `Empty` for hermetic, overrides on top.**

```bezel
public enum EnvironmentMode: byte  Inherit, Empty;  end EnvironmentMode;
```

The *base* environment is the parent's (`Inherit`, default) or nothing (`Empty`); `environmentOverrides` then sets/replaces named variables on top of the base. This is a **considered refinement** of the gap-review sketch, which leaned "explicit environment dictionary rather than silent inheritance." The refinement, with its reason: the marquee consumers — build tools, test runners, `git` — almost always want the *inherited* `PATH`/`HOME`/`TERM` plus a handful of overrides; forcing every spawn to enumerate a full environment is an ergonomic tax that pushes callers toward wrong-but-easy full copies. So the default inherits (least surprise), and `Empty` preserves the hermetic option the sketch wanted — `environment := EnvironmentMode.Empty, environmentOverrides := myFullDict` is the explicit-total-environment form, and `Environment.GetEnvironmentVariables()` (stdlib §2.4) is the way to seed such a dict from the parent. Composing with §2.4's existing dictionary is why `environmentOverrides` is a `Dictionary<string, string>` and not a bespoke type. *Un-setting* an inherited variable (as distinct from overriding it) is deferred (§5); the v1 workaround is `Empty` + an explicit dict.

**`workingDirectory`** is an explicit per-spawn argument precisely because the process-global `Environment.CurrentDirectory` **setter is declined** ([gap review](stdlib-gap-review-2026-07.md) §Declined #4 — a process-global mutable cwd fights domain isolation). The child's cwd travels as data on the spawn call, never as ambient global state.

### Q2 — Ownership and lifecycle: an owned handle whose disposal *kills* a live child

**Decision.** `Process` is an **owned class** consumed with `using var`. The §28 must-decide — *what does disposal do to a still-running child?* — is answered: **disposal kills the child (SIGKILL) and reaps it.**

```bezel
using var p := Process.Start("server", []);
// … if the scope exits (normally or by exception) with the child still running,
//   the destructor SIGKILLs it and reaps the zombie. No orphan, no leak.
```

Precisely, the destructor:
1. If the child has **already exited** (whether reaped by a prior `WaitForExitAsync`/`Kill`+wait, or exited on its own), it reaps it if not already reaped (`waitpid`) and returns. No signal is sent.
2. If the child is **still running**, it sends `SIGKILL`, then reaps (`waitpid`). `SIGKILL` cannot be caught or ignored (POSIX), so the child is guaranteed to terminate; the reap of a just-killed child returns essentially immediately.

**Why kill, not detach.** Detaching (letting disposal orphan a running child) is a **silent resource leak of an OS process** — exactly the class of leak §28's dispose-exactly-once discipline exists to prevent, lifted from heap objects to process handles. An owned `Process` that could leave a runaway child behind would make `using var` a lie. Kill-on-dispose makes the child's lifetime *bounded by the `Process` value's lifetime*, which is what an owned handle means. A caller who truly wants a child to outlive the parent scope needs an explicit **detach** operation — deferred (§5), never the silent default.

**Why the reap is sound in a synchronous destructor — and is *not* the declined blocking wait.** The declined item ([gap review](stdlib-gap-review-2026-07.md) §Declined #5) is a *blocking wait for an arbitrary-duration child to exit on its own* — unbounded, and a §30-deadlock hazard. The destructor's reap is the opposite: it runs **after `SIGKILL`**, on a child that is guaranteed to be dying, so `waitpid` returns in O(1). This bounded reap is the same shape as `FileStream`'s destructor calling `NativeClose` (spec §28.4) — a fast, terminating syscall, not an open-ended await. (One honest edge: a child wedged in an uninterruptible-sleep `D` state does not die immediately even on `SIGKILL`; the reap can then briefly block. This is a rare kernel pathology with no clean userspace remedy; noted in §6.) Because disposal is a bounded synchronous kill+reap, `Process` needs **only a synchronous `destructor`** and does **not** implement `IAsyncDisposable` — a deliberate contrast with 0006's `StreamWriter`, whose *flush* can suspend and therefore needs the async close edge.

**Zombie-reaping obligation.** Every child is reaped exactly once — by `WaitForExitAsync` on the awaited path, or by the destructor otherwise. The runtime additionally must not let a child that outlives its `Process` (it cannot, given kill-on-dispose) become an unreaped zombie; the destructor is the backstop. `Dispose()` is idempotent (§28.2), so a `WaitForExitAsync`-then-scope-exit sequence reaps once and the destructor's reap is a no-op.

**Kill surface, and the relation to graceful signals (gap #14).** v1 exposes **one** terminate primitive — `Kill()` — deliberately the *forceful* one (`SIGKILL`), because it is the only signal guaranteed to terminate an uncooperative child and it is what disposal relies on. A **graceful** shutdown (`SIGTERM`, letting the child flush and exit cleanly) requires the POSIX-signal surface that is **gap-review item #14** (signal hooks); v1 does not deliver arbitrary signals. The v1 cooperative-shutdown pattern uses the child's *own* protocol — most commonly closing its stdin to signal EOF (see Q4 and the `FormatAsync` tour example) — followed by `await WaitForExitAsync(deadline)`, and `Kill()` only if the deadline lapses (the `RunWithDeadlineAsync` example). When #14 lands, a `Signal(sig)` method (or a `terminate := Signal.Term` option on disposal) composes on top of this without changing the kill-on-dispose default.

### Q3 — Exit: async-only wait, and the access-before-exit fault

**Decision.** Waiting is **async-only**, per the recorded no-blocking-waits decline:

```bezel
public async method WaitForExitAsync(token: CancellationToken := CancellationToken.None): Task<int>;
public readonly property HasExited: bool;
public readonly property ExitCode: int;     // InvalidOperationException before exit
public readonly property Id: int;           // the OS process id (pid)
```

- **`WaitForExitAsync` returns `Task<int>`** (Pattern A — `int` is a value type, so no ownership crosses the await; spec §30.5). It completes when the child exits *for any reason* and yields the **exit code**. It may be awaited from **more than one place** — every awaiter observes the same completed exit code (fan-out is the reason for `Task<int>` over `OwningTask<int>`). The first completion reaps the child; the code is cached.
- **Signal-terminated children.** A child killed by signal *N* has no exit code in the C sense; `WaitForExitAsync` reports **`128 + N`** — the shell convention, and what .NET-on-Unix's `ExitCode` returns — so the `int` return stays honest (`137` = `128 + SIGKILL(9)`). This keeps the return a plain `int`; a richer `{ code, signal }` split is deferred (§5, §6).
- **Cancellation abandons the *wait*, not the child.** If the supplied `token` fires, `WaitForExitAsync` raises `OperationCanceledException` (spec §30.8) and the child **keeps running** — cancelling a wait is not a kill. To time-out-then-terminate, catch the cancellation and call `Kill()` (the `RunWithDeadlineAsync` example). This is the one semantic a caller must internalize, and it is stated on the method.
- **`ExitCode` before exit is `InvalidOperationException`** (spec §31.4, "operation not valid in current state") — the same wrong-state fault the language already uses. `ExitCode` is a convenience for *after* you have awaited; the value returned by `WaitForExitAsync` is the primary source, and `HasExited` lets a caller check without faulting.

**Decline — a synchronous `WaitForExit()` (any blocking form).** Already declined ([gap review](stdlib-gap-review-2026-07.md) §Declined #5): a blocking wait stalls the calling domain's single-threaded run-to-completion (spec §30.13) for the child's entire lifetime — a deadlock hazard (the child might itself be waiting on the domain). The async form is the only wait offered; there is no sync dual, unlike the read/write duals elsewhere, because the *reason* to omit it (domain-blocking) is fundamental, not mechanical.

### Q4 — Stdio: pipes as 0006 streams, transferred out, drain-both-or-deadlock

**Decision — redirection model.** Each standard stream is independently one of three modes:

```bezel
public enum Redirect: byte  Inherit, Pipe, Discard;  end Redirect;
```

- **`Inherit`** (default) — the child shares the parent's corresponding fd (its output lands on the parent's terminal; its stdin reads the parent's). This is the least-surprise default for a spawned tool and matches .NET (`Redirect… = false`) and the shell.
- **`Pipe`** — a pipe is created; the parent end is exposed for reading (stdout/stderr) or writing (stdin).
- **`Discard`** — connected to the null device (`/dev/null`); output is dropped, stdin reads EOF.

**Decision — the pipes are *transferred* out, not borrowed.** The `Process` owns each redirected pipe `Stream` as an `owned` field until the caller takes it:

```bezel
public method TakeStandardInput(): owned Stream;    // stdin  == Redirect.Pipe, else InvalidOperationException
public method TakeStandardOutput(): owned Stream;   // stdout == Redirect.Pipe
public method TakeStandardError(): owned Stream;    // stderr == Redirect.Pipe
```

`Take*` **moves** the owned pipe `Stream` out of the `Process` (spec §28.8 transfer): after the take, the `Process` no longer owns or disposes that pipe — the caller does (`using var reader := p.TakeStandardOutput();`) — and the child's own copy of the fd still closes on child exit, delivering EOF to the reader. A pipe never taken is closed by `Process` disposal (the owned-field cascade, §28.4). Taking the same stream twice, or taking a non-`Pipe` stream, raises `InvalidOperationException`.

**Why transfer and not a §28.6 borrow-out.** The natural instinct (0007's `TcpConnection.GetStream()` borrows) is a **borrow accessor** returning the pipe as a borrow of a `Process`-owned field (§28.6). It loses here because **0006's text and capture wrappers demand ownership**: `new StreamReader(move stream)`, `new StreamWriter(move stream)` (0006 §3 Q1) take the stream `owned`, and `MemoryStream`-backed capture wants to own too. A §28.6 borrow-out cannot feed an `owned`-ctor wrapper — so a borrowed pipe `Stream` would support only inline `ReadAsync`/`CopyToAsync` loops and could never become a `StreamReader` for line reading, which is the dominant need. Transfer makes the whole 0006 family compose: `new StreamReader(p.TakeStandardOutput())` just works. The cost — the caller now owns the pipe and must `using var` it — is exactly the ownership legibility Bezel wants, and disposal is still total (the `Process` disposes any untaken pipe). The `TakeStdout(): owned TextReader` sketch in the gap review is this design with the wrapper pre-applied; returning the raw `owned Stream` instead is *more* general (the caller picks text via `StreamReader`, bytes via `CopyToAsync`, or in-memory via `MemoryStream`) at the cost of one `new StreamReader(...)`, and avoids baking a `TextReader`-vs-`Stream` choice into the `Process` surface.

**The deadlock hazard, and the drain pattern (load-bearing).** If stdout **and** stderr are both `Pipe` and the caller drains only one, the child blocks writing to the *other* (full) pipe while the caller blocks on the drained one — a classic deadlock, made worse under Bezel's per-domain run-to-completion (a single domain awaiting `WaitForExitAsync` while the child is wedged makes no progress). **The rule, stated in the docs and shown in the `RunCapturedAsync` tour example: any stream redirected to `Pipe` must be drained, and when more than one is piped they must be drained *concurrently*.** The idiom is a `TaskScope` (stdlib §6.10) running one `CopyToAsync` (0006) per pipe on the owning domain, alongside the awaited exit:

```bezel
scope.Spawn(lambda() => outPipe.CopyToAsync(outBuf, token := scope.Token));
scope.Spawn(lambda() => errPipe.CopyToAsync(errBuf, token := scope.Token));
var code := await p.WaitForExitAsync(scope.Token);
await scope.Join();
```

For the single-piped-stream case (only stdout), a straight `await reader.ReadToEndAsync()` then `await WaitForExitAsync()` is safe — the hazard is specifically *multiple* full pipes.

**Closing stdin to signal EOF.** Many children block reading stdin until they see EOF. Because the caller *owns* the stdin pipe after `TakeStandardInput()`, the caller closes it the ordinary way — dispose the `StreamWriter` (which flushes, 0006 §3 Q7) or the raw `Stream`. Scoping the writer in an inner `do … end` with `await using var` (the `FormatAsync` example) flushes-closes-and-surfaces-errors at scope end via 0006's `StreamWriter.DisposeAsync` (`StreamWriter` is `IAsyncDisposable`), closing the write end *before* the parent reads the child's output. No bespoke `CloseStandardInput` is needed — ownership makes the close a normal disposal.

### Q5 — Domain story: `Process` is domain-local (not sendable)

**Decision (proposed §30.14 classification).** **`Process` is domain-local and *not* sendable** (spec [§30.14](../Bezel_Language_Spec.md#sec-sendable-types)); its pipe drains complete on the domain that owns the `Process`.

This follows **0007's `TcpConnection` precedent** ([0007 §3](0007-system-net-substrate.md), the load-bearing ruling) essentially verbatim, because a `Process` is the same kind of object:

1. **It is the live-resource family §30.14 already excludes.** §30.14 lists `Task`, `Task<T>`, `OwningTask<T>`, enumerators, `EventSource<T>`, `CancellationTokenSource`, and `Weak<T>` as "domain-local and not sendable" — each a live async resource whose completion state binds to a domain. A `Process` holds a runtime-registered child handle plus pipe fds whose `ReadAsync`/`WriteAsync` completions queue to a specific domain (spec §30.13 scheduling); it belongs to that family. Classing it sendable would be inconsistent with `TcpConnection`, which it most resembles.
2. **Its stdio carries mutable in-flight state that is not internally synchronized.** A stdin `StreamWriter`'s encoder buffer and a stdout reader's read-ahead buffer are mutated by the reading/writing continuations; two domains reaching one `Process`'s streams simultaneously is the shared-mutable-state race §30.14 exists to exclude (unlike `Channel<T>`, a pipe wrapper is not internally synchronized, and making it so would tax every read to enable a pattern no one needs).
3. **It imposes the weakest runtime contract.** Domain-affinity lets the runtime bind each pipe fd's completion routing to the one owning domain — no per-operation re-homing, no proof that a moved `Process` is quiescent. Per CLAUDE.md's "simplicity beats speed until the compiler is complete," the affine ruling is the weaker, safer contract.

**Which domain runs the drains.** The `CopyToAsync`/`ReadAsync` continuations that drain the pipes run on the **domain that issued them** — the owning domain of the `Process` (spec §30.13: completions target the issuing domain). The `TaskScope` in `RunCapturedAsync` is that domain's scope; the drains are intra-domain concurrency (interleaved at `await` points), not cross-domain parallelism.

**Fanning process work across domains — results travel, the `Process` stays put.** The one place child work "wants" to cross domains is a pool of workers each running children. That composes with **zero `Process` handoff**, exactly as 0007's connections do: a worker domain (spawned via `Domain.Spawn`, stdlib §6.8) creates and drives its *own* `Process`es locally and sends **sendable results** — the exit-code `int`, captured `string`s, or `owned Array<byte>` — back over a `Channel<T>` (stdlib §6.9). The live `Process` never moves; only bytes and codes do. This is the same "live resource stays, sendable data travels" shape 0007 §3 establishes, and it needs nothing new.

**These classifications are proposed additions to §30.14's compiler-known list** (which already enumerates the stdlib live-resource types case by case); they graduate into the spec when `Process` lands. Marked proposed, not assumed.

### Q6 — Errors: a typed spawn-failure taxonomy rooted under `IOException`

**Decision.** Spawn and process faults are typed, catchable, and rooted under **`IOException`** (stdlib §7.5) so that `catch IOException` is the honest "any I/O — file, socket, or process — failed" umbrella (the same rooting 0007 §7 chose for sockets), while the two conditions a caller actually distinguishes at its boundary get named subtypes. This is **band 4 of the failure-signal rule** (stdlib [§1](Bezel_Standard_Library.md#sec-failure-signal-rule), v0.229): an *expected* environmental condition (executable missing, permission denied) consumed at a boundary, supervision, or retry site is a distinct catchable type, producer-local per spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types), not a string to match. The `TryStart` form deferred in §5 is the rule's band-2 tier waiting on evidence — a different call-site stance, not a second mechanism for this condition (0076 §5's table and R6; graduation-checklist obligation [#2338](https://github.com/s8lap/bezel-lang/issues/2338)).

```
Exception (spec §31.4)
└─ IOException (stdlib §7.5)
   └─ ProcessException                    — base for any child-process fault
       └─ ProcessStartException           — spawn failed; carries Reason: ProcessStartError
           ├─ ExecutableNotFoundException — the executable was not found (ENOENT / not on PATH)
           └─ ExecutableAccessException   — found but not runnable (EACCES / ENOEXEC)

  reused as-is:
   InvalidOperationException  (spec §31.4)  — ExitCode/Take* accessed in the wrong state
   OperationCanceledException (spec §31.4)  — a supplied wait token fired (child NOT killed, Q3)
   IOException                (stdlib §7.5) — a pipe I/O failure (e.g. write to a closed child ⇒ broken pipe)
```

```bezel
public class ProcessException: IOException                     // requires import System.Diagnostics
  public constructor(message: string);
end ProcessException;

public class ProcessStartException: ProcessException
  public readonly property Reason: ProcessStartError;
end ProcessStartException;

public enum ProcessStartError: byte  NotFound, AccessDenied, Other;  end ProcessStartError;

public class ExecutableNotFoundException: ProcessStartException  end ExecutableNotFoundException;
public class ExecutableAccessException:   ProcessStartException  end ExecutableAccessException;
```

- **Spawn failure taxonomy.** `Start` raises `ExecutableNotFoundException` when `fileName` resolves to nothing (`ENOENT`, or not found on `PATH`), and `ExecutableAccessException` when the target exists but cannot be executed (`EACCES`, `ENOEXEC` — a non-executable or a bad-format file). Any other spawn failure is `ProcessStartException` with `Reason := ProcessStartError.Other` (the `Error`-code long tail, as 0007's `SocketException.Error` carries the residue). The inheritance is arm-ordering-relevant (spec §31.4): `catch ProcessStartException` matches both named subtypes; `catch IOException` matches everything here plus pipe faults.
- **Wrong-state faults** reuse `InvalidOperationException`: reading `ExitCode` before the child exits, or `Take*`-ing a non-piped or already-taken stream.
- **A non-zero exit is not an exception.** A child that runs and exits with a non-zero code is a *value*, not a fault — `WaitForExitAsync` returns the code; the caller decides whether non-zero is an error (the `GitHeadAsync` example throws its own). This is the EOF-is-a-value / decode-failure-is-a-fault line 0006 §3 Q5 and Guide 2 draw, applied to process exit.
- **Pipe I/O failures** surface as ordinary `IOException` from the `Stream` layer — writing to a child that has closed its stdin is a broken pipe, an `IOException` at the `await` (spec §30.9).
- **Cross-domain faults.** A `Process` fault raised inside a spawned worker domain surfaces at its join as `DomainFaultedException` carrying copied message/stack strings (spec §30.14) — process exceptions are not sendable and never cross as objects, exactly like every other exception.

## 4. Proposed API surface

All types are **new** in `System.Diagnostics`; everything cited to the stdlib/0006 is marked. `import System.Diagnostics;` (plus `System.IO` for `Stream`/0006, `System.Threading` for `CancellationToken`/`TaskScope`).

```bezel
public class Process                                            // owned; DOMAIN-LOCAL, not sendable (Q5)
  // ── Spawn (Q1) ──
  public static method Start(
      fileName: string,
      args: Array<string>,
      workingDirectory: string? := null,                        // null ⇒ inherit parent cwd
      stdin: Redirect := Redirect.Inherit,
      stdout: Redirect := Redirect.Inherit,
      stderr: Redirect := Redirect.Inherit,
      environment: EnvironmentMode := EnvironmentMode.Inherit,
      environmentOverrides: Dictionary<string, string>? := null): owned Process;

  // ── Identity / state (Q3) ──
  public readonly property Id: int;                             // OS process id (pid)
  public readonly property HasExited: bool;
  public readonly property ExitCode: int;                       // InvalidOperationException before exit

  // ── Wait / terminate (Q2, Q3) ──
  public async method WaitForExitAsync(token: CancellationToken := CancellationToken.None): Task<int>;
  public method Kill();                                         // SIGKILL; forceful, uncatchable by child

  // ── Stdio: transfer the redirected pipe out (Q4). Each is present only when that
  //    stream was Redirect.Pipe; otherwise (or if already taken) InvalidOperationException.
  public method TakeStandardInput():  owned Stream;             // wrap in new StreamWriter(...) (0006)
  public method TakeStandardOutput(): owned Stream;             // wrap in new StreamReader(...)  (0006)
  public method TakeStandardError():  owned Stream;

  // Owns the child handle + any untaken redirected pipes as owned fields; the
  // compiler-generated Dispose() cascade (§28.4/§28.5) closes untaken pipes, and
  // the destructor (below) kills+reaps a still-running child (Q2).
  destructor;                                                   // if still running: SIGKILL then reap;
                                                                //   else reap-if-needed. Bounded, sync.
end Process;

public enum Redirect: byte          Inherit, Pipe, Discard;  end Redirect;
public enum EnvironmentMode: byte   Inherit, Empty;         end EnvironmentMode;

// ── Error taxonomy (Q6) ──
public class ProcessException: IOException
  public constructor(message: string);
end ProcessException;
public class ProcessStartException: ProcessException
  public readonly property Reason: ProcessStartError;
end ProcessStartException;
public enum ProcessStartError: byte  NotFound, AccessDenied, Other;  end ProcessStartError;
public class ExecutableNotFoundException: ProcessStartException  end ExecutableNotFoundException;
public class ExecutableAccessException:   ProcessStartException  end ExecutableAccessException;
```

Notes on the surface:
- **Only a synchronous `destructor`, no `IAsyncDisposable`** — disposal is a bounded kill+reap (Q2), so unlike 0006's `StreamWriter` there is no suspending close edge.
- **`WaitForExitAsync` is `Task<int>`, not `OwningTask<int>`** — `int` is a value type (fan-out, no ownership; Q3).
- **`Kill()` is the only terminate primitive in v1** — graceful `SIGTERM` rides gap-review #14 (Q2).
- The stdio surface is three `Take*` methods returning `owned Stream` — the caller composes 0006's `StreamReader`/`StreamWriter`/`MemoryStream`/`CopyToAsync` over them (Q4).

## 5. Deferred (v2+, in rough priority order)

Each with its one-line reason; none blocks v1's build-tool / test-runner acceptance test.

- **Graceful termination (`SIGTERM` / arbitrary signals)** — rides gap-review **#14** (signal hooks); v1 has only forceful `Kill()` and the close-stdin cooperative pattern (Q2).
- **Detach (let a child outlive its `Process`)** — the deliberate opposite of kill-on-dispose; needs its own explicit `Detach()` that severs the owned handle, and a design for how the detached child is ever reaped. Never a silent default (Q2).
- **A `{ code, signal }` exit result** — v1 folds signal-termination into `128 + N` (Q3); a structured `ProcessExit` record (with `WasSignalled: bool`, `Signal: int?`) is a small later addition if the encoding proves lossy in practice.
- **Un-setting inherited environment variables** — v1 overrides set/replace; removing an inherited var is `Empty` + explicit dict for now (Q1). A `Dictionary<string, string?>` (null ⇒ unset) overlay is the natural v2 shape.
- **Process groups / session leadership / `setsid`** — killing a whole tree (a child's grandchildren) is a process-group operation; v1 kills only the direct child. Its own small proposal.
- **Pseudo-terminal (pty) allocation** — interactive children that need a tty (line editors, `ssh`); a distinct fd/ioctl story, out of the pipe-based v1.
- **Shell execution helper** — a `Process.Shell(script)` convenience is deliberately absent (Q1); if ever added it is an explicit, clearly-named opt-in, not a `Start` overload.
- **Non-blocking `TryStart` / spawn without throwing** — v1 throws the Q6 taxonomy; a `bool`+`out` Try form (spec §1 Try convention) is addable if a probe-many-executables use case appears.
- **`Process.GetCurrent()` / self-inspection, `Process.GetProcessById`** — inspecting *other* processes is a separate (privilege-sensitive) surface; v1 spawns and manages *children* only.

## 6. Open questions for the owner

1. **Disposal kills a running child (the load-bearing one, Q2).** Kill-on-dispose is chosen over detach-on-dispose so an owned `Process` cannot leak a runaway child. Confirm — the alternative (detach, à la .NET's `Process` which leaves the child running) is a genuine choice, and the one that would make `using var Process` unsound as a leak-prevention tool. The chosen semantics is the Rust `std::process::Child`-*without*-the-footgun (Rust drops the handle and leaves the child running, which its own docs flag as a hazard).
2. **`Task<int>` exit encoding for signals (Q3).** `128 + N` keeps the return a plain `int` and matches .NET-on-Unix. Confirm this over a structured `{ code, signal }` result now (§5 defers the record). Windows has no signals, so the encoding is Unix-only surface the runtime maps.
3. **`Take*(): owned Stream` vs pre-wrapped `TakeStandardOutput(): owned StreamReader` (Q4).** Raw `owned Stream` is chosen (caller picks text/bytes/memory via 0006); the gap-review sketch pre-wraps to `owned TextReader`. Confirm the raw-stream form — it costs the caller one `new StreamReader(...)` but keeps the `Process` surface free of a text-vs-bytes commitment.
4. **`Process` not sendable — proposed §30.14 addition (Q5).** Confirm the domain-affine classification (mirroring 0007's `TcpConnection`) over making `Process` sendable with runtime completion re-homing. Argued safer and weaker-on-the-runtime, but it is the load-bearing domain ruling.
5. **Module placement.** Types are placed in `System.Diagnostics` (with `Stopwatch`), the qualified type being `System.Diagnostics.Process` as stdlib §11 names it. Confirm this over a dedicated `System.Diagnostics.Process` submodule.
6. **The uninterruptible-sleep (`D`-state) reap edge (Q2).** A child wedged in `D` state does not die on `SIGKILL`, so the destructor's reap can briefly block. This is an unavoidable kernel pathology; confirm it is acceptable to document as a known limitation (the alternative — a fully non-blocking destructor that abandons the reap — would leak a zombie, which is worse).
7. **`fileName` `PATH` resolution.** `execvp` semantics (search `PATH` when no separator) are chosen for ergonomics (`Start("git", …)`). Confirm, versus requiring an explicit path always (more predictable, less convenient) — .NET and Go both do the `PATH` search.

## Rulings recorded at acceptance (2026-07-07, delegated ruling — owner review pending)

The seven §6 owner questions are resolved as follows; the §30.14 `Process`-not-sendable classification graduates into the compiler-known list when the type lands.

- **Q1 — disposal kills a running child (the load-bearing one): YES, kill-on-dispose (SIGKILL + reap).** Detach-on-dispose is declined: letting disposal orphan a running child is a silent OS-process leak, which would make `using var Process` a lie and defeat the very leak-prevention discipline §28 exists for. Kill-on-dispose bounds the child's lifetime by the `Process` value's, which is what an owned handle means; and the destructor's reap is bounded (it runs *after* `SIGKILL`, on a guaranteed-dying child, so `waitpid` returns in O(1)) — it is the opposite of the declined unbounded blocking wait. Detach becomes an explicit, never-silent `Detach()` later (§5).
- **Q2 — signal exit encoding `128 + N` on `Task<int>`: confirmed.** Keeps the return a plain `int`, matches .NET-on-Unix; a structured `{ code, signal }` record is deferred (§5).
- **Q3 — `Take*(): owned Stream` (raw), not pre-wrapped `TakeStandardOutput(): owned StreamReader`: confirmed.** The raw owned stream lets the caller pick text (`StreamReader`), bytes (`CopyToAsync`), or memory (`MemoryStream`) via the 0006 family, at the cost of one `new StreamReader(...)`; it keeps the `Process` surface free of a text-vs-bytes commitment. (Transfer, not §28.6 borrow-out, because 0006's wrappers take the stream `owned`.)
- **Q4 — `Process` is domain-local, not sendable: confirmed** (proposed §30.14 addition), mirroring 0007's `TcpConnection` precedent. The domain-affine ruling is the weaker, safer runtime contract; cross-domain process work uses worker domains that own their *own* `Process`es and send sendable results (exit codes, strings, `owned Array<byte>`) over channels.
- **Q5 — module placement `System.Diagnostics`: confirmed** (with `Stopwatch`; qualified type `System.Diagnostics.Process` as stdlib §11 names it), not a dedicated submodule.
- **Q6 — the uninterruptible-sleep (`D`-state) reap edge: documented as a known limitation.** A child wedged in `D` state does not die immediately even on `SIGKILL`, so the destructor's reap can briefly block; this is an unavoidable kernel pathology with no clean userspace remedy, and documenting it beats the alternative (a non-blocking destructor that abandons the reap and leaks a zombie).
- **Q7 — `execvp` `PATH` resolution: confirmed.** `fileName` with no directory separator searches `PATH` (`"git"`); with a separator it is used as a path (`"./build.sh"`) — the ergonomic choice .NET and Go both make.

## Delegated re-validation 2026-08-11 (stdlib-wave readiness; owner review in repo)

The planning papers (master-plan §4b; the roadmap) still carry **kill-on-dispose (Q1)** as this proposal's load-bearing open gate. Under the owner's 2026-08-11 delegation ("work autonomously... don't ask me for confirmations or rulings"; review-in-repo) that gate is now **closed**: the seven 2026-07-07 rulings above were re-validated against the moved docs (v0.238–v0.247) and **stand unchanged — kill-on-dispose is CONFIRMED FINAL**. The premise check, so a reviewer can reconstruct it:

- **v0.238 (silent tracking, proposal 0077)** touched the *trivial-dispose ceremony* only and reaffirmed that no user class gains a runtime lifetime mechanism; `Process` declares a destructor (non-trivial by definition), so the kill+reap disposal semantics and the §28.4 bounded-destructor framing are untouched.
- **v0.243 (Exception-derived throwables, proposal 0080)** requires every throw operand to derive from `Exception`; the §6 taxonomy roots under `IOException` ⊂ `Exception` and needs no change.
- **v0.246 (transfer-marker unification, proposal 0090)** respelled the caller-side markers in this document (2026-08-11 currency sweep); declaration positions (`Take*(): owned Stream`, `Start(...): owned Process`) keep `owned` per the declaration/action split. No consume position in this design is among v0.246's changed forms beyond the already-applied respells.
- The alternatives table is unmoved: detach-on-dispose still orphans a live OS process (the §28 leak class), and Rust's drop-and-leave-running remains the recorded counter-example, not a precedent.

Consequence: the 0011 implementation lane proceeds on the recorded surface with no open design question; the master-plan §4b row for 0011 should read *discharged* (README/planning-paper updates are the coordinator's).

## 7. Relationship to existing work

- **Gap review item #5 / [#1403](https://github.com/s8lap/bezel-lang/issues/1403)** — this is the "child process spawning" item, wave **D2**; [#1440](https://github.com/s8lap/bezel-lang/issues/1440) tracks the proposal. The item's own brief (owned class, destructor kills, stdio as owned streams with a `Take` handoff, async-only wait, explicit environment) is realized here, with the environment default refined (inherit-with-overrides) and the reasons recorded (Q1).
- **Proposal [0006](0006-stream-family.md) (concrete stream family, [#1433](https://github.com/s8lap/bezel-lang/issues/1433))** — the hard dependency. `StreamReader`/`StreamWriter`/`MemoryStream`/`Stream.CopyToAsync` are the stdio wrappers; the `owned`-ctor decision in 0006 §3 Q1 is precisely what makes `Take*(): owned Stream` (Q4) the right shape rather than a §28.6 borrow-out.
- **Proposal [0007](0007-system-net-substrate.md) (System.Net substrate, [#1436](https://github.com/s8lap/bezel-lang/issues/1436))** — the domain-affinity precedent (Q5) is 0007 §3's `TcpConnection` ruling applied to a process handle; the `IOException`-rooted typed taxonomy (Q6) mirrors 0007 §7's socket taxonomy; both are "live resource stays domain-local, sendable data travels over channels."
- **Gap-review declines** — the **blocking-wait** decline (§Declined #5) is honored by the async-only `WaitForExitAsync` (Q3); the **`Environment.CurrentDirectory` setter** decline (§Declined #4) is why `workingDirectory` is a per-spawn argument (Q1).
- **Gap-review item #14 (signal hooks)** — the graceful-termination surface `Kill()` deliberately omits (Q2, §5); v1's cooperative shutdown is close-stdin + timed wait.
- **Gap-review item #12 (Environment surface)** — `Environment.GetEnvironmentVariables()` (stdlib §2.4) is how a caller seeds an explicit child environment (Q1); the two items compose.
- **spec §30.13/§30.14 (runtime model, sendability)** — the domain rules Q5 builds on; the `Process`-not-sendable classification is a proposed addition to §30.14's compiler-known list, graduating with the type.
- **spec §28.4–§28.6 (destructor, owned fields, borrow-out)** — the ownership machinery Q2 (kill+reap destructor, owned-pipe cascade) and Q4 (transfer-out) rest on.
