# Proposals currency audit — 2026-07-15

Lane: proposals-currency (phase3). Base: main @ fd5cf07, docs **v0.194**, working tree read-only.
Scope: every non-graduated, non-declined proposal in `docs/proposals/` (30 files: 0001–0031 minus
graduated 0003; 0032–0039 graduated). Checked against: v0.186–v0.188 graduations, v0.190
task-ownership (#1786), v0.191 editorial/fact-check batch, v0.193's eight rulings (#1793–#1800 +
FS/PG threading), v0.194 bare-T returns (#1801), the pending v0.196 designs
(`phase3/i1790-design.md` #1790 iterator-capture, `phase3/i1803-design.md` #1803 owned-erasure),
and the phase1c stdlib-back findings (LB-01…LB-21, esp. LB-03).

Method: every proposal's header + design surface read; retired-construct greps run across all 30
(`event` member kind, object-initializer `with` blocks, `ReadOnlyAsciiByteSpan`, §24/§22.11/§26.5.1
citations, pre-v0.166 `Owned*` naming); the load-bearing spec passages re-read at v0.194 (§28.3
bare-T rule text, §28.14 scoped-construction bullet, §9.8 synthesis text, §10.5, §2.14 tokens,
§6.7 `WhenAllOwned`).

Verdicts: **CURRENT 13 · NEEDS-NOTE 16 · CONFLICTS 1**.

## The table

| # | Proposal | Pin | Status | Verdict | One-line note |
|---|---|---|---|---|---|
| 0001 | Http client | v0.179 | draft | **NEEDS-NOTE** | v0.190 ratifies its task-ownership assumptions; Q6 (span-async) settled by landed prohibition; "static interface members are deferred" premise stale (shipped v0.176) |
| 0002 | Http server | *(none in header)* | draft | **NEEDS-NOTE** | header lacks pin+tracking rows; dep 0003 graduated v0.186 (cite §30.12/§6.11; `static owned readonly` lock statics); `Listen(routes:)` needs a §28.14 capture-free-register entry (v0.187 mechanism) |
| 0004 | MVU | *(none in header)* | early draft (owner-gated) | CURRENT | v0.188 `with` retirement is class-source-only; MVU's record `with` unaffected; header lacks pin/tracking rows |
| 0005 | Self-host collections | pre-v0.168 (banner) | draft (owner-gated) | **NEEDS-NOTE** | banner incomplete: add v0.188 §2.8 module-identity alignment for the prelude loader; v0.194/PG-08 checks on the POC; #1803 does NOT lift §28.5's field rule |
| 0006 | Stream family | v0.177 | accepted (owner review pending) | **NEEDS-NOTE** | LB-03: its "Read returns 0=EOF (§7.1)" premise is a *missing* stdlib contract; span-async substrate recital stale (v0.179/#1438); #1439 scoped-construction LANDED v0.180 — borrowed-ctor v2 unblocked |
| 0007 | System.Net substrate | v0.177 | accepted (owner review pending) | **NEEDS-NOTE** | §5.3/Q3 span-overload tension is now moot (removal landed); TcpListener classification now entails v0.186 §28.13/§22.12 membership rulings; FFI v2 offers a declaration route for `bz_tls_*` |
| 0008 | Integer toolkit | v0.177 | accepted (owner review pending) | **NEEDS-NOTE** | the deferred `IBinaryInteger<T>`-style generic surface must be designed under #1803's pending producer-marker/erasure regime (BigInteger-class conformer is #1803's named case); v1 concrete surface unaffected |
| 0009 | Base64 + hex | v0.177 | accepted (owner review pending) | **NEEDS-NOTE** | drop the `ReadOnlyAsciiByteSpan` implementor mention (v0.188 pre-registered this exact edit); confirm §3.5/§3.6 slot numbers vs the tombstoned §3.4 |
| 0010 | Serialization value builtins | v0.177 | draft | **NEEDS-NOTE** | spec §9.8 rewritten v0.182/v0.184: ONE eligibility list became TWO (general + record-admissible); the five conformances must join both or the acceptance test still fails |
| 0011 | Process | v0.177 | accepted (owner review pending) | **NEEDS-NOTE** | v0.190: `WaitForExitAsync` must mint a fresh task per call (never re-expose a stored task); PG-12 (#1799) validates the exit-time kill+reap destructor; FFI v2 unblocks the spawn routing |
| 0012 | DateTimeOffset + TimeZoneInfo | v0.177 | accepted (owner review pending) | CURRENT | §25.2 record `=`-overload sanction verified intact at v0.194; dep 0010 unchanged |
| 0013 | Environment | v0.177 | accepted (owner review pending) | CURRENT | Q1's §22.12 grey area reads the same against the v0.186-restructured rule; #1363 routing stands |
| 0014 | Signal hooks | v0.177 | draft | **NEEDS-NOTE** | drain-thread/foreign-thread reasoning now has normative backing (v0.186 §32.10 pins foreign-thread obligations incl. allocation) — re-cite; FFI v2 gives the sigaction/self-pipe layer an in-language route |
| 0015 | PeriodicTimer | v0.177 | draft | CURRENT | `Task<bool>` per-call mint fine under v0.190; #1378 dep unchanged |
| 0016 | Monotonic time | v0.177 | draft | CURRENT | — |
| 0017 | TempFile + Path | v0.177 | draft | CURRENT | destructor posture consistent with PG-12 shutdown semantics |
| 0018 | Async File | v0.177 | accepted (owner review pending) | CURRENT | already aligned with the #1438/v0.179 span-async prohibition; v0.190 signature-neutral |
| 0019 | JSON DOM | v0.177 | draft | **NEEDS-NOTE** | envelope/embedded-field story must be re-derived against the v0.182/v0.184 §9.8 rewrite (class synthesis from field types; record clause forbids JsonValue components in records — aligns with the envelope remedy); OQ1 (value-kind peek) still the IDeserializable gate |
| 0020 | Regex | v0.177 | accepted (owner review pending) | **NEEDS-NOTE** | span retirement doesn't touch it (Match is borrow-free by design); re-verify two premises: #1381 flags-enum ops (v0.183 touched enum operators) and the #1363 "no sound cache design" leg (v0.186 `Locked<T>` weakens it; policy leg stands) |
| 0021 | Structured logging | v0.177 | draft | **NEEDS-NOTE** | 0003 graduated — cite §6.11/§30.12; **Fork C (`Locked<ILogger>`) is inexpressible under v0.186's interior admission (sendable *class* only, not interface)** — decline becomes structural; sink loop can use v0.186 `ReceiveAll()` |
| 0022 | Test framework | v0.177 | accepted (owner review pending) | **NEEDS-NOTE** | the `test`-modifier registry's collection scope should be pinned against v0.188 §2.8 (root-compilation/artifact semantics) at spec-amendment time; runner machinery verified stable through v0.187 |
| 0023 | Task.WhenEach | v0.177 | accepted (owner review pending) | **CONFLICTS** | v0.190 (#1786) makes the borrowing form's canonical input idiom ill-formed (`tasks.Add(GetXAsync(...))` — owned temporary into a borrowing slot, abandoned at statement end); input-side ownership story needs redesign. PG-13/PG-03 meanwhile *ground* its await/dispose semantics |
| 0024 | PriorityQueue | v0.177 | draft | CURRENT | v0.194 grounds `Peek`/`Dequeue` as receiver-sourced borrows (no change); proposed §4.19 now taken by `FrozenArray` (v0.186) — renumber at landing (proposal anticipates displacement) |
| 0025 | BitArray | v0.177 | draft | CURRENT | §4.20 renumbers along with 0024; non-generic `IEnumerable<bool>` reasoning intact |
| 0026 | LINQ additions | v0.177 | draft | **NEEDS-NOTE** | MinBy/MaxBy are v0.194 bare-T selectors — graduation text must re-derive their classification against the landed rule; the new callback-taking lazy operators land under the pending #1790 regime (or must join stdlib §5's certified-storing list if they graduate first) |
| 0027 | SplitSpans | v0.177 | accepted (owner review pending) | CURRENT | Range<int>-cursor design orthogonal to the v0.188 span retirement; §29.2 citations intact |
| 0028 | Random extensions | v0.177 | accepted (owner review pending) | CURRENT | sync-span reasoning matches the landed prohibition exactly |
| 0029 | Checksums | v0.177 | draft | **NEEDS-NOTE** | drop the `ReadOnlyAsciiByteSpan` implementor mention (v0.188 pre-registered); non-owning `[32]byte` tail field unaffected by 0036's owning-inline-array demotion; dep 0008 unchanged |
| 0030 | Unicode normalize | v0.177 | accepted (owner review pending) | CURRENT | — |
| 0031 | Verify / Debug | v0.177 | draft | CURRENT | gap-review decline #6 (conditional compilation) it cites is now also normative at §2.8.5 — citation enrichment only |

Header-convention gaps (report, not edits — no *wrong* values found, only missing rows): **0002**,
**0004**, and **0005** carry no "Written against" or "Tracking issue" header rows (README requires
both; tracking is #1402/#1410/#545 per the README index; 0005 carries #545 in prose). 0002's
effective pin is ~v0.179 (it cites the v0.179 async spawn entries). No
`proposals-currency-edits.json` is emitted: the optional edits file is scoped to factual header
*errors* (wrong Status/tracking vs README), and none exist — every Status and every tracking number
that IS present matches the README ledger.

---

## CONFLICTS detail

### 0023 Task.WhenEach — v0.190 "calls mint, storage lends" (#1786) vs the borrowing form's input story

**What contradicts.** D1/§3's borrowing overloads take `tasks: Array<Task<T>>` (elements are
*borrowed* tasks), and the §4 example builds that input as:

```bezel
var tasks := new Array<Task<string>>();
for var url in urls do
  tasks.Add(client.GetStringAsync(url));   // ← ill-formed at v0.190
end;
```

Under v0.190 (#1786), `client.GetStringAsync(url)` **mints an owned task at the call site**;
passing it to `Array<T>.Add`'s plain (borrowing) parameter makes it an unbound owned temporary
consumed only by a borrowing use — which now **abandons (disposes) at end of statement** (v0.190
migration: "an instance task call nested as a plain argument … bind first"). The array is left
holding disposed tasks; v0.193 PG-14 then makes any await of them raise
`InvalidOperationException`. More broadly, a dynamically-sized collection of *borrowed* tasks now
has no owner story at all: every task call result is owned, and there is no dynamic supply of
plain-var owned bindings to source N borrows from. (The stdlib's own `WhenAll(tasks: Array<Task>)`
rows share this seam; #1787's iterable-frame container-ownership question is the adjacent open
axis.)

**What is NOT in conflict — and is newly grounded.** (a) The yielded-element usage
`var content := await t;` over the borrowed iteration variable is now normatively admitted by
v0.193 PG-13 (#1800: borrowed task locals join §30.5 Pattern A). (b) D7's
"tasks not consumed before enumeration ends are disposed by the enumerator during `DisposeAsync`"
is exactly v0.193 PG-03 (#1794: an owning enumerator owns and disposes its pending value). (c) The
affine/§29.11 classification and the deferred-activation borrow-carrying frame (D3) are unchanged.

**Design attention needed before implementation.** Re-derive the input side: either (i) the
borrowing overloads accept the borrowing *view* of an owned-task collection
(`OwningArray<Task<T>>` — `Task<T>` is a class, so the owning container works; v0.190 explicitly
blesses "populating an `OwningArray<OwningTask<T>>` from call results"), with the examples
rewritten accordingly; or (ii) the borrowing overloads are narrowed to the static-fan-out case
(tasks held in named owned locals) and `WhenEachOwned` becomes the primary dynamic form. Whichever
way, the §4 examples as written must not survive graduation, and the resolution should be
coordinated with the identical seam in §6.7's `WhenAll` rows and with #1787.

---

## NEEDS-NOTE details

### 0006 Stream family (R1; wave-D1 root) — three drifted premises

1. **LB-03 is a co-requisite, and §1/Q5's premise is factually wrong at current text.** The
   proposal states "`Stream.Read`/`ReadAsync` already return `int` = bytes read, **0 = EOF**
   (§7.1)" and rests Q5 (no Try shape) and the `CopyToAsync` reference body
   (`if n = 0 then break; end`) on it. Phase1c LB-03 (major): §7.1 has **no return-value contract
   at all** — bytes-read meaning, 0⇔EOF, short-read permission, block-until-one-byte are all
   unstated; the abstract method is unimplementable-to-spec and read loops unwritable. At
   graduation: land LB-03's §7.1 contract first (or in the same train) and re-trace the
   `CopyToAsync` body and `StreamReader`'s incremental-decode contract against the exact wording
   (short reads across UTF-8 boundaries; whether `Read` may return 0 before EOF). Also note §7.5's
   `EndOfStreamException` still has no documented producer — decide whether `StreamReader` stays
   sentinel-only (as designed) and the exception stays producer-less.
2. **The §1 substrate recital lists span-form `ReadAsync`/`WriteAsync` — those no longer exist.**
   v0.179's borrow-bearing-params-on-async prohibition (#1518/#1438) removed the async span
   overloads from §7.1 (v0.184 carries the removal note). The design never depended on them (the
   `CopyToAsync` body uses the windowed form), but the recital and any "both forms" phrasing must
   be corrected. Relatedly, the v0.179 **blocking classification** of the new sync surface
   (`Read`/`ReadLine`/`ReadToEnd`/`Write` over a `FileStream`) must be stated at graduation —
   LB-07 records that `TextReader`/`TextWriter` escaped the v0.179/v0.184 classification sweep;
   don't extend the omission to the concretes.
3. **The Q1 borrowed-stream blocker has landed.** §5/§6 defer the `constructor(scoped stream:
   Stream)` second signature "once the §28.14 spec amendment lands" — it landed in **v0.180
   (#1439)**, in exactly the anticipated shape (class/interface-typed `scoped` parameters,
   `M(scoped x)` call-site marker, fresh-producer rejection; spec §28.14/§22.3). The roadmap
   already records this. At graduation decide explicitly whether v1 still ships owned-only or the
   `scoped` ctor rides along as the second signature (it is a pure addition; the review reason for
   deferral — "not expressible soundly yet" — is gone).
4. Minor: v0.190 is signature-neutral here (unmarked `Task` returns are the announcement; the
   examples' `await`/`using var` usage already assumed caller-owned results). #1790 does not touch
   `ReadLinesAsync`/`EnumerateLines` (no callback parameters).

### 0010 Serialization value builtins (R1) — §9.8 is now two lists

The proposal quotes v0.177's single component-eligibility sentence. v0.182/v0.184 rewrote spec
§9.8: there is now (a) the **general component list** (spec §9.8 *Synthesis-eligibility*, line
~1352 — primitives, `string`, enums, eligible records, synthesized classes, `Shared/Weak`,
collections, with the class-typed rows admissible in class components only) and (b) the **v0.184
record-admissible list** (the record clause, line ~1363 — primitives, `string`, enums, eligible
records, `T?`, non-owning inline arrays). The five value builtins must be added to **both** (they
are value records, so they slot into "another serialization-eligible record" once conformances
exist — but the landing edits must verify both passages plus the §3.6 derived-property row rather
than the v0.177 single sentence). The acceptance test (`EventRecord(Id: Guid, OccurredAt: DateTime,
Name: string)`) exercises exactly the record-admissible path. Verified unchanged: the §2.14 token
vocabulary the proposal enumerates (13 write / 15 read) matches current text, so the
enumerate-then-extend baseline for `WriteBytes`/`ReadBytes`/`TryReadBytes` holds; the conditional-Z
ruling, frozen-vocabulary ruling, and `out owned result: Array<byte>?` (fine under #1797's
class-typed arm) are all intact. v0.193 constructor-selection/case-insensitivity edits (v0.182) in
§9.8 don't touch the five (compiler-provided conformances bypass constructor matching).

### 0008 Integer toolkit (R1, first in order) — anticipate #1803 in the deferred generic surface

The v1 surface is ownership-inert (instance methods on concrete primitives, a static
`BitOperations` class, the immutable-chaining `HashCode` value record) — nothing landed or pending
touches it. The currency issue is the **deferred generic surface** (§3.5, §4.3, §6, declines 4/6):
the future `IBinaryInteger<T>`-style static interface for generic wrapping/bit code, which 0008
insists be "designed against, not bolted onto, `INumber<T>`". The pending #1803 ruling
(Direction O, v0.196 target) re-signs §2.19's static-interface producers `: owned T` under a new
§28.1 per-instantiation **erasure** rule, and its design doc cites 0008's deferral by name. The
note to attach: **when the generic wrapping surface is designed, its fresh-producing bare-T members
(`WrappingMul(a: T, b: T): T` etc.) are producers and must spell `: owned T` under the
v0.196 signature grammar** (bare-T-with-inputs = selector / `owned T` = producer / bare-T-without =
factory), erased at value substitutions — and a heap-backed BigInteger **class** conformer (the
consumer 0008's §1 says doesn't exist) is exactly the case #1803 makes writable. Also: 0008 does
NOT anticipate this in its current text (pin predates the whole bare-T arc). Minor: the two
"`Range<T>` (§3.4)" citations are stale at any reading (Range is stdlib §9.8 / spec §16.3).

### 0001 Http client (R4) — validated more than drifted

- **v0.190 ratifies the design.** `Text(): Task<string>` / `Json<T>(): Task<T>` from instance
  methods were, at the v0.179 letter, receiver-borrows (the contradiction v0.190's
  "calls mint, storage lends" resolved); the proposal always assumed caller-owned fresh tasks —
  now normative, no signature change. The explicit `OwningTask<HttpResponse>` Pattern-B surface is
  untouched.
- **Q6 (span-taking request bodies) is settled by the spec**: v0.179's borrow-bearing-async
  prohibition (now §30.1) forecloses the "consume-before-first-suspension" alternative; the
  `Array<byte>` form is the answer. Close Q6 as resolved-by-spec at next touch.
- **Stale premise**: §3.3's "static interface members are deferred (stdlib §2.14)" — static
  interface members shipped **v0.176** (§11.8); v0.191 already corrected the rationale's copies of
  this claim. §2.14's Deserialize-by-recognized-name convention may still stand, but the *reason*
  given is wrong, and Q1's `Json<T>`/`JsonOwned<T>` unification question should be re-examined
  against §11.8 + the pending #1803 erasure regime (an `IDeserializable` static-factory constraint
  is closer to expressible than the text believes; #1792's Parse-witness gap is the adjacent open
  item).
- Verify at graduation: `Task.WhenAllOwned` cited in §4.2 exists with the expected signature
  (confirmed at v0.194, stdlib §6.7 line ~3379) — but LB-01 shows §6.7's helper rows have drifted
  before; re-check at landing.

### 0002 Http server (R4)

- **Header**: add the missing "Written against" (effectively v0.179) and "Tracking issue"
  ([#1402]) rows at next touch.
- **Dep 0003 graduated (v0.186)**: cite spec §30.12 / stdlib §6.11 instead of "proposal 0003".
  §2.1's `Store.Todos` must be spelled per the v0.186 statics ruling: `static owned readonly
  Locked<…>` (mutable lock statics are rejected; the locks are rule-4 suppression-list members,
  deliberately not form-(i)). The handler bodies trace clean against the landed guard rules (no
  guard held across an await in the examples; `LockAsync` Pattern B matches §6.11's own registry
  example). The should-level guard-across-`await` lint is worth a line in the graduated text since
  handlers are async.
- **The capture-free `routes` check-site needs the v0.187 mechanism**: for "a capturing builder is
  rejected at the `Listen` call" to be compiler-checked, `Listen`'s `routes` parameter must become
  a **stdlib-stated entry in §28.14's capture-free register** (keyed on the resolved callee) — the
  register v0.187 built for `SharedEventSource`/`Domain.Spawn` — which brings the register-wide
  method-reference-formation bar along. This is now a clean, named mechanism rather than a
  Domain.Spawn special case; the graduation text should say so explicitly.
- v0.190 is signature-neutral (`(HttpRequest) -> OwningTask<HttpResponse>` handlers unchanged).

### 0005 Self-host collections (owner-gated, XL) — extend the provenance banner

The existing banner covers the Owning-rename and stale `src/` anchors. Three post-pin additions:

1. **v0.188 §2.8 now normatively governs the prelude design (SH-1).** §2.8.1 defines
   implementation-provided stdlib modules (exempt from artifact presentation, inside
   duplicate-name detection — `module System;` in user code is a build error); §2.8.2 makes the
   artifact interface a semantic requirement with source-text embedding a legitimate format;
   §2.8.3 requires **one runtime identity and one static state per (prelude) type per process,
   including per generic instantiation** (#1513 is the evidence bug). SH-1's loader and the D1
   location decision must be re-derived against §2.8 — it mostly *supports* the design (the
   prelude *is* the implementation-provided-module mechanism) but adds duplicate-name detection
   and one-copy-per-process obligations the 2026-06 text predates.
2. **v0.194/v0.193 checks on the POC bodies.** `Peek(): T` is now formally the receiver-sourced
   bare-T selector row (no change needed); `Pop(): owned T` / `TryPop(out owned item: T)` are fine
   under #1797's bare-T arm given `where T: class`; the grow-loop's bind-restore-pass move dance is
   the PG-08 (#1796) compliant shape — keep it that way in any refresh. Note deliberately: the
   pending #1803 erasure does **not** lift §28.5's generic owned-field rule (recorded liftable,
   explicitly out of scope), so the `where T: class` floor for `OwningBuffer<T>`-backed self-hosted
   collections stands.
3. **Test-plan mechanics**: the "JIT round-trip test" plans predate the JIT→AOT harness migration
   (the JIT harness is being retired); SH-2/SH-3 test plans should target the AOT harness
   (`runAot`/`runAotChecked`).

### 0007 System.Net substrate (R4)

1. **§5.3/Q3 is moot, as its own ruling predicted.** The "inherited span-async overloads" no
   longer exist (v0.179/#1438 removal reached the docs); strike the "inherited, unresolved
   tension" framing at next touch — the `Array<byte>` windowed recommendation is now simply the
   only async shape.
2. **TcpListener's graduation checklist grew (v0.186).** "Compiler-known sendable + internally
   synchronized" now entails explicit membership rulings under §28.13's no-silent-growth clause
   (Shared-payload set: yes/no?) and §22.12 (rule-4 suppression list membership; form-(i) — the
   locks' payload-leak reasoning suggests *not* form-(i)). The §30.14 addition alone is no longer
   the whole classification.
3. **FFI v2 (v0.186 §32) offers an in-language declaration route for the `bz_tls_*` seam** —
   typed-handle C-compatible records make `ctx` vs `conn` confusion a compile error; boundary
   `out` params cover `bz_tls_peer_cert(conn, out der[])`-style entries. Enrichment option only
   (the seam may stay runtime-internal), but the graduation text should acknowledge the choice now
   exists (roadmap R6 already notes FFI v2 "unblocks the runtime-internal routings in 0007").
4. **Re-trace the §5.2 deadline example**: `CancellationTokenSource.WithCancelAfter(…).Token`
   appears inline as an unbound temporary; stdlib §6.1's must-bind `WithCancelAfter` note plus
   v0.193 PG-06's full-expression disposal boundary make the inline form suspect — bind the source
   to a `using var` in the example.
5. `Connect`'s `timeout:` → `TimeoutException` would be the library's first actual producer of
   that type (LB-05 records that none exists today) — a point in favor, worth stating at landing.

### 0009 Base64 + hex (R1)

§1 D5 lists `ReadOnlyAsciiByteSpan` among `IReadOnlySpan<byte>` implementors — retired v0.188; the
v0.188 changelog pre-registered exactly this edit ("Proposals 0009/0029 each drop a one-phrase
implementor mention at their next touch"). Drop the phrase; the surrounding rationale is unchanged
(two implementors remain). Also confirm at landing that proposed §3.5/§3.6 slot numbers still hold
against the post-v0.188 §3 layout (§3.4 is a tombstone, not renumbered — the slots are free; the
proposal already delegates renumbering to the docs editor).

### 0011 Process (R2)

- **v0.190 callee-side rule shapes the multi-wait story**: "may be awaited from more than one
  place" must be implemented as *fresh task per `WaitForExitAsync` call* completing with the
  cached `int` code — never a stored task re-exposed through a method return (newly rejected,
  #1786). The design text already reads that way; make it explicit at graduation.
- **PG-12 (#1799) validates the destructor**: exit-time destructors run on the shutdown thread
  outside any domain, where blocking sync forms are legal — the bounded kill+reap in a destructor
  is on firmer ground than when written.
- **v0.186 FFI v2** unblocks the runtime-internal spawn/pipe routing (roadmap R6 note); no surface
  change.

### 0014 Signal hooks (R2)

The two-stage self-pipe design's foreign-thread reasoning ("the §32 discipline applied where it
actually holds") predates v0.186, which **pinned** the foreign-thread runtime obligations in
§32.10 — including that allocation is available on foreign threads (which the drain thread's
`Send` path needs) and the foreign-thread invocation taxonomy. Re-cite §32.10 at graduation; the
design's substance is confirmed, not contradicted. FFI v2 also gives the `sigaction`/self-pipe
layer an in-language route if wanted. Channel-side semantics (bounded capacity 4, coalescing,
drain-thread-catches-`ChannelFullException`, SIG_DFL restore) are untouched by v0.186–v0.194.

### 0019 JSON DOM (R3)

Re-derive the serialization-integration section against the v0.182/v0.184 §9.8 rewrite:

- The **record clause** (v0.184) now *forbids* records from holding class-typed serialized
  components — which independently confirms 0019's envelope shape (an owning **class** envelope
  with an `owned JsonValue` field is exactly §9.8's stated remedy). Cite the landed text.
- **Class synthesis is now normative** (shape read from field types, component-admissibility
  recursion): whether an envelope class with a hand-written-`ISerializable` `JsonValue` component
  counts as eligible flows through the current recursion wording, not the v0.177 sentence —
  re-verify the §(d) write-only story against it, noting v0.184's recorded decline of
  direction-dependent eligibility for record collection components ("recorded liftable") as the
  adjacent posture.
- Open Question 1 (typed-pull vocabulary has no value-kind peek; vocabulary frozen per 0010) is
  unchanged and remains the gate for `IDeserializable` in v1.

### 0020 Regex (R3, XL)

No span-surface impact: the Match-as-`Range<int>` design is borrow-free by construction, and the
v0.188 retirement removed only `ReadOnlyAsciiByteSpan` (`ReadOnlyStringSpan` §3.3, which §1 leans
on, is untouched). Two premises to re-verify at graduation, neither changing the decisions:
(a) Q7's "flags-enum `|` is inexpressible (#1381 open)" — v0.183 landed enum-operator work
(#1582); confirm #1381's status before repeating the claim (the bool-knob constructor stands
regardless); (b) Q2's no-hidden-cache argument leans on "#1363 … no sound design yet" — v0.186's
`Locked<T>` now *is* a sound guarded-static mechanism, so the decline should rest on its second
leg (hidden cost/eviction policy is un-Bezel), which remains decisive.

### 0021 Structured logging (R3)

- **Fork C is inexpressible as sketched.** `static readonly Log.Default: Locked<ILogger>` fails
  v0.186's interior admission: `Locked<T>`'s `T` must be a **sendable class type**, checked at
  instantiation — an interface-typed interior is rejected. The fork was already declined on cost
  grounds; the record should state the structural rejection too (a workable variant would need a
  sealed sendable concrete logger type as the interior).
- Dep 0003 graduated: cite stdlib §6.11 / spec §30.12. Fork B's rule-4 analysis survives the
  v0.186 restructuring of §22.12 (interface-typed statics remain warned; the suppression list
  covers only the named synchronized types); the rule-2 rejection of a settable `Log.Default`
  likewise stands.
- Topology (b)'s sink loop can now be written over v0.186's `Channel<T>.ReceiveAll()` (the
  sanctioned drain idiom) instead of a `Receive()` loop.
- The §6.8 `Spawn` signature it cites (`(owned A) -> Task, owned arg: A): owned Task`) is verified
  current (v0.190 kept the explicit markers as emphasis).

### 0022 Test framework (R3, L)

- Runner machinery verified stable at v0.194: `scoped` storage-bounded `Add` (form-4 bounding via
  the `using var` runner), method-reference registration, capture-bearing bodies gated on
  declaration order — none disturbed by v0.184's capture/freeze changes or v0.187's formation bar
  (which covers capture-free-*register* positions, not `scoped` storage).
- At the (a)-endpoint spec-amendment: pin the `test`-registry's **collection scope against v0.188
  §2.8** — which artifacts' `test` methods are collected (root compilation only, per the §2.8.4
  entry-point-scope precedent/OQ5?), and how the registry composes with one-copy-per-process
  (§2.8.3). Fork (c) remains blocked on #1398 (R7).
- `Assert.Throws` still needs the v0.182 generic-catch rule + #1505 fix (roadmap note stands).

### 0026 LINQ additions (R3)

- **MinBy/MaxBy are v0.194 selectors.** `MinBy<TKey>(key: (T) -> TKey): T` — bare-T return whose
  potential source set is `{receiver}` (the v0.194 Aggregate precedent — sources {receiver, seed},
  the *function-typed* fold excluded — shows selector callbacks stay out of the source set;
  otherwise every lambda-literal argument would trip the temporary-source rejection). The
  graduation text must state the classification (borrow of the receiver enumerable, no factory
  warning) and verify the `T?`-returning `OrNull` forms ride the rule's nullability handling.
  Design intent already matches; this is derivation work, not redesign.
- **The indexed `Select`/`Where` (callback-retaining lazy operators) land under whichever #1790
  state holds at graduation**: post-#1790 (v0.196), the plain signatures are the normative
  instance of frame-residence rule (a) and nothing extra is needed; pre-#1790, stdlib §5's
  *certified storing signatures* list is enumerated — the new operators must be added to it
  explicitly or their capture admission is unstated.
- `Chunk`'s eager/owned reasoning and the #1334/#29 deferrals are unchanged.

### 0029 Checksums (R3)

Same pre-registered v0.188 edit as 0009: drop `ReadOnlyAsciiByteSpan` from the §4.1 input list.
The `XxHash64Hasher` record's `[32]byte` tail is a **non-owning** inline array — unaffected by
0036's demotion of *owning* inline arrays to Extended. Dep 0008 (wrapping ops) unchanged; the
immutable-chaining record shape tracks 0008's accepted ruling.

---

## Cross-cutting observations (for the coordinator)

1. **The v0.188 pre-registered edits (0009/0029 AsciiByteSpan mentions) are still outstanding** —
   both proposals carry the phrase; the changelog's "at their next touch" has not yet occurred.
2. **v0.190 is net-validating for the async proposals** (0001/0006/0007/0011/0015/0018 designed
   caller-owned task results before the spec said so) with exactly one casualty: 0023's
   borrowing-input idiom (the CONFLICTS above). Any implementation run touching §6.7 helpers
   should handle 0023 and the `WhenAll` input seam together.
3. **v0.186's graduations quietly moved three proposals' citation targets** from "proposal
   0003/0032/0033" to normative text (0002, 0021, 0007, 0011, 0014); none of the five graduated
   simplification RFCs (0035–0039) is load-bearing for any live proposal (grep-verified: no event
   members, no class-source `with` blocks, no §26.5.1 reliance anywhere in the live set).
4. **Section-number collisions from v0.186**: proposed stdlib §4.19 (0024) is now `FrozenArray`;
   0024/0025 renumber mechanically at landing.
5. **Header hygiene**: 0002/0004/0005 lack the README-required "Written against"/"Tracking issue"
   rows — worth adding at each file's next touch (not emitted as edits per this lane's scope).
