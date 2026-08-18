# Proposal 0032 — Parallel extensions: `Parallel` combinators, frozen shared views, scoped fork-join

| | |
|---|---|
| **Status** | graduated-v0.186 — **superseded — graduated in v0.186 (`3d08ec2`)**: Parallel.Map/Reduce/ForEach + FrozenArray<T> (stdlib §5/§6, spec §22.12/§28.3/§28.13/§30.14); Stage-0 spawn entries already normative (v0.179); Stage-2 recorded design-now/ship-later. Open questions resolved as delegated rulings (recorded in the v0.186 changelog). *(prior: accepted (owner, 2026-07-12); draft (RFC))* |
| **Tracking issue** | [#1510](https://github.com/s8lap/bezel-lang/issues/1510) |
| **Target modules** | `System.Threading` (`Domain` overloads, `Parallel`), `System.Collections.Generic` (`FrozenArray<T>`, span `SplitAt`), spec [§30.14](Bezel_Language_Spec.md#sec-multiple-domains)/[§28.13](Bezel_Language_Spec.md#sec-shared-ownership)/[§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters)/[§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)/[§22.12](Bezel_Language_Spec.md#sec-static-members) (Stage 1.5 set scoping) |
| **Written against** | spec/stdlib **v0.177** |
| **Origin** | Owner-commissioned design lane (2026-07). Built on a completed design-space investigation whose probes live under `/var/tmp/bezel-scratch/specrun/parallel-ext/` — every "expressible today"/"rejected today" claim below was compiled and run against `build/bezelc`. |
| **Design influences** | Rust **rayon** (`join`, `par_iter`, `split_at_mut` — the API-total-disjointness discipline, *not* the `unsafe` core), .NET **TPL/PLINQ** (combinator shapes, *not* shared-capture freedom), **OpenMP/Cilk** (the lazy worker team and work-stealing runtime, *not* the race-by-discipline model), Swift task groups (whose serial half Bezel already has as `TaskScope`), Go (already subsumed by [§30.15](Bezel_Language_Spec.md#sec-channels) channels) |

Bezel today has *parallelism* (domains run simultaneously, [§30.14](Bezel_Language_Spec.md#sec-multiple-domains)) but no *data parallelism*: "run this function over this big array on all my cores" is a ~60-line ceremony with measured expressiveness holes. This proposal designs the parallel extensions in four stages — each stage stating exactly which new rules it requires, most requiring none — plus a designed-now/shipped-later flagship. Nothing here weakens the two guarantees the language is built on: no data races by construction, and every heap object disposed exactly once.

The staged shape, up front:

| Stage | What | New rules |
|---|---|---|
| **0** (prerequisite) | Async `Domain.Spawn` entry overloads; `Channel<T>.ReceiveAll()` drain form | zero checking rules — stdlib rows + join rewiring + one overload-resolution preference |
| **1** (v1) | Compiler-known `Parallel.Map`/`Reduce`/`ForEach` over a lazy worker-domain team | zero new checking rules — one table extension of the existing spawn-position category |
| **1.5** | `FrozenArray<T>` joining the internally-synchronized set → `Shared<FrozenArray<T>>` sendable, zero-copy shared inputs | no new checking rules — but one **definitional widening** of the §28.13 synchronized set, stated in §6 and put to the owner (§15 q4) |
| **2** (flagship, design now / ship later) | Parent-blocked `Parallel.Join`: the *concurrently-readable* derived property, fork-position closure admission, provenance-linear `SplitAt` span lending | four genuinely new pieces, each built on named existing machinery |
| **v2+** (endnote) | `.AsParallel()` over the affine query model | recorded, not designed |

---

## 1. What exists, measured

All citations verified against v0.177; all probes compiled and run.

**The model.** "A domain is a unit of *serial* execution: at any instant, at most one continuation belonging to a domain is running" ([§30.13](Bezel_Language_Spec.md#sec-runtime-model)). Serial execution is what makes the §28 ownership and borrow checking sound. Domains run in parallel under one invariant: "**no mutable state is ever reachable from two domains**" — everything crossing is *moved* or *immutable* ([§30.14](Bezel_Language_Spec.md#sec-multiple-domains)). Sendability is a derived type property ([§3.6](Bezel_Language_Spec.md#sec-derived-type-properties) table row); function-typed values are **never** sendable ("their captures are borrows of the origin domain's scopes", §30.14); spawn entries must be capture-free; `Shared<T>` is sendable "iff `T` is sendable and internally synchronized" ([§28.13](Bezel_Language_Spec.md#sec-shared-ownership), v0.140).

**What is expressible today** (probes):

- **Chunked parallel reduce** (`p1_parsum.bz`, runs, correct): copy chunks out (`Skip/Take/ToArray` — spans are borrow-bearing, hence not sendable; `n1_span_arg.bz` confirms rejection), one `Domain.Spawn` per chunk with a static-method entry, await the joins. Ceremony: a named static method per operation (function values can't cross — `n2_fn_channel.bz` confirms), a materialized copy of every chunk, flat fan-out only.
- **Pipelines** (`p2_pipeline.bz`, runs, correct): the [§30.15](Bezel_Language_Spec.md#sec-channels) shape works well; the drain loop needs `catch ChannelClosedException` as its *normal* exit (§4.2 below).
- **Divide-and-conquer: flattened only** (`p3_dnc.bz` runs; `p3b_nested.bz` **rejected**): a child domain cannot await its own children — all nine `Spawn` overloads (stdlib [§6.8](Bezel_Standard_Library.md#sec-domain)) take synchronous entries, and the blocking alternatives deterministically raise inside a domain ([§30.3](Bezel_Language_Spec.md#sec-the-task-and-taskt-types)). Recursive parallel algorithms are **inexpressible** (§4 fixes this).
- **Worker pool** (`p4_pool.bz`, runs, correct): N single-consumer work channels (MPSC — the composition stdlib §6.9's own deferral note names: "worker pools compose today from N single-consumer channels") + one results channel; an aggregate sealed sendable context class per worker (spawn takes one argument); `Task.FireAndForget(WorkerLoop(owned ctx))` inside the sync entry so the receive loop can be async — after which the join handle completes immediately and worker faults are "logged, not propagated" (§30.14 domain-completion clause): **supervision is lost**. Completion must be counted in-band. Total: **~60 lines of ceremony for map-square-over-ints.** That measurement is this proposal's motivation record.
- **Shared reads: none without copies.** Bezel has no read-only borrow of a class object, and no borrow of any kind crosses a domain. The only shared-immutable precedent is `string`: "it crosses as a shared reference, which is safe because strings are immutable (implementation note: the runtime's reclamation bookkeeping for strings must tolerate cross-domain sharing)" (§30.14).

**Why a user-side library cannot close the gap.** A generic user-written `Parallel.Map` is structurally impossible today: its function parameter would have to cross a domain boundary, and function-typed values are never sendable; the call-site capture-free check that `Domain.Spawn` gets is compiler-known machinery unavailable to user signatures. Whatever fixes this must be a compiler-known surface — the same position `Domain.Spawn` already occupies.

## 2. The core tension and the four strategies

Data parallelism needs exactly two things the domain invariant bans: **shared immutable reads** (N workers scanning one big input) and **disjoint mutable writes** (N workers filling N slices of one output). Borrows cannot cross domains; there is no read-only borrow form; there is no split-borrow. Four reintroduction strategies organize every candidate design, and this proposal ships them in cost order:

| Strategy | Mechanism | Soundness source | Stage |
|---|---|---|---|
| **MOVE** | copies / ownership transfer | already sound (§30.14 verbatim) | 1 |
| **FREEZE** | immutable views shared by reference | the `string` precedent + §28.13 v0.140 | 1.5 |
| **BLOCK** | parent-quiescent frame lending | new (built on §28.14's freeze + §3.6's mechanism) | 2 |
| **LOCK** | guard-mediated access | proposal [0003](0003-locked-t.md) (amended; §11) | complement |

## 3. Engaging the recorded decline: Send/Sync, the Bezel way

The rationale records a decline this proposal must not re-propose around. Deciding v0.131's serial-domain model, [rationale §12.5](Bezel_Design_Rationale.md#sec-execution-domains) declined *Send/Sync-style thread-safety typing*:

> "*Send/Sync-style thread-safety typing* (Rust's approach) statically tracks which types may cross threads; it is the right tool for a language committed to shared-memory parallelism, but it infects every generic signature, roughly doubles the constraint vocabulary, and buys safety for a parallelism model Bezel does not actually offer — the cost lands on every user, the benefit on few."

What v0.133 then *accepted* ([rationale §12.6](Bezel_Design_Rationale.md#sec-sendability-spawning-channels)) is the shape that decline permits: a **derived type property checked at crossing points** — sendable is computed from structure, appears in no generic constraint vocabulary, and is checked only where a value actually crosses (spawn arguments, results, channel elements). That is Bezel's Send: same guarantee as Rust's, paid for only at boundaries.

Every stage of this proposal follows the accepted pattern and none touches the declined one:

- Stages 0/1 add **no** property at all — they reuse sendable at the same crossing points it already governs.
- Stage 1.5 adds **no** property — it adds one type to an existing compiler-known table.
- Stage 2 needs the **Sync analog** — a classification of what may be *read concurrently* — and defines it exactly the way §12.6's precedent demands: a derived type property ([§3.6](Bezel_Language_Spec.md#sec-derived-type-properties): "Computed, never declared"), checked **only at fork positions**, appearing in no `where` clause anywhere. §3.6's closing rule makes this form mandatory, not merely stylistic: "New rules added to this specification that need a structural classification of types must define it as a derived type property under the rules of this section, rather than introducing a parallel mechanism." One deliberate asymmetry, to preempt the "but `Mutex` is `Sync` in Rust" objection: Bezel's analog is *narrower* than Rust's `Sync` — the §30.12 lock types are excluded (§7.1), because Bezel's fork parent blocks while holding its frame, which makes lock capture a deadlock vector rather than a sharing vector.

The distinction that reconciles the decline with this proposal: what was declined was *pervasive constraint vocabulary*; what is proposed is *boundary-checked derived properties*. The cost lands on the code that crosses, and only there.

## 4. Stage 0 (prerequisite): async spawn entries, and a channel drain form

### 4.1 Async `Domain.Spawn` entries

Today a spawned domain's entry is synchronous, so a child domain can never await — not its own spawns (nested fork-join), not a channel receive under supervision. Probe `p3b_nested.bz` (async static entry) is rejected — the compiler reads `() -> Task<int>` as a sync entry returning a non-sendable `Task<int>`; probe `p4_pool.bz` shows the sanctioned workaround (`Task.FireAndForget` inside the entry), which forfeits the join handle and fault propagation per §30.14's domain-completion clause. Proposed: nine async rows mirroring stdlib [§6.8](Bezel_Standard_Library.md#sec-domain)'s nine sync rows.

```bezel
// PROPOSED additions to stdlib §6.8 `Domain` (async entries; representative forms —
// the full set mirrors the existing nine: {no-arg, owned class arg, value arg} ×
// {no result, sendable class result, sendable value result}).
public static method Spawn(entry: () -> Task): owned Task;
public static method Spawn<A>(entry: (owned A) -> Task, owned arg: A): owned Task
    where A: class;                                      // A sendable (compiler-checked)
public static method Spawn<R>(entry: () -> Task<R>): owned Task<R>
    where R: notnull;                                    // R sendable value type or string
public static method Spawn<A, R>(entry: (owned A) -> OwningTask<R>, owned arg: A): owned OwningTask<R>
    where A: class, R: class;                            // A, R sendable (compiler-checked)
```

Semantics: the join handle completes **when the entry's returned task completes** — [§30.14](Bezel_Language_Spec.md#sec-spawning-domains)'s completion clause already speaks in exactly these terms ("When the entry's task completes, the join handle completes (with the result or fault)"); the async form makes the join track that task instead of the sync prefix. The entry's `Task`/`OwningTask` object never crosses the boundary — it lives and dies in the child domain; only the (sendable) result or the fault crosses, by the unchanged rules. A fault anywhere in the entry's async chain surfaces at the join as `DomainFaultedException` — strictly better supervision than today's FireAndForget-logged fallback. Capture-free entry and sendability checks apply unchanged.

**Backward compatibility / overload resolution.** A `Task`-family entry return currently *unifies* with the sync rows (`Spawn<R>(entry: () -> R)` with `R = Task<int>`) and then fails the sendable check — every program these new rows would capture is an error today, so adding them changes no program's meaning. Resolution rule: an entry whose return type is in the `Task` family resolves to the async rows; the sync rows never bind `R` to a `Task` type (which the sendable check already guaranteed de facto).

What this unlocks — recursive parallel sort, expressible for the first time (the child domain awaits *its* children):

```bezel
public static async method ParSort(owned data: Array<int>): OwningTask<Array<int>>
  using var d := data;
  if d.Count < 1024 then
    return d.Sorted();                       // base case: fresh sorted copy (stdlib §4.4)
  end;
  var mid := d.Count / 2;
  using var left  := Domain.Spawn(Program.ParSort, d.Take(mid).ToArray());  // child domain — NEEDS Stage 0
  using var mine  := await Program.ParSort(d.Skip(mid).ToArray());          // this domain, meanwhile
  using var theirs := await left;                                           // nested join
  return Program.Merge(theirs, mine);
end ParSort;
```

(Chunks are still copies — that is Stage 1.5's business, not Stage 0's.)

> **New rules required: zero checking rules; one overload-resolution preference.** Overload rows in the compiler-known `Spawn` table, plus the Task-family-return resolution preference above — a genuine (if small) resolution rule, counted rather than rounded to zero; it is backward-compatible because every program it captures is an error today. The join handle's completion rewired to the entry's task; the fault path reuses `DomainFaultedException`. No new checking machinery, no spec-rule change — §30.14's completion clause already states the semantics.

### 4.2 Rider: `Channel<T>.ReceiveAll()` — a non-exceptional drain form

The canonical consume loop today uses `ChannelClosedException` as control flow for *normal* termination — "if empty and closed, it raises ChannelClosedException" ([§30.15](Bezel_Language_Spec.md#sec-channels)) is the only async exit, and probe `p2_pipeline.bz`'s consumer is a `while`/`catch` contortion. The language already has the right vocabulary: `await for` over `IAsyncEnumerable<T>`/`IOwningAsyncEnumerable<T>` (stdlib [§4.12](Bezel_Standard_Library.md#sec-borrowing-enumeration-interfaces)–[§4.13](Bezel_Standard_Library.md#sec-owning-enumeration-interfaces), spec [§30.7](Bezel_Language_Spec.md#sec-asynchronous-programming-async-iterators)).

```bezel
// PROPOSED additions to stdlib §6.9:
public method ReceiveAll(): IOwningAsyncEnumerable<T>;   // class T form
// value/string T form: ReceiveAll(): IAsyncEnumerable<T>

// The pipeline consumer becomes:
await for var item in inbox.ReceiveAll() do
  Handle(item);
end;                                          // completes normally on drained-closed
```

Semantics are a restatement of `Receive`'s: yield each item (ownership transferring, class T), suspend while empty-and-open, and complete the enumeration **normally** when the channel is drained and closed. At most one `ReceiveAll` enumeration may be active, and interleaving it with direct `Receive` calls raises `InvalidOperationException` — the channel is single-consumer already; this just names the obligation. The enumerator is affine like every single-pass source ([§29.11](Bezel_Language_Spec.md#sec-multiple-enumeration)).

> **New rules required: zero** — a stdlib method whose semantics restate `Receive`'s over the existing async-iteration interfaces.

## 5. Stage 1 (v1): the `Parallel` combinator library

This is probe `p4`'s proven 60-line pattern with the ceremony deleted — the MOVE strategy, packaged. A compiler-known static class in `System.Threading`:

```bezel
// PROPOSED — System.Threading.Parallel (compiler-known; see "why compiler-known" below).
public static class Parallel
  // The runtime team's width: hardware concurrency — the value of
  // Environment.ProcessorCount (stdlib §2.4) at team creation. Fixed per process.
  // (A named alias, not a second source of truth; it exists so `degree` semantics
  //  are documented where the knob lives. See §15 q11.)
  public static readonly property DefaultDegree: int;

  // ---- value elements (sendable value types and string): chunks cross as copies ----
  public static method Map<A, R>(items: Array<A>, body: (A) -> R,
      degree: int := 0): owned Task<Array<R>>
      where A: notnull, R: notnull;                      // body is a SPAWN POSITION (see below);
                                                         //   Task<Array<R>> is Pattern A — the task
                                                         //   owns the result array, callers borrow
                                                         //   (the §6.7 WhenAll<T> precedent)

  public static method Reduce<A, R>(items: Array<A>, seed: R,
      selector: (A) -> R,                                // spawn position
      combiner: (R, R) -> R,                             // spawn position; MUST be associative
      degree: int := 0): owned Task<R>
      where A: notnull, R: notnull;

  public static method ForEach<A>(items: Array<A>, body: (A) -> (),
      degree: int := 0): owned Task
      where A: notnull;

  // ---- class elements: ownership partitions in, results partition back ----
  public static method Map<A, R>(owned items: OwningArray<A>, body: (owned A) -> owned R,
      degree: int := 0): owned OwningTask<OwningArray<R>>
      where A: class, R: class;                          // A, R sendable (compiler-checked)

  public static method ForEach<A>(owned items: OwningArray<A>, body: (owned A) -> (),
      degree: int := 0): owned Task
      where A: class;
end Parallel;
```

`degree := 0` means `DefaultDegree`; any positive value caps the number of team workers the call uses. As with `Domain.Spawn`, the `where` clauses are the declared constraints and **sendability is the stronger compiler-checked requirement**, diagnosed at the call site naming the failing rule.

### 5.1 Semantics (normative once this lands)

- **The function parameters are §30 spawn positions.** [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters) already names the category — standard-library callees state their retention in their signatures, with "the asynchronous spawn positions of §30.13 [checked] under §30's capture and sendability rules rather than this section" — and `Domain.Spawn` already enforces it at call sites ([§30.14](Bezel_Language_Spec.md#sec-spawning-domains): "The entry must be capture-free: a non-capturing lambda or a static method reference"). `Parallel.*`'s bodies are checked by the *same* rule at the *same* kind of site: capture-free, argument/result types sendable.
- **Snapshot-at-call.** For borrowed inputs (`items: Array<A>`), the chunks are copied **synchronously by the call itself**, before the returned task is exposed. [§30.13](Bezel_Language_Spec.md#sec-runtime-model)'s run-to-completion makes this a real contract, not a hope: the calling continuation cannot suspend mid-call, so by the first point at which any parent-domain code could mutate the source, the workers already hold copies — mutation-after-call can never race a worker. For owned inputs (`owned items: OwningArray<A>`), the elements are *moved* — partitioned across workers, results partitioned back; the source array is consumed by the call.
- **Results in input order.** `Map` reassembles results positionally regardless of which worker finished first.
- **First-fault-wins.** The first body fault cancels the remaining work (unstarted items are skipped; running bodies complete — cancellation is cooperative per the language's uniform posture, [rationale §12.4](Bezel_Design_Rationale.md#sec-cooperative-cancellation)) and the returned task faults with `DomainFaultedException` carrying the first fault's message and stack-trace text — the same crossing vocabulary as every domain fault (§30.14), and the same no-aggregation posture as `TaskScope` (stdlib [§6.10](Bezel_Standard_Library.md#sec-taskscope); [rationale §13.5](Bezel_Design_Rationale.md#sec-handler-exception-policy-first-error-wins-not-aggregate)). Later faults are swallowed. For class-element `Map`/`ForEach`, items already moved into the call but never processed are disposed by the runtime (the channel-close precedent, §30.15: disposing the channel "disposes every undelivered class-typed item in reverse insertion order").
- **The parent suspends; it never blocks.** `Parallel.*` returns a task; the caller awaits it. No §30.3 question arises in Stage 1.
- **The team.** A lazy, process-wide team of worker domains, created on first use, `DefaultDegree` wide, reused across calls; per-worker work queues ride the existing channel core. Team domains are ordinary [§30.13](Bezel_Language_Spec.md#sec-runtime-model) domains; bodies run as their continuations. (The team is deliberately the shared substrate Stage 2 will also stand on.)
- **`Reduce` reassociates.** The combiner must be associative; the runtime combines per-worker partials in an unspecified tree. For floating-point, a parallel `Reduce` may differ from the serial fold in the last bits — stated plainly, not hidden.

### 5.2 Why compiler-known, and what "spawn position" costs

Function values are never sendable, so this library is *not writable in user Bezel* (§1). Making `Parallel.*` compiler-known is what lets its call sites get the same capture-free/sendability classification `Domain.Spawn` gets. The honest accounting: this **extends the site list of an existing checked category** — sema reuses the existing spawn call-site classifier over a table of `Parallel` entry points — it does not add a checking rule. The body never crosses a boundary as a *value*; the compiler proves at the call site that it never needs to (capture-free = a static function, reachable from every domain by name, not by closure).

> **New rules required: zero new checking rules; one table extension.** The spawn-position category (§28.14/§30.14) gains the `Parallel.*` function parameters as checked sites. Elements cross by the ordinary sendable discipline. No grammar change, no type-system change, no new derived property.

### 5.3 Worked example: parallel word count

Total count via `Reduce`; per-word frequencies via chunked class-element `Map` plus a serial merge (the merge is inherently sequential and cheap; the counting is where the time goes).

```bezel
module main;
import System.Threading;
import System.Collections.Generic;
import System.Collections.Linq;

public static class Program
  // ---- bodies: static methods = capture-free, the spawn-position requirement ----
  public static method Words(line: string): int
    using var parts := line.Split(' ');                  // stdlib §9.6
    return parts.Count;
  end Words;

  public static method Add(a: int, b: int): int
    return a + b;
  end Add;

  public static method CountChunk(owned lines: Array<string>): owned Dictionary<string, int>
    using var ls := lines;
    var counts := new Dictionary<string, int>();         // string keys, int values: sendable (§30.14)
    for var line in ls do
      using var words := line.Split(' ');
      for var w in words do
        if counts.ContainsKey(w) then
          counts[w] := counts[w] + 1;
        else
          counts[w] := 1;
        end;
      end;
    end;
    return counts;                                       // fresh owned, crosses back by move
  end CountChunk;

  public static async method Main(): Task<int>
    using var lines := LoadLines();                      // Array<string>

    // 1. total words: value-element Reduce — chunk copies, associative combine
    var total := await Parallel.Reduce(lines, 0, Program.Words, Program.Add);

    // 2. frequencies: caller chunks, class-element Map, serial merge
    var chunks := new OwningArray<Array<string>>();      // plain var: it is MOVED into Map below
    var per := (lines.Count + Parallel.DefaultDegree - 1) / Parallel.DefaultDegree;
    var i := 0;
    while i < lines.Count do
      chunks.Add(lines.Skip(i).Take(per).ToArray());     // materialized copies (MOVE strategy)
      i := i + per;
    end;

    using var partials := await Parallel.Map(owned chunks, Program.CountChunk);
    using var merged := new Dictionary<string, int>();
    for var part in partials do                          // borrows; partials keeps ownership
      for var e in part do
        if merged.ContainsKey(e.Key) then
          merged[e.Key] := merged[e.Key] + e.Value;
        else
          merged[e.Key] := e.Value;
        end;
      end;
    end;

    Console.WriteLine($"{total} words, {merged.Count} distinct");
    return 0;
  end Main;
end Program;
```

Compare probe `p4_pool.bz`: the context class, the per-worker channels, the FireAndForget contortion, the in-band completion counting — all gone; supervision (first fault at the `await`) restored. The remaining wart is the chunk copies, which is exactly what Stage 1.5 removes for read inputs.

**Deliberately not in v1** (each a recorded deferral, not an oversight): *async bodies* (`(A) -> Task<R>` — I/O-bound fan-out already has `TaskScope` intra-domain and channels cross-domain; mixing the two models in one API needs its own design); *class-element `Reduce`* (an `owned`-threading combiner protocol — the Map-then-serial-merge shape above covers it); *a `CancellationToken` parameter* (tokens are not sendable, §30.14 — cross-domain cancellation is a channel message today; a sendable cancellation primitive is its own future design); *user-declared spawn positions* (would require exposing the classifier — evidence first).

## 6. Stage 1.5: `FrozenArray<T>` — zero-copy shared inputs

The cheap half of rayon: N domains concurrently reading one big input with **no copies**. The FREEZE strategy. The design composes two existing rules — plus **one definitional widening that must be stated plainly, not rounded down to a table row**:

1. **The `string` precedent** (§30.14): immutable data may cross as a shared reference; the runtime's reclamation bookkeeping must tolerate cross-domain sharing.
2. **The §28.13 v0.140 rule, reused verbatim**: "`Shared<T>` is sendable **iff `T` is sendable and internally synchronized** — the compiler-known synchronized set: `Mutex`, `Atomic<T>`, `AtomicInteger<T>`, and `Semaphore`."

The widening: every current member of the synchronized set carries actual runtime synchronization machinery. Admitting `FrozenArray<T>` on the ground that "an immutable object is vacuously internally synchronized" changes what the set *means* — from "has a lock" to "safe to reach from two domains at once, by machinery **or by deep immutability**." That is a spec-semantics decision, and this proposal treats it as one: it is an owner question (§15 q4), not a footnote. Two axes must also not be conflated: immutable *reads* need no synchronization, but *reclamation* does — which is exactly why #689's atomic `Shared` control block is a hard prerequisite (below), plus the same cross-domain-reclamation tolerance the spec already notes for `string`.

```bezel
// PROPOSED — System.Collections.Generic. Deeply immutable, fixed-size array.
// v1 element restriction: T is a sendable value type or string.
public class FrozenArray<T>: IEnumerable<T>
  public readonly property Length: int;
  public readonly property [i: int]: T;                  // get-only; no mutating surface exists

  // Construction consumes or copies its source — after either, no writer can exist.
  public static method From(owned buffer: OwningBuffer<T>): owned FrozenArray<T>;
      // VALUE-TYPED T ONLY (see category dispatch below).
      // O(1): steals the storage; the buffer is consumed (owned-marker at the call
      // site, the 0006 `MemoryStream.FromBytes` precedent for owned-vs-copy overloads)
  public static method From(items: Array<T>): owned FrozenArray<T>;    // copies; the only
                                                         //   construction form for T = string
  public method ToArray(): owned Array<T>;               // mutable copy back out

  destructor;                                            // releases the backing storage
end FrozenArray;
```

**Category dispatch on the construction surface (the §4.14/§6.9 house convention).** The `From(owned buffer: OwningBuffer<T>)` row exists for **value-typed T only**: `OwningBuffer<T>` does not admit `string` — "Runtime-managed reference types (`string`, function types) are not admitted as T" (stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)) — so no such buffer can exist to steal. String-element frozen arrays construct only through the copying `From(items: Array<string>)`. Stated in the cost accounting because "zero-copy" is this stage's headline: **the O(1) freeze is value-typed-only; string elements always pay one copy at construction** (the sharing after construction is still zero-copy for both).

**Classification (one table row plus the scoped set membership):**
- `FrozenArray<T>` is **sendable** (compiler-known row, §30.14 list): wholly-owned immutable state — it may cross alone, as an owned transfer, like any sendable class.
- `FrozenArray<T>` joins the **internally-synchronized set** — the definitional widening above — with its scope stated exactly, because the set currently feeds three rules. It joins for **§28.13 sendability** (`Shared<FrozenArray<T>>` is sendable per v0.140, verbatim — clone a handle per worker, move the clones, N domains read one buffer) and for **§22.12 rule 4's warning suppression** (a `static readonly` `FrozenArray<T>` is a sound all-domains singleton). It must **not** join §22.12 **rule 2**'s mutable-static admission: `static var f: FrozenArray<T>` would make the *field slot* — not the array — a cross-domain mutable location, with unsynchronized reassignment and unsynchronized disposal of the displaced value. The spec amendment therefore scopes the row (or splits the table) rather than appending a name to one list. (Whether rule 2's admission carries a latent version of the same slot-reassignment question for the *current* set is flagged for the spec lane; nothing here depends on resolving it.)

Handle counts are maintained with synchronized operations and the last handle — from whichever domain — disposes the buffer; that is precisely the machinery v0.140 already requires of sendable `Shared<T>` (implementation prerequisite: **#689**, the open v0.140 implementation issue, plus the same cross-domain-reclamation tolerance the spec already notes for `string`).

```bezel
var raw := LoadSamples();                                        // owned OwningBuffer<double>
using var input := new Shared<FrozenArray<double>>(
    FrozenArray<double>.From(owned raw));                        // O(1) freeze; explicit type args —
                                                                 //   generic-class static access, §22.12
using var t1 := Domain.Spawn(Program.ScanLow,  input.Clone());   // Shared<FrozenArray<double>>: sendable
using var t2 := Domain.Spawn(Program.ScanHigh, input.Clone());
var answer := await t1 + await t2;                               // zero bytes copied

public static method ScanLow(owned h: Shared<FrozenArray<double>>): double
  using var handle := h;
  var a := handle.Get();                                 // borrow of the frozen array (§28.13 Get)
  var best := a[0];
  for var i in 1..<a.Length do
    if a[i] < best then best := a[i]; end;
  end;
  return best;
end ScanLow;
```

**Why no writer can exist:** `FrozenArray<T>` has no mutating member, and both `From` forms sever the source — the owned overload consumes the buffer (the caller provably loses it, §28.8 machinery), the borrowing overload copies. There is nothing to freeze *later* and no window in which a mutable alias survives.

**Composition with Stage 1:** overloads `Parallel.Map/Reduce/ForEach(items: Shared<FrozenArray<A>>, ...)` skip the chunk copies entirely — the call clones one handle per worker (documented; `Clone()` inside a compiler-known callee, cost stated in its stdlib section) and workers index disjoint ranges of the one buffer.

**Deviation from the investigation's sketch, recorded:** the dive drafted `raw.Freeze()` as an instance method consuming its receiver. Bezel has **no receiver-consumption mechanism** — no rule lets a method call discharge the receiver's ownership — and inventing one for this API would be real new ownership surface. The static `From(owned …)` factory expresses the same O(1) steal with existing rules and matches 0006's owned-marker-distinguished constructor precedent. (An `OwningBuffer<T>.Freeze()` sugar can ride any future receiver-consumption design; nothing here blocks it.)

**Deferred:** frozen **class trees** (freeze = seal a transfer-closed ownership tree — designed extension, real work in the disposal story); span/slice views over frozen backing (today's `ReadOnlySpan<T>` is a record over `OwningBuffer<T>` specifically, stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans) — a frozen view family should be designed once, together with Stage 2's needs); `FrozenDictionary`/`FrozenSet` (same pattern, evidence first).

> **New checking rules: zero. Definitional widenings: one, named.** One stdlib type with a read-only surface, the §30.14 sendable row, and the scoped synchronized-set membership above. The checking machinery is the composition of the `string` immutable-crossing precedent and the v0.140 `Shared<T>` rule reused verbatim — but what "internally synchronized" *means* genuinely widens (machinery **or** deep immutability), and that ruling goes to the owner (§15 q4) rather than riding in as a table row.

## 7. Stage 2 (flagship — design now, ship later): `Parallel.Join` and disjoint span lending

Stages 0–1.5 cover fan-out over *elements*. What they cannot express is **in-place** divide-and-conquer — quicksort partitioning one buffer, a stencil filling two halves of one output — because that needs the two banned capabilities at once: shared reads of frame data without copies, and *disjoint mutable writes* into one allocation. This stage is the BLOCK strategy: a compiler-known construct under which the parent is provably quiescent, so lending frame state to workers is sound for the construct's extent.

```bezel
// PROPOSED — the 2-way form (K-way and array forms follow the same rules):
public static class Parallel
  public static method Join(par a: () -> (), par b: () -> ());
end Parallel;
```

`par` marks a **fork position** — a §28.14-family parameter marker like `scoped`, participating in signature identity the same way. In v-first only compiler-known stdlib members declare `par`. `Join` runs the closures on the worker team and **blocks the calling continuation until both complete**; first fault wins (the sibling completes or is cooperatively cancelled; the fault re-raises at `Join` as `DomainFaultedException`).

### 7.1 The four new pieces (this stage has real spec surface — stated plainly)

An earlier draft of this section claimed three pieces and closure of every escape route; adversarial review produced constructible breaking programs against three of its stated rules. The corrected rules below are stated *with* the attacks that forced them, so the review cycle starts from the real design, not the optimistic one.

**(1) A `concurrently-readable` derived type property** ([§3.6](Bezel_Language_Spec.md#sec-derived-type-properties) table row — the mandated mechanism, per §3.6's closing rule). It classifies what a fork closure may capture:

- sendable value types and `string`;
- `FrozenArray<T>` and `Shared<FrozenArray<T>>` (Stage 1.5) — and any future frozen family member;
- read-only span/view forms **whose source is itself admitted** (deferred to the frozen-view design, §6).

Deliberately **excluded** in v-first, and this is a correction to the investigation's sketch (recorded as such): the §30.12 synchronization types (`Mutex`, `Semaphore`), channels, and the task family are **not** concurrently-readable. The dive listed the synchronized set as capturable; but a fork closure that could `Acquire()` a mutex the *blocked parent already holds* is a deterministic deadlock, and it would break the liveness argument in (4).

Excluding them from *capture* is necessary but — the earlier draft overclaimed here — **not sufficient**: capture is not the only route into a fork closure. **Process-global statics are reachable by name, no capture needed**, and [§22.12](Bezel_Language_Spec.md#sec-static-members) admits mutable statics of the synchronized set (rule 2: `Mutex`, `Semaphore`, `Atomic<T>`, `AtomicInteger<T>`, `Channel<T>`, `ChannelSender<T>`) and `static readonly` locks warning-free (rule 4). Breaking program against the earlier draft: the parent takes `using var g := Globals.M.Acquire();` and calls `Parallel.Join(...)` whose closure calls a helper that runs `Globals.M.Acquire()` — the parent-held-lock deadlock, through a plain blocking call ([§30.12](Bezel_Language_Spec.md#sec-thread-synchronization): `Acquire()` returns a `Lock`) that no capture check ever sees. The honest liveness accounting is therefore three-part:

- **Task and channel waits are already defended — at runtime, by existing machinery.** Fork bodies run as continuations of team domains (§5.1), so [§30.3](Bezel_Language_Spec.md#sec-the-task-and-taskt-types)'s v0.162 rule applies verbatim: `await` does not exist in a sync closure, and `Wait()`/`Result` on an incomplete task "raises `InvalidOperationException` at the call site instead of parking" — including inside any helper the closure calls, and including tasks reached through statics. Channel receives only wait through the task family (`Receive` returns a task; `ChannelSender<T>.Send` never blocks, stdlib §6.9), so they are covered by the same guard.
- **§30.12 blocking acquisition needs a new guard — counted, not assumed away.** `Mutex.Acquire()` and `Semaphore.Acquire()` are plain blocking calls with no task in sight; nothing existing stops them inside a fork closure. This proposal adds the same v0.162-shaped runtime rule: **inside a fork extent, `Mutex.Acquire()` and `Semaphore.Acquire()` raise `InvalidOperationException`** naming the fork extent (their `AcquireAsync` forms are unreachable — no `await`). Static rejection cannot see these calls through helpers, so the guard is a runtime check, exactly like §30.3's own fallback.
- What remains is **divergence** — a fork closure can loop forever, as any code can; no liveness rule in the language excludes that.

With the guard, "fork closures are pure compute" is an **enforced invariant**, not an assumed discipline: they read admitted data, write their disjoint spans, call functions — and every blocking or waiting primitive they could reach deterministically faults instead of parking. That property is load-bearing in (4). (Widening to lock captures with a parent-does-not-hold obligation is a possible later step; evidence first.)

Like every derived property: computed never declared, per-instantiation for generics, named in diagnostics — and checked **only at fork positions**. It appears in no constraint vocabulary (§3's decline, engaged in §3 above; §3 also records why this Sync-analog is deliberately narrower than Rust's).

**(2) A fork-position closure admission** — the one place in the language where a capture-bearing closure's borrows reach another thread:

- **Captures remain §28.9 borrows — no second capture mode.** The earlier draft said sendable value types are "captured as copies at fork time"; that would have been exactly the second capture mode [rationale §12.6](Bezel_Design_Rationale.md#sec-sendability-spawning-channels) declines (§10 engages the decline for move-capture; a copy-capture variant rides the same objection — closure typing, §28.3 escape analysis). Corrected: a closure argument to a `par` parameter may capture, as ordinary borrows, (a) concurrently-readable values and (b) writable spans carrying a disjointness provenance (piece 3).
- **Fork-closure bodies are write-restricted — a rule, not an implication.** Inside the closure body, assignment to a captured source, any store through a captured borrow, and passing a captured source to a `ref`/`out` parameter (the write-by-helper route, statically visible at the call site) are rejected; the only admitted writes are through captured writable spans (piece 3) and to closure-local state. Without this stated rule, two closures borrowing the same `int` local could both assign it — a cross-thread race that "classifies what a fork closure may capture" alone does not exclude. Reads of captured sources through borrows are sound: the parent is blocked and the sources are frozen.
- Captured sources are **frozen** for the construct's extent — [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters)'s freeze, verbatim ("the captured sources are **frozen**: they may not be moved from or reassigned (the same freeze the `catch` borrow rules apply)"). In the blocked form this freeze is enforced trivially by quiescence; it becomes a real static check only in the awaiting variant (below).

**(3) Disjoint span lending: provenance linearity plus opaque lending.** The disjoint-splitting API — rayon's `split_at_mut` discipline with the `unsafe` confined inside a compiler-known member:

```bezel
// PROPOSED — stdlib §4.14/§4.15 additions (value-typed T shown; ArraySpan<T> twin analogous):
public record SpanPair<T>(First: Span<T>, Second: Span<T>);
// on Span<T>:  public method SplitAt(index: int): SpanPair<T>;   // [0, index) and [index, Length)
```

The earlier draft claimed disjointness was closed by construction under a per-binding linearity rule. Two constructible breaking programs refute that, and each forces a named piece of machinery:

*Attack 1 — spans are copyable records, so per-binding linearity is vacuous.* `Span<T>` is a plain record (stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)); `var lo2 := lo;` mints a second binding denoting the same slot range, and "each binding captured by at most one closure" then admits `lo` in one closure and `lo2` in the other — two closures racing on the same slots while every per-binding check passes. The rule must be **provenance-linear, not binding-linear**: the admission traces each captured writable span back through copies, `Slice` narrowings, and record projections to a **disjointness witness** — one component of one `SplitAt` result (or the `AsSpan()` of distinct buffers) — and each witness may feed **at most one closure** of the `Join`, however many bindings alias it. This is a genuine new local dataflow analysis (the same family as §28.3's borrow-source tracking, but a new instance), and it is counted as such, not filed under bookkeeping.

*Attack 2 — the buffer is recoverable from the span, so admission-time checks alone close nothing.* `Span<T>`'s `Buffer` is a **public positional-record property** (stdlib §4.14). Inside a fork closure, `lo.Buffer` recovers the whole underlying `OwningBuffer<T>`, and `lo.Buffer[j] := …` — or `new Span<T>(lo.Buffer, …)` — writes the *sibling's* range while every stated capture check passes: concurrently-readable is checked at fork positions, not at body expression sites, and *capturable* and *reachable* are different predicates. The fix is **opaque lending**: within a fork extent, the buffer-recovering projections of the span family must be unreachable — `.Buffer` on any span value, and span construction over a buffer (`new Span<T>(…)`/`new ReadOnlySpan<T>(…)`/the `AsSpan` forms); spans narrow only via `Slice`, which stays inside the witness's range by construction. Two mechanisms can enforce it, and the cost difference must be stated plainly: **(a) a body-side rejection rule** is cheap only intraprocedurally, and intraprocedural is *insufficient* — `() => Program.Helper(lo)` where `Helper(s: Span<int>)` does `s.Buffer[…] := …` re-opens the hole one call away, so the rule must extend to every function reachable from a fork closure with a span parameter: an interprocedural fork-reachability classification of functions, real new analysis surface. **(b) A distinct opaque lend-span type** (no `Buffer` member, `Slice`-only narrowing) closes the hole by ordinary typing — a helper declared over `Span<T>` simply cannot receive one — at the cost of forking the span vocabulary and every span-generic helper on the fork path. The typing route is the decidable-by-construction one and is the lean here; the choice is the owner's, §15 q9.

One residual is inherited, not created: a `static readonly` `OwningBuffer<T>` admitted under §22.12 rule 4's declaration-site **warning** is reachable by name from any fork closure — exactly as it is from any two `Domain.Spawn` domains today. That warned, programmer-obligation residual pre-exists this design; fork extents neither widen nor close it, and it is named here rather than silently absorbed into a "no escape hatch" claim.

With provenance linearity and opaque lending together, the disjointness argument is real: the only writable locations reachable from a fork closure are slot ranges of distinct disjointness witnesses, closure-local state, and the pre-existing rule-4 warned statics — sibling writes are provably disjoint.

**(4) A blocking dispensation for `Join`** — the genuinely new ruling. [§30.3](Bezel_Language_Spec.md#sec-the-task-and-taskt-types) bans blocking waits inside domains because "any awaited chain whose completion routes through that domain can never finish: a deterministic deadlock." That argument does not apply to `Join`'s forks **as enforced by piece (1)**: fork closures cannot wait on tasks or channels (the existing v0.162 runtime guard) and cannot block on §30.12 locks (the new fork-extent guard), so their completion never routes through the blocked caller or any other domain; they terminate by their own CPU work or diverge as any loop can. The residual liveness hazard is *team exhaustion under nesting* (every worker blocked in its own nested `Join`), solved as rayon solves it: **a blocked parent lends its thread to the team** (work-stealing) — the blocking `Join` is also the recruitment mechanism.

And blocking is not merely tolerable here — it is the *soundness mechanism*: a blocked continuation blocks its whole domain (§30.13 run-to-completion), so **no other continuation of the parent's domain runs during the fork extent**; nothing domain-reachable can be mutated under the workers.

*Recorded alternative — `Parallel.JoinAsync`, weaker, and NOT the cheap reuse the earlier draft claimed.* The parent suspends instead of blocking; other parent-domain continuations *do* run during the fork extent. The earlier draft asserted that §28.14's declared-before-and-frozen admission shape, reused verbatim, restores soundness. **It does not** — declared-before does not make a local unreachable from sibling continuations. Breaking program: `using var buf := new OwningBuffer<int>(100);` then `await using var scope := new TaskScope();` with `scope.Spawn((…) => …)` capturing `buf` — admitted by §28.14, since `buf` is declared before `scope` — then `await Parallel.JoinAsync(…)` over spans of `buf`. During the suspension the TaskScope child continuation runs on the parent domain and writes `buf` through its captured borrow while a fork worker writes a lent span on another thread: a cross-thread data race passing every condition the earlier draft stated. The blocked form is immune to this exact attack — a blocked continuation stalls the whole domain, so no sibling continuation runs (§30.13). The awaiting variant therefore needs a genuinely stronger admission: **no captured source, and no object reachable from one, may be reachable from any stored callback (§28.14 storage), pending continuation, or live scope across the suspension** — a frame-wide reachability analysis with no existing shape to reuse. The recommendation is the blocked form as primary, now for two reasons: nesting/work-stealing is the flagship use case, *and* the awaiting variant's admission is the most novel analysis on either path, not the §28.14 reuse it first appears. This is open question §15 q6.

**Memory-model clause** (new, small): a happens-before edge from the `Join` call to each closure's start, and from each closure's completion to `Join`'s return — the parent's prior writes are visible to closures; closures' writes are visible to the parent afterwards. This is the §30.13 memory-visibility guarantee extended to fork extents.

**The invariant, restated honestly — and at the right granularity.** Stage 2 relaxes the *letter* of §30.14's "no mutable state is ever reachable from two domains" at exactly one construct. The earlier draft proposed "no mutable state is ever **concurrently reachable from two running domains**" — which is *still false for this construct's own flagship case*: during a `Join` over `SplitAt` halves, the one mutable `OwningBuffer` is reachable (and writable, in disjoint ranges) from two **running** team domains simultaneously. Object-granular reachability is the wrong predicate for the construct being admitted. The honest amendment is **location-granular**: "no mutable memory location is ever accessible from two running domains, **except that a `Join` extent may lend provably disjoint slot ranges of one allocation to distinct fork closures**, under the fork-admission rules" (section numbering illustrative). The *justification* of the original invariant — no two threads ever access a mutable location concurrently without synchronization — is fully preserved: the parent provably isn't running, sibling writable ranges are provably disjoint (piece 3), and everything else capturable is immutable. This is a real amendment to flagship spec prose; it is the price of in-place parallelism; and note it is needed under **both** the blocked and the awaiting variants (the awaiting variant also runs fork closures on other threads during the extent), so it is ratified on its own (§15 q7), not as a rider on the blocked-vs-awaiting choice. This proposal puts it in v2, not v1, precisely because it deserves its own review cycle.

### 7.2 Worked example: in-place parallel quicksort

```bezel
public static class Program
  public static method QSort(s: Span<int>)
    if s.Length < 32 then
      Program.InsertionSort(s);                  // plain serial helper
      return;
    end;
    var p := Program.Partition(s);               // Hoare partition, returns pivot index
    var parts := s.SplitAt(p);                   // disjoint by construction: two witnesses
    var lo := parts.First;                       // each witness feeds exactly ONE closure —
    var hi := parts.Second;                      //   provenance-linear, so a copy of lo
                                                 //   counts against lo's witness (§7.1(3))
    Parallel.Join(
        () => Program.QSort(lo),                 // fork closures: capture ONE disjoint span each;
        () => Program.QSort(hi));                //   parent blocked; nested Joins steal work
  end QSort;

  public static async method Main(): Task<int>
    using var buf := LoadData();                 // owned OwningBuffer<int>
    Program.QSort(buf.AsSpan());                 // sorts in place, in parallel, no copies
    return 0;
  end Main;
end Program;
```

No copies, no `unsafe`, recursion nests on one work-stealing team — and every ingredient is checked: the closure admission rejects a capture of `buf` itself (an `OwningBuffer<int>` is not concurrently-readable), the provenance rule rejects both closures capturing spans of one witness (however many bindings alias it), opaque lending makes `lo.Buffer` unreachable inside the fork extent (by type or by rule, §7.1(3)), and the freeze forbids the frame from reassigning `lo` mid-construct. This is the candidate that makes Bezel's answer *stronger* than rayon's marketing: rayon's disjointness rests on an `unsafe` block inside `split_at_mut`; Bezel's rests on a compiler-known member plus checked admission and body rules, with **no user-reachable escape hatch in the construct itself** (the one inherited residual — §22.12 rule-4 warned statics — is named in §7.1(3), pre-exists this design, and is unchanged by it). Under q9's type mechanism, `QSort`'s parameter and `SplitAt`'s pair are spelled in the lend-span vocabulary; the example's shape is unchanged either way.

> **New rules required: four, named.** (1) The `concurrently-readable` derived type property — a §3.6 table row, defined structurally, checked only at fork positions — together with the fork-extent runtime guard on §30.12 blocking acquisition that makes the pure-compute property an enforced invariant. (2) The fork-position closure admission (`par` marker): borrow captures only (no second capture mode), the body write-restriction, and the §28.14 freeze for the extent. (3) Disjoint span lending: the provenance-linear witness analysis plus the opaque-lending requirement (type-based, or an interprocedural body rule — §15 q9), with the `SplitAt`/`SpanPair<T>` stdlib surface. (4) The §30.3 blocking dispensation for `Join` (with the memory-model clause and the location-granular invariant amendment), justified by (1)'s enforced pure-compute property; *or* the awaiting variant, which trades work-stealing for a frame-wide reachability admission that is genuinely novel, not a §28.14 reuse. Every piece builds on named existing machinery; none requires read-only borrows in general position, and nothing touches code outside fork positions and fork-closure bodies.

## 8. What stays channels: the pipeline, and the guide chapter

Not everything should become a combinator. Heterogeneous, streaming, rate-mismatched work — parse/enrich/write stages, producer-consumer with back-pressure — is what [§30.15](Bezel_Language_Spec.md#sec-channels) is *for*, and probe `p2_pipeline.bz` shows it working well today (better still with §4.2's `ReceiveAll`):

```bezel
// stage 2 of a pipeline: owns its inbox, sends enriched records onward
public static async method Enrich(owned ctx: EnrichCtx): Task
  using var c := ctx;
  await for var rec in c.Inbox.ReceiveAll() do           // Stage 0 rider (§4.2)
    c.Out.Send(Program.Annotate(owned rec));             // ownership transfers at both ends —
                                                         //   the v0.169 call-site marker is
                                                         //   mandatory on a named owned binding
  end;
end Enrich;                                              // sender disposed: downstream drains
```

Independently of every other stage, the worker-pool/pipeline/scatter-gather patterns measured by probes `p1`–`p4` should become a **guide chapter** (the docs workflow, not this proposal): they remain the right tool for non-uniform work, and the guide is where the "which shape do I reach for" decision belongs. Stage 1 does not replace channels; it replaces the *abuse* of channels for uniform data-parallel maps.

## 9. v2+ endnote: `.AsParallel()` over the affine query model

Recorded as the ergonomic end-state, not designed here. The striking fact — worth recording even this early — is that Bezel's **affine query ruling** ([§29.11](Bezel_Language_Spec.md#sec-multiple-enumeration), the #1226 ruling) removes PLINQ's two classic semantic hazards *structurally*:

1. **Re-enumeration nondeterminism is already a compile error.** C# PLINQ's worst wart — re-enumerating a deferred parallel query silently recomputes it under different interleavings — cannot exist: a bound lazy query "may be enumerated **at most once**"; a second enumeration is a static error where visible and `InvalidOperationException` otherwise. The hazard class PLINQ documents around is *absent*, not mitigated.
2. **Consumption transfers the whole plan.** §29.11's normative consumption-point list (v0.165, #1353) already includes "passing the query as an `IEnumerable<T>` argument: the caller's binding is consumed and the single-enumeration right transfers to the callee" — so `xs.Where(f).AsParallel()` legally *consumes* the pipeline; the parallel engine owns the plan outright, with no aliasing of partially-consumed state.

What affinity does **not** solve, and why this is v2+: operator callbacks are closures with frame borrows (the stdlib-certified storing signatures §28.14 names), so running them on workers needs either a capture-free restriction checked at the `AsParallel` boundary (spawn-position style) or Stage 2's fork admission; and chunking needs an indexable source (`Array<T>`, `OwningBuffer<T>`, `FrozenArray<T>`) — over an arbitrary iterator it must either degrade to serial or reject, *loudly, saying which*. The engine (parallel planner, ordered merge, per-operator fallbacks) is high-cost machinery for wins Stage 1 already delivers on explicit shapes. Do not gate anything on it.

## 10. Declined, with reasons

**A `parallel for` statement** (`parallel for var i in 0..<n do out[i] := F(inp[i]); end` — Chapel/OpenMP style). Declined; the one candidate with no acceptable soundness story at this language's standards:

- Its safety rests on *per-index write disjointness* (`out[i]` touches only slot `i`), which requires dependence analysis the spec could neither state simply nor check totally. Every degradation is off the table by recorded principle: a runtime check hides a per-iteration cost and converts compile errors to runtime ones; UB is not in this language's vocabulary; and a "trust me" independence annotation is precisely what [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties) rule 1 forbids ("Computed, never declared").
- The library-first posture is already Bezel policy at the statement level: "Bezel has no language-level synchronization primitives (no `synchronized` keyword, no `lock` statement)" ([§30.12](Bezel_Language_Spec.md#sec-thread-synchronization)). Parallel *control flow* warrants the same discipline as parallel *synchronization*.
- Everything the statement would express safely is already this proposal: uniform bodies over elements = Stage 1; in-place disjoint writes = Stage 2, where disjointness is by construction instead of by analysis.

**GPU offload.** Out of scope: an FFI/backend concern with its own memory-transfer and kernel-compilation design space. Nothing in the domain model or in any stage forecloses it later.

**Move-capturing spawn/body lambdas** (closing Stage 1's capture-free restriction by letting closures *own* their captures). Not re-proposed: [rationale §12.6](Bezel_Design_Rationale.md#sec-sendability-spawning-channels) records the decline — §28.9's capture-is-borrow is one uniform closure story, and a second capture mode "would ripple into closure typing, disposal of captured-but-never-run closures, and the §28.3 escape analysis" — and explicit `owned` arguments are semantically identical and visibly costed. Stage 1's bodies take elements as parameters, which is exactly the sanctioned shape.

**User-generic parallel libraries** (exposing the spawn-position/sendability classification to user-declared signatures). Declined for now: it is the first step onto the constraint-vocabulary road §12.5 declined; the compiler-known table costs nothing at call sites and can be widened later without breaking anything. Revisit with evidence that user frameworks are being blocked.

**A runtime send-check** (dynamic graph verification to admit open classes across boundaries): already recorded declined in §12.6; nothing here disturbs that.

## 11. The shared-state complement: proposal 0003

The four strategies of §2 include LOCK, and it is deliberately *not* delivered by this proposal: [0003 — `Locked<T>`/`RwLocked<T>`](0003-locked-t.md) is the shared-**state** story (a registry, an index, a cache — one structured mutable object reached from several domains through guards), where this proposal is the shared-**compute** story (many workers, moved or frozen data, no locks anywhere in the API). They compose: `RwLocked<T>` read guards held concurrently by several domains are the parallel-read primitive for *structured mutable* state, exactly where `FrozenArray<T>` (bulk immutable) and Stage 1/2 (compute) stop.

The investigation found 0003 as drafted needs amendments before it can carry that role — its central example is ill-formed under §28.6/§28.11 (a fresh owned `Session` stored into a *borrowing* `Dictionary` slot), and it is silent on the interior-ownership condition and §28.13 sendability position its own static (all-domain-reachable) use case requires. The amendments are recorded on 0003's tracking issue ([#1404](https://github.com/s8lap/bezel-lang/issues/1404), comment of 2026-07-07) — recorded per the file-an-issue discipline, but **not yet landed in the 0003 file itself**; this proposal's dependency is therefore on unlanded text, flagged as such. It takes a dependency only on the *amended* 0003 and adds nothing to it.

## 12. Implementation cost, per stage

| Stage | Compiler | Runtime | Spec/stdlib text | Size |
|---|---|---|---|---|
| **0** | overload rows in the compiler-known `Spawn` table; async-row resolution preference | join-handle completion rewired to the entry task; `ReceiveAll` iterator over the channel core | stdlib §6.8/§6.9 additions; §30.14 completion-clause touch-up | **small–medium** |
| **1** | reuse the spawn call-site classifier over a `Parallel` entry-point table | the worker-domain team (lazy, `DefaultDegree`-wide, per-worker queues on the channel core); chunk/partition/reassemble; first-fault cancellation | new stdlib §6.11 section | **medium** — the team is the main work, and it is shared with Stage 2 |
| **1.5** | two table rows (§30.14 sendable list; §28.13/§22.12 synchronized set) | `FrozenArray` storage + steal/copy `From`; atomic `Shared` counts — **prerequisite #689** (v0.140 implementation) | one stdlib type section | **small–medium** |
| **2** | the derived property; the `par` marker + fork admission (body write-restriction, provenance-linear witness analysis, opaque-lending body checks); freeze enforcement | work-stealing blocked-parent scheduling on the Stage-1 team; `SplitAt`; the fork-extent guard on §30.12 acquisition | a §30.16-sized spec section + §3.6 table row + location-granular invariant amendment | **high** — the one stage with real spec surface |

Sequencing: 0 → 1 → 1.5 are one-lane-each after the pre-work bugs (§13). Stages 0 and 1 are independently shippable and independently valuable. Stage 1.5 is **not** independently schedulable in the same sense: its whole point — the zero-copy `Shared<FrozenArray<T>>` fan-out — is **hard-gated on #689** (the v0.140 `Shared<T>` sendability implementation, itself unshipped), so its timeline is #689's timeline; only the bare sendable-`FrozenArray<T>`-by-move row ships without it. Stage 2 ships only after its rulings (§15) and a review cycle of its own.

## 13. Pre-work: known compiler bugs on these exact paths (reference, not fixed here)

The investigation's probes surfaced four compiler bugs sitting directly on the paths this proposal leans on — filed, and cheap to fix before any stage lands:

- **#1490** — `Domain.Spawn` with a non-capturing *value-arg lambda* entry compiles then crashes at runtime (glibc pthread assert). Spawn path; spec-admitted entry form.
- **#1491** — `Domain.Spawn(() => 42)` silently yields `0`: the int-returning no-arg lambda's result is lost. A silent wrong value on the flagship API.
- **#1492** — an *unqualified* static method reference is rejected by the spawn capture-free classifier (qualified form works); the same classifier Stage 1 reuses.
- **#1493** — bare owned-field borrow-return spuriously rejected when the field type is `ChannelSender<T>` (§28.6 dispensation skipped for runtime-known types); bites the team's channel plumbing.

Also filed from the same investigation: **#1494** (spec §30.3's prose contradicts its own v0.162 `OwningTask<T>.Result` code block — an editorial docs-workflow fix, independent of this proposal).

## 14. What this does NOT give you

Stated plainly, so nobody buys this proposal expecting rayon-plus-everything:

- **No shared mutable state without a lock or a channel.** Nothing here adds unsynchronized shared writes — that is the point, not a gap. Structured shared state is 0003's lane (§11).
- **Stage 1 copies or moves.** The MOVE strategy pays for isolation with chunk copies (borrowed inputs) or consumption (owned inputs); parallel speedup exists only where body cost dominates copy cost. Stage 1.5 removes the copies for *read* inputs only — and its O(1) freeze is value-typed-only; string elements pay one construction copy (§6).
- **No zero-copy sharing of arbitrary object graphs.** `FrozenArray<T>` v1 is value/string elements; frozen class trees are designed-later (§6). Your `OwningDictionary` does not cross without moving.
- **No async bodies in `Parallel.*` v1.** CPU-bound combinators only; I/O-bound fan-out stays `TaskScope` (intra-domain) and channels (cross-domain).
- **No caller cancellation of a running `Parallel` call** in v1 — `CancellationToken` is not sendable; first-fault cancellation is internal to the team. (Cooperative cross-domain cancellation remains the channel-message pattern; a sendable cancellation primitive is future work.)
- **No determinism promise for `Reduce` over non-associative operations** — floating-point sums reassociate; the serial fold is the reference semantics only for associative combiners.
- **No automatic parallelization, ever.** Every parallel site is written as one: a `Parallel.` call or a `Domain.Spawn`. No heuristics decide for you.
- **Until Stage 2 ships: no in-place divide-and-conquer, no capture-bearing closures near parallelism.** Stages 0–1.5 are capture-free-and-move/freeze only; that ceiling is explicit and is exactly what Stage 2 lifts.
- **No GPU.**

## 15. Open questions for the owner

1. **Ratify the staged shape**: ship 0 → 1 → 1.5 as three lanes (1.5's freeze half gated on #689, §12); hold Stage 2 at designed-not-shipped pending its rulings (this section) and the pre-work (§13)? Ratifying this shape means, explicitly: **the flagship rayon-parity value — in-place divide-and-conquer — ships in a later cycle, not v1**; v1 delivers MOVE + FREEZE only, where "parallel speedup exists only where body cost dominates copy cost" (§14).
2. **Stage 1 fault identity**: the returned task faults with `DomainFaultedException` (reusing the §30.14 crossing vocabulary, no new type). Acceptable, or is a dedicated `ParallelFaultedException: DomainFaultedException` naming the failing index worth its surface?
3. **`Reduce` associativity is documented, not checked** (no effect-system pretensions). Confirm.
4. **Stage 1.5's definitional widening of the synchronized set** (§6): "internally synchronized" comes to mean *has synchronization machinery **or** is deeply immutable*, scoped to §28.13 sendability and §22.12 rule-4 suppression and explicitly **not** rule-2's mutable-static admission. This is a spec-semantics ruling, not a table row — ratify the widening and its scoping.
5. **`FrozenArray.From(owned …)` static-factory shape** (deviation from the dive's receiver-consuming `Freeze()`, §6): accept the factory, or commission a receiver-consumption mechanism first (real new ownership surface — not recommended for this lane)?
6. **Stage 2's load-bearing ruling — blocked vs awaiting**: the parent-**blocked** `Join` (work-stealing recruitment; needs the §30.3 dispensation whose liveness argument §7.1(4) makes, resting on the enforced pure-compute property including the new fork-extent acquisition guard) versus the parent-**awaiting** variant (forfeits work-stealing **and** — corrected from the first draft — needs a genuinely novel frame-wide reachability admission, not a §28.14 reuse; §7.1(4)'s TaskScope-sibling attack). The proposal recommends blocked-primary; this is the flagship decision.
7. **The §30.14 invariant amendment, ratified on its own** (§7.1): the location-granular rewording — "no mutable memory location is ever accessible from two running domains, except provably disjoint slot ranges lent under a `Join` extent." Needed under **both** variants of q6, so it is not subsumed by that choice; it is the most consequential piece of spec prose in this document.
8. **Stage 2 v-first exclusions and the acquisition guard**: the proposal *narrows* the investigation's sketch by excluding locks/channels/tasks from `concurrently-readable`, and adds the fork-extent runtime guard on `Mutex.Acquire()`/`Semaphore.Acquire()` (§7.1(1) — the parent-held-mutex deadlock is reachable through §22.12 statics by name, so capture exclusion alone is insufficient). Confirm the narrowing and the guard; widening to lock captures later is additive.
9. **Opaque lending mechanism** (§7.1(3)): a distinct opaque lend-span type without the `Buffer` projection (leaned to — sound by ordinary typing, since intraprocedural body checks are refuted by the one-call-away helper attack), or the body-side rejection rule extended interprocedurally (keeps one span vocabulary; costs a fork-reachability classification of functions)?
10. **`par` spelling and reach**: marker name, and confirm v-first restricts `par` declarations to compiler-known stdlib members (mirroring spawn positions), not user signatures.
11. **`degree: int := 0` = `DefaultDegree`** — or a nullable `degree: int?` / named constant? Cosmetic, but it is the API's most-typed knob. (Note `DefaultDegree` is a documented alias of `Environment.ProcessorCount` at team creation, stdlib §2.4 — a knob-shaped question is whether the alias is worth having at all, §5.)

## 16. Relationship to existing work

- **spec [§30.13](Bezel_Language_Spec.md#sec-runtime-model)/[§30.14](Bezel_Language_Spec.md#sec-multiple-domains)/[§30.15](Bezel_Language_Spec.md#sec-channels)** — the runtime model, sendability discipline, and channels everything here composes over; Stage 0 amends §30.14's stdlib surface, Stage 1.5 adds the §30.14 row and the scoped §28.13 set membership (a definitional widening, §6), Stage 2 proposes the §30.16-sized construct and the location-granular invariant amendment.
- **spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters)** — supplies both load-bearing precedents: "spawn position" as a named category of specially-checked function parameters (Stage 1's entire soundness story) and the freeze (Stage 2's capture discipline).
- **spec [§3.6](Bezel_Language_Spec.md#sec-derived-type-properties)** — the mandated mechanism for Stage 2's `concurrently-readable`; [rationale §12.5/§12.6](Bezel_Design_Rationale.md#sec-execution-domains) — the Send/Sync decline this proposal engages in §3 and does not re-litigate.
- **spec [§29.11](Bezel_Language_Spec.md#sec-multiple-enumeration) (#1226 affinity ruling)** — why `.AsParallel()` (§9) is *safer* in Bezel than its C# namesake; recorded for v2+.
- **stdlib [§6.8](Bezel_Standard_Library.md#sec-domain)/[§6.9](Bezel_Standard_Library.md#sec-channel)/[§6.10](Bezel_Standard_Library.md#sec-taskscope)** — `Domain` (Stage 0 extends), `Channel` (Stage 0 rider; the team's substrate), `TaskScope` (the intra-domain concurrency complement; its first-fault posture is Stage 1's model).
- **Proposal [0003](0003-locked-t.md) (+ amendments on [#1404](https://github.com/s8lap/bezel-lang/issues/1404))** — the shared-STATE complement (§11).
- **Proposal [0006](0006-stream-family.md)** — the owned-marker-distinguished construction precedent `FrozenArray.From` follows.
- **#689** — the open v0.140 `Shared<T>` sendability implementation; a Stage 1.5 prerequisite.
- **#1490 #1491 #1492 #1493** — pre-work compiler bugs on the spawn paths (§13); **#1494** — the §30.3 editorial spec defect found en route.
- **Probes** — `/var/tmp/bezel-scratch/specrun/parallel-ext/` (`p1`–`p4`, `n0`–`n4`): the measured evidence base for §1's claims.
