# 0045 - Deterministic record/replay and differential verification

**Status:** graduated-v0.204 — normative deltas graduated v0.204 (spec §30.13 *Determinism*, §8.3 assigned identity hashes per Q1, stdlib §1 intercept points, §4.9 determinism note; impl: #2055). The record/replay runtime — journal, Modes W/D, audit and differential verbs — remains this proposal's open tooling lane (owner-gated).
**Currency note:** 2026-09-17 — stdlib §2.4's `Environment` members were renamed at v0.231 (`GetEnvironmentVariable`→`Get`, `SetEnvironmentVariable`→`Set`, `GetEnvironmentVariables`→`Variables`); this superseded proposal keeps its pinned pre-v0.231 names per the v0.231 changelog's directive (a currency note at the next revalidation, not a retro-edit). A graduated proposal owes no further revalidation.
**Tracking issue:** [#2032](https://github.com/s8lap/bezel-lang/issues/2032)
**Target:** Almost entirely **runtime + tooling** (a journaling/replaying runtime mode and driver verbs on proposal [0040](0040-build-system.md)'s `bezel` surface). Docs delta is deliberately minimal: **one proposed normative paragraph** in spec [§30.13](../Bezel_Language_Spec.md#sec-runtime-model) (the determinism statement, §3.2 below), **one clarification sentence** in stdlib §4.9/§4.10 (owning-hash-container order is deterministic given history, §3.7), **one stdlib conventions note** naming the intercept points (§3.8), and — the one representation ruling — the identity-hash pinning in spec [§8.3](../Bezel_Language_Spec.md#sec-equals-and-gethashcode) (§3.7). No grammar change, no new user-facing syntax, no new stdlib types.
**Written against:** spec v0.197 (stdlib/rationale at the same revision)

This proposal expands item **P5** of [`future-directions-2026-07-18.md`](future-directions-2026-07-18.md) to graduation depth. Where it sharpens or diverges from the P5 sketch, the divergence is recorded explicitly in §9.

## 0. Summary

Bezel's runtime model makes a claim no mainstream language can make: **all nondeterminism enters a domain through an enumerable set of edges.** Serial-per-domain execution ([§30.13](../Bezel_Language_Spec.md#sec-runtime-model)) plus the sendability invariant ([§30.14](../Bezel_Language_Spec.md#sec-multiple-domains) — "no mutable state is ever reachable from two domains", and §30.13's own closing: *"Beyond this enumeration there is nothing left to specify, by construction"*) means everything a domain observes that its own code did not compute arrives through a scheduling decision, a compiler-known synchronized object, a runtime-mediated world read, or the FFI. This proposal turns that structural fact into a tool: a **journal** of those edges, recorded live, that makes an execution exactly reproducible — and, at a domain seam, makes "behaves like yesterday" a *sound* differential oracle rather than a heuristic one.

Two modes with deliberately different guarantees:

- **Mode W (whole-program replay).** Every domain re-executes its own code; the journal records only *decisions and world inputs* — which continuation ran next, which sender's item a receive delivered, what the clock said, what an extern call returned. Payloads that crossed between domains are **recomputed** by the re-executing sender, so nothing is serialized. Serves: flaky-test reproduction, postmortems, and the project's own gate.
- **Mode D (domain-isolated replay).** One domain replays alone against a journal that additionally captures **full boundary payloads**, serialized with the stdlib [§2.14](../Bezel_Standard_Library.md#sec-serialization-vocabulary) vocabulary. Serves: differential verification of a regenerated or modified module — run the old build and the new build over the same Mode-D journal and compare edge-for-edge. This is the dynamic half of the regeneration oracle whose static half is P4 (behavioral artifact interface, proposal 0044 — drafted in parallel, not yet landed).

The spec's job here is small and mostly *audit*: state the determinism property §30.13 already implies (§3.2), and close the two places where the current documents leave a domain's execution dependent on something outside the edge set — identity-based class hashing (spec §8.3, today implicitly address-shaped) and the owning hash containers' iteration order (stdlib §4.9/§4.10) (§3.7). Everything else is runtime and driver work, staged so the first stage pays for itself inside this project's own test gate (§8).

## 1. Problem and evidence

All citations verified against v0.197.

### 1.1 Differential testing is the workhorse; today its oracle is unsound

Most real software's contract is "behaves like yesterday, plus this one intended change." Verifying a regenerated module against that contract by proof is not the near-term path; verifying it by *re-running recorded traffic and comparing* is — but in every mainstream runtime that comparison is heuristic, because a re-run is not the same run: thread interleavings, hash-order iteration, clock reads, and connection timing all shift. Differential harnesses in those ecosystems live on sameness *statistics* and flaky-diff triage.

Bezel is structurally different, and the difference is already normative:

- Within a domain, continuations are serial and run-to-completion; the interleaving points are exactly the suspension points ([§30.13](../Bezel_Language_Spec.md#sec-runtime-model)).
- The scheduler's freedom is stated in one sentence: "The domain runs queued continuations one at a time, **in unspecified order**" (§30.13, *Scheduling*). That sentence is the choice point — pin the choices and a domain's step sequence is fixed.
- Across domains, §30.14's discipline plus §30.13's closing enumeration ("no residual 'racy access' category") means cross-domain influence flows *only* through the compiler-known synchronized objects — channels ([§30.15](../Bezel_Language_Spec.md#sec-channels): interleaving from multiple senders is "an unspecified but consistent total order"), the [§30.12](../Bezel_Language_Spec.md#sec-thread-synchronization) primitives, admitted statics ([§22.12](../Bezel_Language_Spec.md#sec-static-members)), task settlement, and spawn/join.
- What is not scheduling is a **runtime-mediated world read** (clock, entropy, environment, console, files — stdlib §2.1/§2.4/§2.5/§2.6/§2.7/§7/§8.1) or the **FFI** ([§32](../Bezel_Language_Spec.md#sec-foreign-function-interface)), whose importation is already the language's audit marker ([§2.3](../Bezel_Language_Spec.md#sec-always-available-names): "`import System.Interop;` ... is the audit marker that a file performs foreign-memory access").

No mainstream language has this enumeration. Record the edges and replay is *exact*; nobody owns this niche.

### 1.2 The same-week internal payoff

The project's own gate fights contention flakes: the run logs record whole-suite timeouts under fork/link contention, "suspect" tests that pass in isolation, and a standing rule that a `--full` gate needs a quiet box. Every one of those investigations is a manual attempt to reproduce a scheduling accident. A recorded suite run makes the accident a file: re-run the failing test's journal and the interleaving that failed is the interleaving that runs. Stage S0 (§8) targets exactly this.

### 1.3 The nondeterminism audit (current state, verified)

The complete inventory of what, today, can differ between two runs of the same binary on the same inputs:

1. **Scheduling.** Ready-queue pop order (§30.13 *Scheduling*: "unspecified order"); multi-sender channel interleaving (§30.15 "Ordering"); grant order of `Mutex`/`Semaphore`/`Locked<T>`/`RwLocked<T>` and the outcomes of their `Try*` forms (stdlib §6.2/§6.3/§6.11); every `Atomic<T>`/`AtomicInteger<T>` result (stdlib §6.4); which domain wins a static-init first touch (§22.12 rule 1: the loser "blocks until initialization completes"; "the runtime records the single global initialization order"); which `Shared<T>` handle-drop is last ([§28.13](../Bezel_Language_Spec.md#sec-shared-ownership)); timer-driven completions (`Task.Delay`, stdlib §6.7; `CancelAfter`, stdlib §6.1) relative to other work; cross-domain settlement arrival ([§30.5](../Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)/[§30.1](../Bezel_Language_Spec.md#sec-async-methods) fix what settlement *means* and who owns results; *when* a foreign completion lands relative to a domain's steps is scheduling); bounded-channel `Send` full-or-not and `SendAsync` capacity grants (§30.15).
2. **World inputs.** `DateTime.Now`/`UtcNow`/`Today` (stdlib [§2.5](../Bezel_Standard_Library.md#sec-datetime-timespan-dateonly-timeonly): "computed at each access"); `Stopwatch` elapsed reads (stdlib [§8.1](../Bezel_Standard_Library.md#sec-stopwatch)); `Random`'s default constructor ("a default-constructed `Random` uses a system-time-based seed", stdlib [§2.7](../Bezel_Standard_Library.md#sec-random)); `Guid.NewGuid()` (stdlib §2.6); `Environment` reads incl. `ProcessorCount` — which feeds `Parallel.DefaultDegree` (stdlib §6.12) and hence chunking — and env vars (stdlib [§2.4](../Bezel_Standard_Library.md#sec-environment)); `args` ([§33.4](../Bezel_Language_Spec.md#sec-command-line-arguments)); console input (`Console.ReadLine`/`ReadKey`, stdlib §2.1); the entire file system surface (stdlib §7).
3. **FFI.** Extern call results, boundary `out`/`ref` writebacks ([§32.5](../Bezel_Language_Spec.md#sec-ffi-compatible-types)), native writes into passed buffers, callbacks ([§32.8](../Bezel_Language_Spec.md#sec-callbacks)), foreign threads entering through the sanctioned §32.10 surface, and foreign memory read through `Ptr<T>` ([§32.12](../Bezel_Language_Spec.md#sec-foreign-memory)).
4. **Hash-derived order and values.** This is the audit item the P5 sketch ordered, with a sharper result than the sketch anticipated:
   - `Dictionary<K, V>` iteration is **already pinned to insertion order** (stdlib [§4.8](../Bezel_Standard_Library.md#sec-dictionaryk-v): "in insertion order. Removing a key and re-adding it places it at the end ... `Clear()` resets the order"), and `Set<T>` likewise (stdlib [§4.10](../Bezel_Standard_Library.md#sec-sett-and-ownedsett): "insertion order ... the same guarantee as `Dictionary<K, V>`"). No work needed; report only.
   - `OwningDictionary<K, V>` makes **no order promise** (stdlib §4.9: order "depends on insertion-and-mutation history rather than purely on insertion order. The standard library makes no insertion-order guarantee"), and `OwningSet<T>` follows it (§4.10). Crucially, the stated *reason* (the drain-and-rebuild representation) makes the order a pure function of (operation history, key hash values) — unspecified is not random. §3.7 pins exactly that.
   - Spec §8.3 makes a class's default `GetHashCode` "**identity-based**" without saying what identity hashes *are*. An address-derived implementation — the obvious one — is per-run nondeterministic under ASLR, which leaks into owning-container bucket order and into any program that reads a hash code. §3.7 resolves this.
   - Proposal [0008](0008-integer-toolkit.md) (accepted, not landed) adds `HashCode` with a **per-process randomized seed** — an owner-confirmed decision ("determinism belongs to checksums"), which §8.3 permits because it promises no cross-process stability. Unjournaled, that seed would make every auto-generated record hash per-run random. §3.7 journals it; the randomization stays.
5. **Static initialization — confirmed already deterministic (the P5-ordered check).** Within a type, textual order ([§22.13](../Bezel_Language_Spec.md#sec-static-initialization-order)); across types, lazily on first use — which is a function of execution order, so it is deterministic once the schedule is; the exit-disposal pass runs on a single shutdown thread in reverse recorded-initialization order with a defined drain rule (§22.13). The **only** nondeterministic ingredient is the cross-domain first-touch election of §22.12 rule 1, which is a scheduling edge (row A7 of §3.3). No new pinning is needed; §22.13 needs no amendment.

A worked instance of category 1, for concreteness (every API per stdlib §6.8/§6.9; the shape is [§30.15](../Bezel_Language_Spec.md#sec-channels)'s own example):

```bezel
public static class Repro
  public static method WorkerA(owned tx: ChannelSender<string>)
    using var sender := owned tx;
    sender.Send("A");
  end WorkerA;

  public static method WorkerB(owned tx: ChannelSender<string>)
    using var sender := owned tx;
    sender.Send("B");
  end WorkerB;

  public static async method Main(): Task<int>
    using var inbox := new Channel<string>();
    using var a := Domain.Spawn(WorkerA, inbox.NewSender());
    using var b := Domain.Spawn(WorkerB, inbox.NewSender());
    Console.WriteLine(await inbox.Receive());   // "A" or "B": the §30.15
    Console.WriteLine(await inbox.Receive());   //   multi-sender interleave
    await a;
    await b;
    return 0;
  end Main;
end Repro;
```

Both outputs are spec-conforming; a test asserting one of them is flaky. Under record, the journal pins which sender's item each `Receive` delivered; under replay, the same program prints the same two lines, every time.

### 1.4 The journal of one `Repro` run, worked (Mode W)

To fix intuitions about granularity and size before the design sections. Domains number hierarchically (§3.1): 0 = main, 1 = the `WorkerA` domain, 2 = `WorkerB`. One possible recorded run, "B" first — frames abridged to their information content:

```
header:  format v1, runtime rev, build id, mode W, hash-seed, argv=[], env snapshot, ...
domain 0: A1 dispatch main#0            | A2 recv inbox#1 -> (sender 2, seq 1)
          C1 console "B\n" (bytes, g#1)  | A1 dispatch main#1
          A2 recv inbox#1 -> (sender 1, seq 1)
          C1 console "A\n" (bytes, g#2)  | A5 join d1 settled: ok | A5 join d2 settled: ok
          C3 exit 0
domain 1: A1 dispatch entry#0           | (Send stamps (1, seq 1); send frames none —
                                        |    unbounded Send has no outcome to record)
domain 2: A1 dispatch entry#0
```

Roughly a dozen frames, tens of bytes. No payload appears anywhere except the two tiny `C1` output verifications — "A" and "B" are recomputed by the re-executed workers. Replay delivers `(2, seq 1)` to the first receive regardless of which worker's send happens to land first on the replay's wall clock, waiting if necessary (§3.4); the program prints "B" then "A", every time, and a program whose re-executed code produces anything else stops with a §3.10 report at the first disagreeing frame. The same run recorded in Mode D for domain 0 would add the two `string` payload bodies to the A2 frames and drop domains 1/2 from the file entirely.

## 2. The two modes and their guarantees

| | **Mode W — whole-program replay** | **Mode D — domain-isolated replay** |
|---|---|---|
| What re-executes | Every domain (all Bezel code re-runs) | Exactly one domain (or one spawn-rooted subtree) |
| Journal contents | Scheduling decisions + world inputs + FFI results (§3.3). **No cross-domain payloads** — the re-executing sender recomputes them | Everything Mode W journals *for that domain*, **plus every boundary payload**, serialized (§3.6) |
| Serialization requirement | None | Boundary values must be journal-serializable (§3.6: sendable **and** serializable, or virtualizable) |
| Binary constraint | Same executable artifact, byte-identical (build id in the header) | Artifacts may differ — that is the point; same runtime revision and journal format |
| Guarantee | Edge-equivalent re-execution or a first-divergence report (§3.2) | The replayed domain sees the recorded boundary exactly; its *emitted* edges are compared against the recording |
| Serves | Flaky-test reproduction, postmortem debugging, gate determinism | Differential verification of a regenerated/modified module; long-lived regression corpora |
| Cost profile | Cheapest journals (decisions are bytes; payloads recomputed) | Journals proportional to boundary traffic |

The split is load-bearing. Mode W's no-serialization property is what makes it deployable on the whole test suite *now* — no type needs any conformance, because every payload is recomputed by re-executed code. Mode D pays serialization to buy independence: the counterpart domains do not run at all, so the same journal can drive two *different* builds of the replayed module. A program need not choose: recording Mode W and Mode D streams simultaneously for a named domain is the differential-capture configuration (§3.9).

## 3. The design

### 3.1 Architecture: intercepts, streams, and the replay scheduler

Recording and replay live in the runtime, behind a process-level mode (off / record / replay) fixed before `Main` and never changed mid-run. Every edge in the taxonomy (§3.3) is **already a runtime-mediated operation** — a scheduler dispatch, a synchronized-object call, a stdlib world call, an extern trampoline — so interception is a branch on the mode inside code the runtime already owns. **No codegen change in v1**: computation compiles exactly as today; only runtime entry points check the mode. (The declined codegen-instrumented alternative is §10.2.)

Each domain owns one **journal stream**, written by the domain's own executing thread — single-writer by construction, so recording adds no cross-domain synchronization beyond the id tags below. Frames within a stream are ordered by the domain's **step counter** (one tick per dispatched continuation, sub-ticked per edge within a continuation). Cross-domain facts are recorded in the *observing* domain's stream, keyed by stable ids: domains are numbered hierarchically in per-parent spawn order (deterministic once the parent's schedule is), channels and senders in per-domain creation order, and each channel item carries a `(sender id, per-sender sequence)` tag stamped at `Send` — well-defined because per-sender FIFO is already normative (§30.15 "Ordering").

Replay inverts each intercept: the scheduler dispatches the continuation the journal names (waiting, if it is not yet ready, for the producing domain's own replay to make it ready — which the recorded run proves will happen); world reads return journaled values without consulting the world; synchronized-object operations complete with journaled grants and results; extern calls substitute (§3.5). A replay that reaches an edge whose journal frame does not match — or that cannot make the journaled choice available within a watchdog bound — stops with a first-divergence report (§3.10).

The **startup thread** and the **shutdown thread** — the code the model runs outside any domain (§30.13 v0.179; §22.13 v0.193) — record into two dedicated streams with the same frame vocabulary: their world reads are ordinary B frames, their blocking waits at the process edge are A5 frames, and the shutdown pass needs nothing beyond A7's recorded initialization order (its disposal sequence is derived, §1.3.5). No special-case machinery: "outside any domain" is, for the journal, simply two more serial streams.

### 3.2 The proposed normative paragraph (spec §30.13)

To be appended to §30.13 after the *Scheduling* subsection, as a named paragraph:

> **Determinism.** A domain's execution is a deterministic function of what its edges deliver. Precisely: fix one executable artifact and its runtime. An **edge** of a domain is one of: a scheduling decision of this section (which queued continuation is dispatched next; which item a channel receive delivers ([§30.15](#sec-channels)); a grant, completion, or observed result of an operation on a compiler-known internally-synchronized object (the [§30.12](#sec-thread-synchronization) types, channels, task settlement and join delivery, the [§22.12](#sec-static-members) rule-1 first-touch election, the [§28.13](#sec-shared-ownership) last-drop election); a timer-driven completion's delivery point); a **runtime-mediated world interaction** (the standard library's clock, entropy, environment and argument, console, and file-system operations — the intercept points the standard library designates); or a **foreign-function boundary event** ([§32](#sec-foreign-function-interface)). Two executions of that artifact in which corresponding edges deliver identical values in identical per-domain order are **observationally identical**: each domain runs the same continuation steps computing the same values, issues the same edge requests with the same request payloads in the same order, produces the same output bytes at each output edge, and the program terminates identically. Exclusions, exactly two: (i) elapsed real time — how long anything takes, and every value derived from an *unsubstituted* clock; (ii) the contents of foreign memory and every value obtained through it ([§32.12](#sec-foreign-memory)) outside the writebacks the boundary contract names ([§32.5](#sec-ffi-compatible-types), [§32.10](#sec-boundary-contract)) — foreign reads are outside this paragraph entirely. Memory addresses need no exclusion: no conforming program can observe one (§32.12's no-object-address invariant), and identity hash codes are derived per [§8.3](#sec-equals-and-gethashcode)'s assignment rule, not from addresses. Executions that encounter undefined behavior are outside this paragraph, as they are outside every guarantee. This paragraph imposes no new obligation on programs and no new restriction on schedulers — it states the property the preceding rules already establish, and it is what makes a recorded execution replayable (tooling: proposal 0045).

Two notes on the drafting. First, the paragraph is written to be *checkable against the implementation*: any within-domain nondeterminism given fixed edges — an uninitialized read, an unpinned iteration order, an address leak — becomes, on landing, a runtime bug by definition, which is precisely the audit value the P5 sketch claimed. Second, the identity-hash clause is a forward reference to the §8.3 amendment in §3.7 — the paragraph cannot land before that ruling does; they ride the same revision.

### 3.3 The edge taxonomy

Normative-intent inventory (the stdlib conventions note of §3.8 will carry the member-level list; this table is the design). "Step" = the observing domain's step counter position, implicit in stream order.

| # | Edge | Trigger point | Recorded (Mode W) | Replay substitution (Mode W) | Mode D delta |
|---|---|---|---|---|---|
| A1 | Continuation dispatch | §30.13 *Scheduling*: ready-queue pop | Chosen continuation's task id | Dispatcher picks the journaled continuation, waiting if not yet ready | Same, for the replayed domain only |
| A2 | Channel receive | §30.15 `Receive`/`TryReceive`/`ReceiveAll` step; bounded `Send`/`SendAsync` outcome | Delivered item's `(sender id, seq)` tag; `TryReceive`/`IsClosed` outcomes; full/closed/grant outcomes on the send side | Deliver exactly that item (payload recomputed by the re-executed sender); force the journaled outcome | **Payload serialized** into the journal; counterpart domains absent |
| A3 | Lock/semaphore grants | stdlib §6.2/§6.3/§6.11 acquisitions incl. `Try*` and async forms | Grant sequence and `Try*` outcomes per primitive | Grant in journaled order; other claimants wait | Same; grants from absent domains are journal-driven |
| A4 | Atomics | stdlib §6.4 every operation | The operation's observed/returned value | Return the journaled value; the cell is maintained journal-side | Same |
| A5 | Settlement delivery | Cross-domain join completion (§30.14), TCS/OwningTCS completion (stdlib §6.6), blocking waits outside domains (§30.3) | Arrival step; result-vs-fault kind; fault message/trace strings (§30.14's `DomainFaultedException` carries strings — journalable) | Complete at the journaled step; faults re-synthesized from journaled strings | Join/settlement **values** serialized (they are sendable by §30.14 rule) |
| A6 | Timer completions | `Task.Delay` (stdlib §6.7), `CancelAfter` (stdlib §6.1) | Delivery step only | Fire at the journaled step, **without waiting real time** | Same |
| A7 | Static-init election | §22.12 rule 1 cross-domain first touch | Winning domain + the global init order (§22.13 relies on it for exit disposal) | Elect per journal; losers block as recorded | Touches racing absent domains resolve per journal |
| A8 | `Shared<T>` last drop | §28.13 handle count reaching zero | Which drop ran the destructor | Same drop destructs | Same |
| A9 | Shared-event delivery | `SharedEventSource<T>` raise/subscribe interleave (§22.12 rule 5, stdlib §6.5) | Delivery order per raise | Deliver per journal | Same |
| B1 | Clock reads | `DateTime.Now`/`UtcNow`/`Today` (stdlib §2.5); `Stopwatch` `Elapsed`/`ElapsedMilliseconds`/`IsRunning`-adjacent reads (stdlib §8.1) | The returned value, per read | Return the journaled value; no clock consulted | Same |
| B2 | Entropy | `Random()` default ctor (stdlib §2.7); `Guid.NewGuid()` (stdlib §2.6) | The derived **seed** (ctor) — see §9.1 for the deliberate divergence from per-draw capture; the 16-byte Guid per call | Seed the instance identically (draw streams then reproduce, PRNG pinned per runtime revision, header-checked); return the journaled Guid | Same |
| B3 | Environment & args | stdlib §2.4 reads incl. `ProcessorCount` (→ `Parallel.DefaultDegree`, stdlib §6.12) and env vars; `args`/`CommandLineArgs` (§33.4, stdlib §2.4) | Header snapshot (argv, initial env, processor count); per-read frames only for reads the snapshot cannot answer; intra-run `SetEnvironmentVariable` is internal state replayed against the snapshot | Answer from snapshot/journal; the OS env is not consulted | Same |
| B4 | Console input | `Console.ReadLine`/`ReadKey` (stdlib §2.1) | Returned string/key or EOF, per call | Journaled value; stdin not consulted | Same |
| B5 | File-system reads | stdlib §7.2/§7.3/§7.4 reads, opens, existence/metadata checks, directory listings — including their `System.IO` exceptions (§7.5) | Full result bytes/values, or the raised exception's identity+message | Journaled result or re-synthesized exception; the FS not consulted | Same |
| C1 | Console/stderr output | stdlib §2.1 writes | Payload (full ≤ threshold, else digest — §3.10) + a global interleaving tag (writes are already synchronized process-wide) | **Verify-and-perform**: bytes compared (mismatch = divergence), echoed to the replay terminal | Emitted side compared as part of the differential verdict |
| C2 | File-system writes & mutations | stdlib §7 writes, creates, deletes | Op + payload (full ≤ threshold, else digest) | **Verify-and-suppress** by default: compared, not performed (sound because all reads are substituted, B5); `--live-effects` opts into performing | Same; the differential verb compares old-vs-new payloads |
| C3 | Process exit | `Environment.Exit` (stdlib §2.4), `Main` return (§33.3) | Terminal event + exit code | Verified terminal frame | Same |
| D1 | Extern calls | Every §32.1 extern invocation | Return value; declared `out`/`ref` writebacks (§32.5); post-call contents of **known-extent** buffer arguments (`RawBytes`, `CString`/`WString`, inline arrays, by-pointer C records — extents the marshaller already knows) | **Substitute; never re-invoke foreign code** (§3.5). Known-extent writebacks applied; raw-pointer arguments (`IntPtr`, `Ptr<T>`, `DangerousPointer` products) have unknowable write extents — outside the envelope (§3.5) | Same |
| D2 | Same-thread callbacks | §32.8 callbacks invoked during an extern call | Invocation count, order, and argument values (FFI-compatible types — journalable scalars/handles) | The Bezel callback body **is re-executed** with journaled arguments at the journaled points (it is Bezel code; its domain-visible effects must be recomputed, not patched) | Same |
| D3 | Foreign-thread entries | §32.10 foreign threads using the sanctioned surface | Their synchronized-surface effects land as A3/A4/A5 frames tagged *external*, with payloads **by value** | Effects re-delivered from the journal; the foreign thread does not exist during replay | Same |
| E1 | Hash seed | Proposal 0008's per-process `HashCode` seed, at startup | One header field | Installed before `Main` | Same |

Edges deliberately **absent**: cancellation (`CancellationToken` is not sendable and linking is intra-domain — §30.14, §30.8 — so polls are pure domain-local reads; only `CancelAfter`'s timer, A6, and any §32.10-surface completion, D3, touch the journal); enumerator and iteration machinery (pure computation); ownership/disposal order (fully determined by §28's rules given the schedule); static-init order beyond A7 (§1.3 item 5); identity hashes (pinned, §3.7 — no frames needed).

### 3.4 Replay scheduling, liveness, and time

Replay runs real threads and real domains (Mode W) but the dispatcher consults the journal: at each A1 point the domain blocks until the journaled continuation is ready, then runs it. Because the recorded run *happened*, and every input that made each continuation ready is itself journaled, the journaled choice always becomes ready — a stall beyond the watchdog is therefore itself a divergence signal (some earlier substitution produced a different value than recorded; report per §3.10). Timers never wait wall time (A6): a replay is typically **faster** than its recording. Nothing about replay is a profiler; see §7.

**The observer effect, stated honestly.** Recording does not *pin* scheduling — it *transcribes* it. The recorder's own microdelays shift which interleavings occur, so a recorded run is *a* real run, never "the run that would have happened unrecorded"; a Heisenbug that only manifests unrecorded stays out of reach. The consequence is operational and drives Q9's lean: for flake hunting, recording must be **on when the flake occurs** (record-always in the gate), not switched on afterward in the hope of re-luring the accident. What replay then guarantees is the strong half: any accident that happened *while recorded* is reproducible forever.

### 3.5 FFI: substitution, not re-execution — and the honest static boundary

**Decision (as the P5 brief poses, answered): Mode W does not re-invoke foreign code. Ever.** An extern call during replay consumes its D1 frame: return value, declared writebacks, known-extent buffer contents. Consequences, stated plainly:

- **External side effects do not re-occur.** A recorded run that wrote rows through `sqlite3` does not write them again under replay. This is the property that makes replaying a destructive test *safe*, and it is symmetric with C2's verify-and-suppress posture for runtime file writes.
- **Native state does not exist during replay.** A handle (`IntPtr`) returned by frame N and passed to frame N+1 is, at replay, an opaque token flowing between journal frames. Consistency is verified only to the extent the journal records argument digests (§3.9); a program whose control flow depends on *interpreting* native memory behind a retained pointer is outside the envelope below.
- **Callbacks re-execute** (D2) because they are Bezel code whose domain-visible effects (mutations of domain state, legal per §32.8's same-thread contract) cannot be patched from outside — but a callback that itself dereferences its `Ptr<T>`/`IntPtr` arguments is reading foreign memory, which brings it under the exclusion.

**The exclusion, made static.** Direct foreign-memory reads — `Ptr<T>.Load`/`LoadAt`/`CopyToArray`, `RawBytes.FromForeign` views — are compiler-intrinsic operations lowered inline (§32.12); they are not runtime-mediated calls, and their extents and validity are the programmer's trusted declaration. They are **not journaled** in v1 (the instrumented alternative is declined at §10.2), and native writes through raw retained pointers into Bezel-owned buffers are not journalable *even in principle* (no extent is knowable — §32.10's own hazard list). The language already gives this boundary a static marker, and this proposal adopts it as the replay envelope's definition:

> A module is **replay-clean** when its source contains no `extern` block ([§32.1](../Bezel_Language_Spec.md#sec-the-extern-block)) and no `import System.Interop;` ([§2.3](../Bezel_Language_Spec.md#sec-always-available-names) — already "the audit marker that a file performs foreign-memory access"). A program whose module closure is replay-clean is fully inside the Mode W guarantee. A program that crosses the marker either confines the crossing to journaled D1/D2/D3 shapes (results, declared writebacks, known extents — the shapes §32.5/§32.10 already privilege) or accepts that divergence originating in unjournaled foreign memory is detected **late** — at the next journaled edge whose value disagrees — rather than at the offending read.

`bezel replay --audit` (§3.9) lists the crossing modules and the journal-opaque call shapes, so "is this program replayable?" is answerable by inspection, before any run.

**Worked, on the spec's own §32.12 example** (the sqlite3 blob read), to make the D1 classification concrete:

- `sqlite3_column_bytes(stmt, col): int` — fully journalable: scalar args, scalar result, no writebacks. Replay substitutes the recorded `int`; sqlite never loads.
- `sqlite3_column_blob(stmt, col): Ptr<byte>` — the *call* journals cleanly (the returned address is recorded as an opaque token), but the pointer's only use is the next line's `p.CopyToArray(n)` — a §32.12 foreign-memory read, outside the envelope. At replay the copy has nothing to read.
- Consequence, and the recorded **liftable bound**: `ReadBlob` as written is journal-opaque *only* at `CopyToArray`. Unlike `Load`/`LoadAt` (candidates for inline lowering), the bulk accessors — `Ptr<T>.CopyToArray`, `RawBytes.FromForeign` — are runtime-mediated copies with **known extents at the call**; a later stage could intercept exactly these, bringing the dominant §32.12 idiom ("one explicit copy", the section's own principle) inside the envelope without touching codegen. v1 keeps the uniform exclusion for honesty (a half-covered `Ptr` surface invites false confidence); the lift is recorded here so it lands as a deliberate widening, not scope creep (§11 Q10).
- An `out`-parameter shape (`sqlite3_open`'s handle, §32.5 boundary `out`) journals as a declared writeback — D1's easy case.

The audit verb classifies every extern declaration in the closure into exactly these buckets (scalar/writeback/known-extent = journalable; raw-pointer argument or `Ptr`-returning-then-dereferenced = opaque) and reports per module.

### 3.6 Mode D: boundary payloads and the serialization bound

Mode D replays one domain with its counterparties absent, so every value that crossed the domain's boundary must be reconstructable from the journal. The boundary set is exactly §30.14's: spawn arguments, join results, channel items — all statically **sendable**. Sendable is necessary but not sufficient to journal; the recorded rule:

- **Data payloads** must be *journal-serializable*: a sendable value or `string`; or a sendable class that is `ISerializable` **and** `IDeserializable` (stdlib §2.14, including the spec §9.8-synthesized conformances), serialized with the stdlib [§2.15](../Bezel_Standard_Library.md#sec-concrete-serialization-formats) binary format — whose stated posture ("same-version persistence and IPC, where ... the producer and consumer share a build") matches a journal's lifetime exactly.
- **Capability payloads** — sendable objects whose meaning is machinery, not data: `Channel<T>`/`ChannelSender<T>`, `Shared<T>` handles over the §28.13 synchronized set, `Mutex`/`Semaphore`/atomics, `Locked<T>`/`RwLocked<T>`, `FrozenArray<T>` handles — are **virtualized**: the journal records a handle id, and every subsequent operation through the handle is an ordinary A2/A3/A4 frame against a journal-backed stub. (`FrozenArray<T>` is data-shaped and is serialized by content instead when its element type is journal-serializable.)
- **Excluded**: `Ptr<T>` and `IntPtr` payloads (foreign addresses are meaningless in a counterparty-free replay — the §3.5 exclusion, surfacing at the boundary). Recording in Mode D fails fast at the first excluded payload, naming the type, the edge, and the source position.

The bound is checked **at record time by the runtime** (which knows every crossing value's type), not statically, in v1 — a `sendable`-and-serializable static property is deliberately deferred (§10.6, §11 Q6). The diagnostic is immediate and terminal for the recording (the run itself continues un-recorded past the mark):

```
record error: Mode D payload not journal-serializable
  edge:  channel-send prices#1 (domain 3 -> domain 0)
  type:  Inventory.SkuHandle (sendable class; implements neither
         ISerializable nor IDeserializable — stdlib §2.14)
  site:  Inventory.bzl:88 (Publish)
  note:  derive or hand-write the §2.14 conformance, or exclude this
         edge from the recorded seam
```

**The seam, worked.** The natural Mode D subject is a worker-domain module — the shape §30.14/§30.15 already make idiomatic. Every API below is current stdlib (§6.8's `Spawn<A, R>(entry: (owned A) -> Task<R>, owned arg: A)` row; §6.9; §30.15's `ReceiveAll`); `Quote` is synthesis-eligible per spec §9.8/stdlib §2.14 (a positional record of `string`/`long` components), so the conformance clause is the whole serialization story:

```bezel
public record Quote(Sku: string, Cents: long): ISerializable, IDeserializable;

public static class Pricer
  public static async method Total(owned rx: Channel<Quote>): Task<long>
    using var inbox := owned rx;
    var sum: long := 0;
    await for var q in inbox.ReceiveAll() do
      sum := sum + q.Cents;
    end;
    return sum;
  end Total;
end Pricer;

public static class Checkout
  public static async method Main(): Task<int>
    var rx := new Channel<Quote>();
    var tx := rx.NewSender();           // independently owned endpoint (§30.15)
    using var worker := Domain.Spawn(Pricer.Total, owned rx);
    tx.Send(new Quote("sku-1", 1250));
    tx.Send(new Quote("sku-2", 990));
    tx.Dispose();                       // announced kill (§28.3); last sender:
                                        //   the channel drains and closes
    var total := await worker;
    Console.WriteLine($"total {total}");
    return 0;
  end Main;
end Checkout;
```

`bezel run --record --domain Pricer.Total ...` records, at the worker domain's boundary: the spawn argument (the `Channel<Quote>` — a capability, virtualized to a handle id); each received item as a serialized `Quote` body tagged `(sender, seq)`; the close; and the settlement value `2240` flowing out through the join. Replaying that journal runs `Pricer.Total` **alone** — no `Checkout`, no real channel — feeding the recorded items through the virtualized handle and comparing the emitted settlement against `2240`. Point a *regenerated* `Pricer` at the same journal (`bezel replay --diff old new journal`, §3.11) and the verdict is edge-exact: a rewrite that reorders arithmetic passes; one that drops overflow handling or reorders its *emitted* sends is caught at the first differing frame with both values printed.

**Differential verification** is Mode D run twice: replay builds *old* and *new* of the target module's program against one journal, compare the two runs' **emitted** edges (send payloads, settlement values, output digests, exit) frame-by-frame, and report the first difference with both values. The relation to proposal 0044 (P4, drafted in parallel) is complementary halves: 0044 diffs the *declared* seam (interface, contracts) statically; this verb diffs *behavior over recorded traffic* at the same seam — and the natural deployment for a module under regeneration is therefore the worker-domain shape (§30.14/§30.15), where the module's entire observable surface is already an edge stream. An intended behavior change appears as a diff at a known edge; the verdict "no diffs outside the intended set" is the regeneration oracle P5 promised.

### 3.7 Hash and order determinism: the audit's resolutions

Three coordinated pieces; together they remove category 4 of §1.3 without journal frames beyond one header field.

1. **Report (no change): the borrowing hash containers are already pinned.** `Dictionary<K, V>` and `Set<T>` iterate in insertion order (stdlib §4.8/§4.10, quoted at §1.3). Replay-stable by construction.
2. **Stdlib clarification (proposed text, §4.9 — §4.10 inherits by its existing cross-reference):**
   > The iteration order of `OwningDictionary<K, V>`, while unspecified, is **deterministic**: it is a pure function of the sequence of mutating operations performed and the key hash values involved. Two instances built by identical operation sequences over keys with identical hash values iterate identically. (This is a determinism statement, not an order statement; programs must still not rely on any particular order.)
   Cost analysis for the alternative — pinning insertion order outright — is recorded at §10.5: it would force order metadata into the drain-and-rebuild representation whose economics §4.9 already chose; determinism-given-history is everything replay needs and is what the implementation already does.
3. **Spec §8.3 amendment (proposed): identity hashes are assigned, not derived.** Append to the §8.3 table's discussion:
   > The identity-based default `GetHashCode` for classes is **assigned on first request**, not derived from the object's storage address: the runtime draws the value from a per-domain deterministic sequence (objects first hashed outside any domain draw from the startup/shutdown thread's sequence) and stores it with the object thereafter. Consequence: identity hash codes are a function of the order in which objects' hashes are first requested — deterministic under [§30.13](#sec-runtime-model)'s determinism paragraph — and never expose addresses. Programs must continue to treat identity hashes as opaque per-run values; only their *stability within a run* and their address-independence are promised.
   This is the .NET lazy-syncblock design with the entropy source replaced by a deterministic per-domain stream. Objects never hashed pay nothing; the storage is the runtime's existing per-object metadata word or a side table (implementation detail). Without this ruling, the §3.2 paragraph is false for any program that hashes a class by identity — this is the one place the proposal *must* touch object-model territory, and it is flagged as the first owner decision (§11 Q1).
4. **Proposal 0008 coordination: journal the seed, keep the randomization.** 0008's per-process `HashCode` seed (owner-confirmed) is recorded as header field E1 and installed before `Main` during replay. Recording does not weaken the hash-flood defense (the recorded run's seed was random; the journal is as sensitive as the inputs it already contains). Requires of 0008's implementation only that the seed live in one runtime-settable location initialized before any hash — a record/replay hook, not user API. A stable unseeded hash as the resolution is declined (§10.4).

### 3.8 The stdlib intercept note (proposed conventions text)

One paragraph in the stdlib's §1 conventions, so the intercept set is a maintained contract rather than proposal lore:

> **Replay intercept points.** The members of this library that consult the world outside the process — the clock and elapsed-time reads (§2.5, §8.1), entropy (§2.6, §2.7's default constructor), environment, arguments, and process information (§2.4), console input (§2.1), and the file system (§7) — together with the scheduling and synchronized-object operations the language spec enumerates ([§30.13](Bezel_Language_Spec.md#sec-runtime-model), *Determinism*), are **intercept points**: a conforming runtime may record their results and later substitute the recorded values, and no program may observe the substitution except through the values themselves. A future addition to this library that reads the world must declare itself an intercept point in its own section at the time it is added.

The last sentence is the standing obligation on proposals 0001/0007 (sockets), 0011 (Process), 0012 (`TimeZoneInfo`'s tzdata reads), 0013, 0015 (timers), 0016 (`Instant` reads), 0017/0018 (files) — enumerated in §5.

The member-level intercept list the note designates, enumerated once here so the graduation edit is mechanical (all members verified present at v0.197):

- **§2.1 `Console`**: `ReadLine`, `ReadKey` (inputs); `Write`/`WriteLine`/`Error` writes (outputs, C1).
- **§2.4 `Environment`**: `NewLine`, `MachineName`, `UserName`, `OSVersion`, `ProcessorCount`, `CommandLineArgs` (header snapshot); `GetEnvironmentVariable`, `GetEnvironmentVariables` (snapshot-answered; per-read frames past the snapshot); `SetEnvironmentVariable` (internal state, §3.3 B3 note); `Exit` (C3).
- **§2.5**: `DateTime.Now`, `UtcNow`, `Today` (B1). `MinValue`/`MaxValue` and all arithmetic are pure — not intercepts.
- **§2.6 `Guid`**: `NewGuid` (B2). Parsing/formatting pure.
- **§2.7 `Random`**: the parameterless constructor (B2, seed capture). The seeded constructor and every `Next*` member are pure given state — not intercepts (§9.1).
- **§7**: `File.*` reads/writes/existence, `FileStream` construction + I/O, `Directory.*`, `TextReader`/`TextWriter` over file streams (B5/C2), with §7.5's exceptions journaled as outcomes.
- **§8.1 `Stopwatch`**: `Elapsed`, `ElapsedMilliseconds` (B1); `Start`/`Stop`/`Reset`/`Restart`/`IsRunning` manipulate journaled clock state.
- **Language-spec-side edges** (the §3.2 paragraph's own enumeration, not stdlib members): scheduling, channels, §30.12 primitives + §6.4 atomics + §6.11 guards, settlement/TCS, spawn/join, §22.12 elections, §28.13 last drop, §6.5 shared events, timers (`Task.Delay`, `CancelAfter`).

Everything not listed is pure computation and is deliberately *not* interceptable — the note's "no program may observe the substitution" clause is what keeps the intercept set from quietly growing into an effect system.

### 3.9 The journal: format sketch, ids, and size controls

A format *sketch* (deliberately not a binary spec; the format is runtime-internal, versioned, and explicitly **not** an interchange contract — the §2.15 binary format's same-build posture, adopted verbatim):

```
journal      := header chunk*
header       := magic "BZLJ", format-version,
                runtime-revision, artifact-build-id, disposal-mode (§28.2),
                debug-elision flag (stdlib §8.2), mode (W | D | W+D),
                hash-seed (E1), prng-algorithm-id (B2),
                argv + initial-env snapshot + processor-count (B3),
                wall-clock start (informational only),
                mode-D: replayed-domain id, payload-format-id (= §2.15 binary)
chunk        := domain-id, first-step, frame-count, byte-length, crc32c, frame*
frame        := edge-kind (varint), site-id (varint), payload
site table   := interned intercept call sites (source file, line) — appended
                lazily; resolves §3.10's source positions
```

- **Per-domain streams, chunked**: each domain appends to its own chunk sequence (single writer, §3.1); chunks spill to disk as filled, so recording is O(1) memory per domain. `crc32c` is runtime-internal (no stdlib dependency; unrelated to proposal 0029's user-facing checksums).
- **Ids**: domains hierarchical by spawn order; channels/senders/handles by per-domain creation order; items by `(sender, seq)` (§3.1). All deterministic, so Mode W journals contain no addresses of any kind.
- **Size controls**: (i) world-input payloads are stored once, whole — correctness forbids truncation, so the control is *visibility*: per-edge-family byte accounting surfaced by `bezel replay --stats`, which names the hog (it is almost always B5 file reads); (ii) a record-time byte budget (`--record-limit`) that, when exceeded, **stops recording cleanly** at a marked frame — the journal replays up to the mark; (iii) D1 argument digests (for §3.5's consistency checking) are digests, not copies. Mid-stream checkpointing and ring-buffer journals are deferred with reasons (§10.7). Expected magnitudes: scheduling frames are a few bytes per dispatch; a unit-test journal is kilobytes; an I/O-heavy run's journal is proportional to bytes actually read.

### 3.10 Divergence semantics

Replay verifies as it substitutes; the **first** mismatch stops the run (later state is untrusted by definition — divergences do not compose). The report names, for the diverging domain: the step; the expected frame (kind, ids, value or digest); the actual event the re-execution produced; the source position via the site table; and the last N matching frames as context. Machine-readable (`--json`) for CI and for P12's diagnostics program. Illustrative shape:

```
divergence: domain 2 (spawned Orders.bzl:31), step 1142
  expected: B1 clock-read DateTime.UtcNow -> 2026-07-18T09:14:07.0021Z
  actual:   A2 channel-receive inbox#1
  site:     Orders.bzl:214 (Reconcile)
  context:  1137..1141 matched (A1, A2 (s1,#86), B1, A1, C1)
```

Three standing divergence classes and their meanings: a **value** divergence at a B/C/D frame (the program computed differently — in Mode W after a foreign-memory crossing, this is §3.5's *late detection*; in differential Mode D, this is the verdict itself); a **request** divergence (the program asked for a different edge than recorded — control flow changed); a **liveness** divergence (§3.4's watchdog — the journaled choice never became ready). A clean end-of-journal with the terminal C3 frame verified is the replay success criterion.

Matching rules per family, decided (what "the same edge" means frame-by-frame):

| Family | Matched exactly | Matched by digest | Not compared |
|---|---|---|---|
| A (scheduling) | Edge kind, ids `(domain, task/channel/handle, seq)`, outcomes | — | Wall-clock timing of the step |
| B (world inputs) | Edge kind, site id, request parameters (path, name, count requested) | — | The *substituted* value is definitionally consistent — B frames diverge on the request side only |
| C (outputs) | Edge kind, site id, op parameters | Payload bytes (full bytes kept under a `--record` size threshold, digest above it — mismatch reports offsets either way) | Interleaving *between* domains beyond the recorded global tags (§4) |
| D (FFI) | Symbol, site id, scalar argument values | Buffer-argument contents at call entry (consistency check for §3.5's opaque-token flow) | Foreign side effects (never re-fired) |

Digest-matched families trade report precision for journal size; the `--record` threshold (default: keep full bytes ≤ 4 KiB per write) is a knob, not a semantic: matching is equally sound either way, only the report's "expected bytes" arm degrades to a digest.

### 3.11 CLI surface (proposed; rides proposal 0040's driver)

```
bezel test --record[=DIR]          # 0022's runner: one journal per test, kept on failure
bezel test --replay JOURNAL        # re-run one test's recorded execution
bezel run --record[=FILE] ...      # record an ordinary program run (Mode W)
bezel replay JOURNAL [BIN]         # replay; BIN defaults to the header's build id
bezel replay --domain NAME ...     # Mode D: record/replay the named domain's seam
bezel replay --diff OLD NEW JOURNAL  # the differential verb (§3.6): two builds, one
                                     #   Mode-D journal, first-difference verdict
bezel replay --audit [BIN|SRC]     # §3.5: list extern/System.Interop crossings and
                                     #   journal-opaque call shapes
bezel replay --stats JOURNAL       # §3.9: per-edge-family accounting
```

Flags: `--live-effects` (§3.3 C2), `--record-limit` (§3.9), `--json` (§3.10). Stage S0 may ship these as `bezelc` modes before the `bezel` umbrella exists (0040 Q1's own lean); the verbs are specified against the driver so the surfaces converge.

### 3.12 Self-verification and journal hygiene

Two properties the implementation must hold itself to, specified now so S0's harness tests them from day one:

- **Re-record idempotence.** Replaying a journal while recording afresh must produce a journal identical to the input outside the header's informational fields (wall-clock start). This is the mechanical whole-system check — it exercises every substitution and every capture in one pass, with no oracle beyond byte comparison — and it is the S0 exit test's core (§8). It is also the *transitivity* guarantee users rely on: a journal produced by a replay replays.
- **Journals are secrets-bearing artifacts.** A journal contains the initial environment snapshot (B3 — env vars routinely hold tokens), file bytes read (B5), console input (B4), and FFI payloads (D1). This is inherent — the values *are* the execution — so the mitigation is handling, not format: journals are plaintext-equivalent secrets; `bezel test --record` defaults its journal directory into the build tree (gitignore-covered under proposal 0040's layout), the docs state the property in one blunt sentence, and `--stats` reports which families contributed bytes so an operator can see what a journal holds before sharing it in a bug report. Selective redaction is declined for v1 (§10.10): a redacted journal cannot replay (the redacted value *is* the input), and a false "sanitized" affordance is worse than a true warning. Divergence *reports* (§3.10), by contrast, print digests for payload families by default — shareable without the journal.

## 4. What is deliberately NOT promised

- **Wall-clock fidelity.** Replay reproduces values and order, never durations; timers fire at journal points (§3.4). Timing bugs reproduce only insofar as they manifested as recorded orderings.
- **Addresses.** Unobservable by construction (§32.12 rule 9); the journal never contains one.
- **Unseeded/unpinned hash values across *unrecorded* runs.** Identity hashes and 0008 hashes remain opaque per-run values (§3.7); the promise is stability within a run and under its replay, nothing more.
- **FFI side-effect re-execution.** §3.5's decision: foreign code does not run under replay; external effects (writes through natives, network sends by natives) do not re-occur, and native state is not reconstructed.
- **Foreign-memory reads.** Outside the envelope entirely (§3.5); divergence there is detected late, at the next journaled edge.
- **Cross-revision journal portability.** A journal binds to its runtime revision and format version (header-enforced refusal, §3.9); Mode W additionally binds to the exact artifact. Journals are working artifacts, not archives.
- **Replay as a profiler or a sanitizer.** No timing, no memory-pressure reproduction; ASan/checked-mode findings reproduce only if the recorded schedule reaches them (which is exactly what it makes deterministic).

## 5. Interactions (complete, honestly)

1. **Proposal 0008 (`HashCode` seed)** — E1 header field; seed installable pre-`Main`; randomization preserved (§3.7.4). 0008 need only expose the runtime hook.
2. **Spec §8.3 identity hash** — the one object-model ruling this proposal needs (§3.7.3); the §3.2 paragraph is conditional on it.
3. **Stdlib §4.9/§4.10** — clarification sentence (§3.7.2); no representation change.
4. **Proposal 0022 (test framework)** — `bezel test --record/--replay` is the runner integration; a failing test's journal becomes the bug report's reproducer. 0022's `Assert` needs nothing new.
5. **Proposal 0040 (driver)** — hosts every §3.11 verb; the artifact build-id the cache already computes is the journal's binding key.
6. **Proposal 0044 / P4 (behavioral interface)** — the static/dynamic halves of one oracle (§3.6); 0044's seam definition tells `--domain` where to record.
7. **P2 (`pure`) and P13 (properties)** — a `pure` body contains no edges by construction, shrinking journals and widening the guaranteed-identical region for free; P13's failing property cases replay from journaled seeds (its own stated dependency on P5).
8. **P12 (machine-grade diagnostics)** — `--json` divergence reports join its output vocabulary.
9. **Checked disposal mode (§28.2) and `Debug` elision (stdlib §8.2)** — both change control flow; both are header fields, and replay refuses a mismatched build flavor.
10. **`Parallel` (§30.14/stdlib §6.12)** — degree journaled via B3 (`ProcessorCount` snapshot); team domains are ordinary domains under A1; `Reduce`'s "unspecified tree" is deterministic given degree + artifact, and its frames verify it.
11. **§30.15 give-back and bounded channels** — the abandoned-receive give-back is program-driven (deterministic given A1/A2); `ChannelFullException`/capacity grants are A2 outcomes.
12. **§32.10 foreign threads** — bounded by D3: sanctioned-surface effects with by-value payloads journal cleanly; a foreign-thread TCS completion carrying a non-serializable owned class is diagnosed at record time as outside Mode W v1 (rare by construction — the sanctioned surface is narrow).
13. **Future world-reading stdlib** (0001/0007 sockets, 0011 `Process`, 0012 tzdata, 0013, 0015/0016 timers and `Instant`, 0017/0018 files) — each inherits §3.8's declare-your-intercepts obligation at design time; sockets in particular become B-family edges when they land, which is what makes recorded-traffic service replay (the long-game use case) fall out of the same design.
14. **Conformance (Appendix D)** — no level change; record/replay is a runtime capability, and the §3.2 paragraph describes all conforming executions (it constrains implementations, not programs).
15. **This repository's gate** — S0's consumer: `tests/run_parallel.sh`/`gate.sh` grow a record knob; a flaky-gate failure ships its journal instead of a "suspect, passed isolated" note.
16. **Journal handling** — journals are secrets-bearing (§3.12); test-runner defaults keep them inside the build tree, and CI recipes that upload failure journals must treat them at the sensitivity of the environment they recorded.
17. **Proposal 0049 Train C (`Channel<T>.ReceiveAll`, landed v0.186)** — the future-directions P9 note ("P5's replay journal wants channel consumption flowing through one structured edge anyway") is realized here: `ReceiveAll` consumption journals as a single A2 sub-stream (one frame per yielded item plus the drain-complete outcome), the cleanest edge shape a consumer can present; 0049's Train C currency edits and this proposal do not otherwise touch.
18. **§33.2 async `Main` forms and the startup thread** — journaled as ordinary streams (§3.1); no entry-point form is special.

## 6. Migration and compatibility

- **No source changes, anywhere.** No syntax, no new user-facing API, no new obligations on programs; a program that never records runs exactly as today (mode-off is the status quo, and §7's measurement item (1) verifies the intercept branches cost nothing).
- **The docs delta changes no program's meaning.** The §3.2 paragraph and the §3.7.2 stdlib sentence are clarification-class — they state properties as promises; the §3.8 note is conventions text. The one *observable-value* change is the §8.3 identity-hash ruling (§3.7.3): identity hash codes take different concrete values than an address-derived implementation produces. No conforming program can depend on the old values — §8.3 has never promised anything about them beyond the hash-equality invariant, and treating them as opaque is already the documented posture — so this is a permitted-implementation change ratified into a rule, not a breaking change; it ships with a runtime migration task, not a user migration.
- **Implementation migration tasks, named**: (i) the runtime identity-hash reimplementation (Q1); (ii) an audit of the runtime for latent §3.2 violations — after landing, any within-domain nondeterminism given fixed edges is a *bug class* with a name, and the audit files its instances; (iii) 0008's seed hook (§3.7.4) lands with 0008 itself.
- **Journal compatibility policy**: header-enforced (§3.9) — same journal-format version and runtime revision always; same artifact build-id for Mode W; Mode D admits differing artifacts by design. There is no cross-revision migration path for journals and none is promised (§4): re-record on upgrade. This is the §2.15 binary format's recorded posture applied to a new artifact kind, not a new policy.
- **Build-flavor discipline**: disposal mode and `Debug` elision are header fields; replay refuses a mismatch rather than diverging confusingly (§5.9). The test runner records whatever flavor the suite runs.
- **Rollback**: every stage is additive and flag-gated; disabling record/replay returns the toolchain to today's behavior with no residue beyond the landed docs paragraphs (which describe behavior that was always permitted).

## 7. Overhead honesty

Recording is a branch plus an append at operations that are already runtime calls: scheduling frames are a few bytes per continuation dispatch; synchronized-op and world frames are bytes each; world-*payload* copies (B4/B5) cost what the I/O itself cost, roughly doubling the memory traffic of read-heavy paths. Honest expectations, to be measured at S0 exit (§8): low single-digit percent slowdown for compute-bound domains; up to ~2× on file-read-saturated paths; journal bytes ≈ bytes read + small-constant × edge count. Replay usually runs *faster* than record (timers elide; the world is a file) — which is a feature for the gate and a reminder that replay is not a profiler (§4). Mode D adds serialization cost proportional to boundary traffic on the recorded run only. Production always-on recording is *not* a v1 goal; the v1 goals (tests, postmortems-by-rerecording, differential corpora) tolerate opt-in runs.

The S0 exit report publishes four numbers, so the estimates above stop being estimates: (1) mode-off overhead of the compiled-in intercept branches (must be ~0, or Q2 flips); (2) record overhead on a compute-bound suite shard and (3) on an I/O-bound one, each vs. mode-off; (4) journal bytes per test, distributed. The Q9 record-always lean stands or falls on (2)/(3); the numbers go in this section at graduation.

## 8. Staging

- **S0 — intercept layer + Mode W, aimed at the test suite.** The runtime mode, the A/B/C/E edges, D1-substitute-only FFI (enough for the suite's FFI tests), journal format v1, `--record`/`--replay` on the test runner, divergence reports. Exit criterion: the gtest suite records and replays green, with measured overhead published; a manufactured scheduling flake replays deterministically. This is the near-term flake payoff, and it is the *evidence gate* for the docs delta: the §3.2 paragraph, the §8.3 ruling, and the §3.7/§3.8 stdlib notes graduate through the normal spec-change process once S0 demonstrates the envelope on real code (owner sequencing at §11 Q8).
- **S1 — Mode D.** Boundary payload serialization over §2.15 binary, capability virtualization, record-time serializability enforcement, `bezel replay --domain`, D2/D3 completion. Exit: a worker-domain module records at its seam and replays isolated, bit-equal on emitted edges.
- **S2 — the differential harness.** `bezel replay --diff`, edge-diff reporting, CI corpus workflow, coordination with 0044's interface diff so a verdict cites both halves. The corpus workflow, concretely:

  ```
  bezel test --record --domain Pricer.Total     # green suite run -> journals at the seam
  cp build/journals/*.bzlj corpus/pricer/       # promote passing journals to the corpus
  # ... module regenerated/modified ...
  bezel replay --diff build-old/app build-new/app corpus/pricer/*.bzlj --json
  ```

  Exit: a deliberate one-line behavior change in a demo module is caught with the exact edge and position; an interface-only refactor passes clean over the whole corpus.
- **S3+ (recorded, unscheduled).** Socket edges when 0007 lands; checkpointing for long-running services; multi-journal corpora management.

## 9. Divergences from the P5 sketch (explicit, with reasons)

1. **`Random`: seed capture, not per-draw capture.** The expansion mandate names "`Random` draws" as journaled inputs (the P5 sketch itself says only "`Random` ... become[s a] journalable intrinsic"). Recorded here as construction-time seed capture instead (B2): the stdlib's stated nondeterminism is precisely the default ctor's system-time seed (§2.7) — an explicitly-seeded `Random` is already deterministic; draws after seeding are pure computation. Per-draw capture would bloat journals (a frame per `Next`, a buffer copy per `NextBytes`) and — decisive for Mode D — *break* differential runs whose two builds legitimately draw different counts (index misalignment turns an intended change into cascade noise), whereas a shared seed keeps both builds' streams aligned by construction. Cost of the choice: the PRNG stream function must be pinned per runtime revision (header field `prng-algorithm-id`, §3.9); a runtime that changes its PRNG bumps the id and old journals refuse cleanly — acceptable under §4's no-cross-revision-portability posture. Per-draw capture is retained as a declined alternative (§10.1).
2. **The edge inventory is wider than the sketch's list.** The sketch enumerates channel receives, settlements, timers, clock, `Random`, env/args, FFI. The audit (§1.3) forced additions: console and file-system I/O (without B4/B5 the determinism paragraph is simply false for any program that reads a file), atomics and lock grants (§30.13's own edge enumeration), static-init election, `Shared<T>` last drop, shared events, and the output-verification C family. This is deepening in the sketch's stated direction ("everything that would violate it must be either pinned or journaled"), recorded as a divergence only because the edge list is the design's core and the delta is large.
3. **The iteration-order audit found half the problem already solved and the other half mis-aimed.** The sketch's "if implementation-defined today, either pin it or place it behind the journal" presumed the borrowing containers were the risk; they are already insertion-order-pinned (§1.3.4). The real exposure is the owning containers plus §8.3's identity hash — resolved by a determinism (not order) pin and the lazy-assignment ruling (§3.7), with *zero* journal frames, rather than by journaling orders as the sketch's disjunction contemplated.
4. **"Bit-identical" is narrowed to "observationally identical" (edge-equivalent).** The sketch says "execution is bit-identical or the runtime reports the first divergence." Bits (addresses, uninitialized padding, foreign memory) are the wrong unit for a guarantee the language can state; §3.2 defines identity at the observability boundary, which is both provable and what every consumer of the guarantee actually uses. No use case is lost.
5. **Mode structure.** The sketch's record/replay/differential trio is reorganized as Mode W / Mode D with differential as Mode D run twice (§2, §3.6), per the item brief's own mandate; the brief's Mode-W FFI question is decided **no re-invocation** (§3.5).

## 10. Alternatives considered and declined

1. **Per-draw `Random` capture.** Declined for journal volume and differential misalignment; reasons and the cost accepted instead at §9.1.
2. **Codegen-instrumented `Ptr<T>` loads** (a `--record` build flavor that compiles §32.12 intrinsics as journaling calls). Declined for v1: it abandons the no-codegen-change property (§3.1), taxes the exact corpus (FFI bindings) least able to pay, and buys false comfort — the un-instrumentable native-writes-through-retained-pointers hole (§3.5) remains, so the envelope's honest boundary would not actually move. Revisit only with evidence of replay-clean-but-for-`Ptr` programs that matter.
3. **A deterministic-by-default runtime** (one pinned global schedule always — cooperative round-robin or PCT-style). Declined: it surrenders parallel throughput in production for a property only diagnostic runs need, and it would *mask* interleaving bugs rather than reproduce them (the recorded-random schedule is the valuable artifact). Determinism belongs to record/replay runs.
4. **A stable (unseeded) `HashCode` to avoid seed journaling.** Declined: the owner ruled per-process randomization with reasons (0008: hash-flood defense; "determinism belongs to checksums"), and journaling one 64-bit header field costs nothing. This proposal deliberately *preserves* that ruling rather than reopening it.
5. **Pinning `OwningDictionary`/`OwningSet` to insertion order** (symmetry with §4.8/§4.10). Declined on the stdlib's own recorded economics — §4.9 documents the drain-and-rebuild representation's order behavior as a chosen trade; replay needs only determinism-given-history, which is already true and merely unstated (§3.7.2).
6. **A static `journalable`/`replayable` derived type property** (compile-time enforcement of §3.6's Mode D bound, §3.6/§11 Q6). Deferred, not declined: record-time enforcement is complete and precise in v1; the static property becomes attractive if Mode D seams harden into deployment contracts (and P2's `pure` may subsume the read-side analysis). Adding a §3.6-style derived property is a spec-level cost this tooling proposal should not force pre-evidence.
7. **Ring-buffer journals and mid-stream checkpoints** (bounded recording of long-running services with replay-from-checkpoint). Deferred to S3+: checkpointing requires a domain-state snapshot mechanism this proposal otherwise never needs, and none of the v1 payoffs (tests, postmortems, differential corpora) require it. A truncated journal without a checkpoint cannot replay its tail; §3.9's cap therefore stops *cleanly at a mark* rather than pretending otherwise.
8. **Journaling payloads in Mode W too** (self-contained whole-program journals). Declined: it would impose the serializability bound on *every* cross-domain type in every recorded program — the exact tax Mode W exists to avoid — and Mode D already serves the payload-bearing use case at the seam where it pays.
9. **OS-level record/replay (rr-style) as the mechanism.** Declined as the language's answer: syscall/CPU-granularity recording is platform-bound (ptrace, perf counters, single-core scheduling), records below the abstraction (journals of raw bytes, no edge vocabulary, no §3.10 source-positioned divergences), and cannot express Mode D at all (no domain seam exists at the syscall layer). It remains a fine complementary tool for debugging the *runtime itself*, and nothing here conflicts with using it there.
10. **Journal redaction (`--redact` of env values / file payloads).** Declined for v1 (§3.12): a redacted value cannot be substituted, so a redacted journal cannot replay — the flag would manufacture a "sanitized" artifact that silently lost its purpose. The blunt secrets-bearing warning plus digest-only divergence reports serve the actual sharing need honestly.

## 11. Open questions / owner decisions

- **Q1 — the §8.3 identity-hash ruling (§3.7.3).** Approve lazy deterministic assignment (per-object storage on first request; the one object-model cost in the proposal), or direct an alternative (journal identity-hash requests as edges — zero representation change, unbounded frame cost in hash-heavy code). The §3.2 paragraph cannot land without one of them. Lean: lazy assignment.
- **Q2 — always-on intercepts vs a record build flavor.** §3.1 assumes mode checks compiled into the one production runtime (branch cost ~zero, no flavor matrix). Approve, or require a separate instrumented runtime build (faster non-recording hot path in principle, at the cost of "the binary you record is not the binary you ship"). Lean: always-on.
- **Q3 — replay output policy default (§3.3 C1/C2).** Verify-and-suppress for file mutations with `--live-effects` opt-in, console echoed. Approve or flip the default.
- **Q4 — Mode D payload format.** §2.15 binary as assumed (same-build posture matches), or a dedicated journal encoding decoupled from the stdlib's format evolution. Lean: §2.15 binary + format-id header field, revisit only if 0010's wire work moves it.
- **Q5 — `Stopwatch` substitution semantics.** B1 journals elapsed reads as values (honest, simple); an alternative synthesizes a coherent virtual clock (monotonic across reads). Values-only can expose a recorded non-monotonicity a virtual clock would smooth. Lean: values-only v1; 0016's `Instant`, when it lands, inherits the same choice.
- **Q6 — static journal-serializability (§10.6).** Confirm deferral of the derived-property route; name the evidence that would reopen it (Mode D seams as deployment contracts).
- **Q7 — foreign-thread payload bound (§5.12).** Confirm v1's record-time diagnostic for non-serializable foreign-thread completions, or require full Mode-D-style serialization there from the start.
- **Q8 — docs sequencing.** Land the §3.2/§3.7/§3.8 docs delta only at S0 exit (evidence-first; the lean, per §8), or ahead of implementation as a ruling-class revision so the runtime is built against landed text.
- **Q9 — gate integration scope for S0.** Record-always-keep-on-failure for the project's own suite (journal storage per run is kilobytes-to-megabytes), or record-on-demand only. Lean: record-always in the sharded gate once overhead is measured under ~5%.
- **Q10 — the bulk-copy lift (§3.5).** Approve recording the `CopyToArray`/`FromForeign` interception as a designed future widening of the FFI envelope (known extents, runtime-mediated, no codegen change), to be scheduled only on evidence of programs that are replay-clean but for bulk copies — or fold it into v1 despite the uniform-exclusion argument. Lean: record, do not build yet.
