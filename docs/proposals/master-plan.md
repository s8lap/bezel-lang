# All-proposals master implementation plan + dependency graph

*Capstone deliverable of the 2026-07-11 spec-quality run. Dependency-first view over **all 39** numbered proposals (0001–0039). Complements — does not replace — [`implementation-roadmap-2026-07.md`](implementation-roadmap-2026-07.md), which sequences the same work as R0→R8 run-shapes. This document adds the explicit **proposal-to-proposal DAG** the roadmap does not carry.*

*Sources: `docs/proposals/README.md` (status ground truth, per spec-change-process §8), `phase8-interactions.json` (the 37-item verified interaction map — primary dependency data), `phase8-scoreboard.json` (issue state), and the headers of the ten most-entangled proposals. Written against docs **v0.185**; compiler implements ~v0.177 + partial. Do not treat any proposal as implemented: **none have graduated** (none are `superseded`); all 39 are still design documents.*

---

## 1. Status ledger

Implementation state is **`none` for all 39** — verified: not one proposal file carries `Status: superseded`, and the roadmap's R1–R8 are all still "to build." What *has* partially landed is the **R0 substrate** (spec rulings v0.179–v0.185 that some proposals name as prerequisites — e.g. 0006's scoped-construction landed v0.180, 0031's throw-operand rule landed v0.182); those are compiler-catch-up items, not proposal implementations. The "Prereq landed" column below flags proposals whose *blocking spec ruling* is already in the docs, so implementation can begin the moment the proposal is picked up.

### 1a. Accepted (delegated) — owner-blessed, schedulable now (15)

| # | Title | Status | Tracking | Impl | Prereq landed? |
|---|---|---|---|---|---|
| 0003 | `Locked<T>` / `RwLocked<T>` | accepted (delegated) | #1404 | none | statics v0.164; #1391 static-init sync open |
| 0006 | Concrete stream family | accepted (delegated) | #1433 | none | **yes** (v0.180 scoped-construction) |
| 0007 | System.Net substrate | accepted (delegated) | #1436 | none | runtime event loop + OpenSSL owner ruling pending |
| 0008 | Integer toolkit | accepted (delegated) | #1437 | none | **yes** (no deps) |
| 0009 | Base64 + hex | accepted (delegated) | #1434 | none | **yes** (no deps) |
| 0011 | Process | accepted (delegated) | #1440 | none | needs 0006; kill-on-dispose owner Q |
| 0012 | DateTimeOffset + TimeZoneInfo | accepted (delegated) | #1445 | none | needs 0010; §9.6-override owner Q |
| 0013 | Environment surface | accepted (delegated) | #1441 | none | **yes** (no deps) |
| 0018 | Async File family | accepted (delegated) | #1447 | none | **yes** (independent of 0006) |
| 0020 | Regex **[RFC]** | accepted (delegated) | #1449 | none | engine-family owner Q (RE2) |
| 0022 | Test framework **[RFC]** | accepted (delegated) | #1453 | none | Assert lib **yes**; `test`-modifier fork owner Q |
| 0023 | Task.WhenEach | accepted (delegated) | #1450 | none | **yes** (v0.179 deferred-activation) |
| 0027 | SplitSpans | accepted (delegated) | #1458 | none | **yes** (no deps) |
| 0028 | Random extensions | accepted (delegated) | #1459 | none | **yes** (no deps) |
| 0030 | Unicode normalization | accepted (delegated) | #1455 | none | **yes** (no deps) |

### 1b. Draft — needs a routine owner accept before graduation (16)

| # | Title | Status | Tracking | Impl | Note |
|---|---|---|---|---|---|
| 0001 | Networking / `Http` client | draft | #1376 | none | hard dep 0007 |
| 0002 | `Http` server | draft | #1402 | none | hard dep 0001 |
| 0005 | Self-hosting stdlib collections | draft (stale) | #545 | none | XL; multi-run; refresh banner |
| 0010 | Serialization value builtins + WriteBytes | draft | #1435 | none | hard dep 0009 |
| 0014 | Signal hooks | draft | #1442 | none | runtime drain thread |
| 0015 | PeriodicTimer | draft | #1443 | none | #1378 closed |
| 0016 | Monotonic time | draft | #1444 | none | no deps |
| 0017 | TempFile + Path/Directory | draft | #1446 | none | no deps |
| 0019 | JSON DOM | draft | #1461 | none | hard dep 0010; +0006 streaming |
| 0021 | Structured logging | draft | #1460 | none | hard dep 0006 |
| 0024 | PriorityQueue | draft | #1451 | none | no deps |
| 0025 | BitArray | draft | #1452 | none | soft dep 0008 (PopCount, severable) |
| 0026 | LINQ additions | draft | #1457 | none | no deps |
| 0029 | Checksums | draft | #1454 | none | hard dep 0008 |
| 0031 | Verify / Unreachable / Debug | draft | #1456 | none | v0.182 throw-operand landed |
| 0033 | FFI v2 | draft | #1516 | none | §32 amendment train + layout owner ruling |
| 0034 | Module identity | draft | #1517 | none | spec-only; §2.8 owner rulings + #1398 |
| 0032 | Parallel extensions | draft | #1510 | none | dep 0003; Stage 1.5/2 owner rulings |

*(0004 lives in 1d; count above is the draft build-track set.)*

### 1c. RFC — owner-gated design changes; cannot be scheduled until the owner decides the fork (5)

| # | Title | Status | Tracking | The fork the owner must decide |
|---|---|---|---|---|
| 0035 | Demote the `event` member kind | draft (RFC) | #1550 | delete §24 vs. keep-as-expansion; static fork engages #1647 rule-5 alias gap |
| 0036 | Demote owning inline arrays to Extended | draft (RFC) | #1551 | conformance re-partition; pairs with #1385 either way |
| 0037 | Remove object-initializer blocks | draft (RFC) | #1648 | retire `new T(args) with…end` on classes (this run) |
| 0038 | Retire `ReadOnlyAsciiByteSpan` | draft (RFC) | #1649 | delete the 6th span type (this run) |
| 0039 | One specificity rule for extensions | draft (RFC) | #1650 | delete §26.5.1 rules 2–3 (this run) |

### 1d. Not scheduled (owner-declared out of the near-term plan) (3)

| # | Title | Status | Tracking | Why |
|---|---|---|---|---|
| 0004 | Bezel MVU | early draft | #1410 | XL; explicitly unscheduled (proposal-only by owner decision) |
| 0005 | Self-hosting collections | draft (stale) | #545 | XL; multi-run; listed in 1b for completeness |

---

## 2. The dependency graph (the heart)

Edges are derived from `phase8-interactions.json` and cross-checked against the ten most-entangled proposal headers. Two edge classes:

- **HARD** (→): X cannot be implemented before Y exists (Y's types/tokens appear in X's surface).
- **SOFT** (⇢): X shares surface with Y; land together to avoid rework, but neither strictly blocks the other.

Landed-spec-ruling prerequisites (v0.179–v0.185), runtime substrate work, and owner decisions are **not** proposal edges — they are annotated separately because they gate *scheduling*, not *ordering among proposals*.

### 2a. Per-proposal edges

| Proposal | HARD depends on | SOFT shares-surface-with | Blocks (downstream) |
|---|---|---|---|
| 0006 stream family | — *(v0.180 landed)* | 0018, 0029, 0001, 0019, 0021, 0032(naming) | **0011, 0021, 0019, 0001, 0002, 0029** |
| 0008 integer toolkit | — | 0022, 0025, 0029, §2.19 INumber | **0029, 0025** |
| 0009 base64/hex | — | 0027 (§3.5 numbering) | **0010** |
| 0010 serialization | **0009** | 0012, 0019, 0021 | **0012, 0019** |
| 0007 net substrate | — *(runtime+OpenSSL)* | 0002, 0011, 0033(§9) | **0001** → 0002 |
| 0001 http client | **0007**, 0006(body) | 0002 (one API) | **0002** |
| 0002 http server | **0001** | 0007, 0006, 0003, 0014, 0021 | 0021(as consumer) |
| 0011 process | **0006** | 0007, 0013, 0014, 0033(§9) | — |
| 0012 datetimeoffset | **0010** | 0016 (temporal cluster) | — |
| 0019 json dom | **0010**, 0006(streaming) | 0021, 0004 (owned-tree) | — |
| 0021 logging | **0006** (file sink) | 0010, 0014, 0002(consumer), 0003 | — |
| 0025 bitarray | 0008 (soft/severable — PopCount) | 0024 (numbering) | — |
| 0029 checksums | **0008** | 0006(stream forms), 0008(accumulator) | — |
| 0032 parallel | **0003**-as-amended | 0006(naming), TaskScope | — |
| 0033 ffi v2 | §32 amendment train *(unlanded)* | 0007/0011/0014(§9), 0034(symbol seam) | 0007/0011/0014 runtime routings |
| 0034 module identity | evidence bugs + #1398 | 0033 (linking/symbol seam), #1365 | 0033(shared seam) |
| 0003 locked | — *(#1391 static-init)* | 0032, 0002, 0021 | **0032** |
| 0014 signal hooks | — *(runtime drain)* | 0011, 0002, 0021, 0033 | 0002/0021(soft) |
| 0015 periodic timer | — *(#1378 closed)* | 0016, TaskScope | — |
| 0016 monotonic time | — | 0012, 0015 (temporal) | — |
| 0020 regex | — *(#1381 landed)* | 0022, 0027, 0030(Unicode tables) | — |
| 0022 test framework | — *(v0.182+#1505)* | 0031, 0008, #1398(fork c) | — |
| 0023 wheneach | — *(v0.179)* | TaskScope, 0032 (fan-in) | — |
| 0031 verify/debug | — *(v0.182)* | 0022 (Assert/Verify) | — |
| 0013, 0017, 0024, 0026, 0027, 0028, 0030, 0018 | — | numbering/adjacency only | — |

Standalone leaves (no proposal edges in *or* out): **0013, 0016, 0017, 0024, 0026, 0027, 0028, 0030** — pure additive utilities, schedulable in any order.

### 2b. Topological build order (layers)

**Layer 0 — zero HARD proposal-dependency; can start immediately** (some gated on landed rulings / runtime / an owner sub-question, annotated):

- **0006 stream family** ← *the hub; highest fan-out (blocks 5)*. Ruling landed.
- **0008 integer toolkit** ← blocks 0029, 0025.
- **0009 base64/hex** ← blocks 0010.
- **0003 locked** ← blocks 0032 (needs #1391 static-init sync to be airtight).
- 0013, 0016, 0017, 0024, 0026, 0027, 0028, 0030 — additive leaves.
- 0018 async file — *deliberately independent of 0006* (interaction map: "none hard").
- 0023 wheneach, 0031 verify, 0015 periodic timer — landed-ruling-gated only.
- 0014 signal hooks — runtime drain thread.
- 0007 net substrate — **gated on runtime event loop + OpenSSL owner ruling** (no proposal dep, but heavy substrate).
- 0020 regex — engine-family owner ruling (XL).
- 0022 test framework — Assert lib now; discovery fork owner Q.
- 0033 ffi v2 — **gated on the six §32 amendments landing first** + Stage-B layout owner ruling.
- 0034 module identity — spec-only; **gated on owner §2.8 rulings + #1398 build driver**.

**Layer 1 — one HARD proposal edge:**

- **0010 serialization** → 0009
- **0011 process** → 0006
- **0029 checksums** → 0008
- **0025 bitarray** → 0008 (severable)
- **0001 http client** → 0007 (+0006)
- **0021 logging** → 0006
- **0032 parallel** (Stage 0/1) → 0003

**Layer 2 — two-deep:**

- **0012 datetimeoffset** → 0010 → 0009
- **0019 json dom** → 0010 → 0009 (+0006)
- **0002 http server** → 0001 → 0007
- 0032 Stage 1.5 / Stage 2 → owner ratifications (design-gated, not a build layer)

**Layer 3 — consumer co-land:** 0021 is *consumed by* 0002 (its "first consumer"); 0021 itself is Layer 1 and buildable earlier, but the end-to-end logging-in-the-server scenario is not exercisable until 0002 (Layer 2) exists.

---

## 3. Critical path

The longest, heaviest dependency chain — and the one gating the most downstream value — is the **networking stack**:

```
runtime nonblocking event loop + OpenSSL TLS seam   (runtime + owner ruling)
        │
        ▼
   0007 System.Net substrate  (L)
        │
        ▼
   0001 Http client           (L)   ← also draws 0006 for body streaming
        │
        ▼
   0002 Http server           (L)   ← also draws 0003, 0014, 0006
        │
        ▼
   0021 logging as the server's first consumer  (end-to-end scenario)
```

Three Large proposals in strict series, sitting on top of the run's biggest unbuilt runtime substrate. Nothing shortens it: 0001 cannot start until 0007's socket/TLS types exist; 0002 cannot start until 0001's `HttpResponse`/`HttpHeaders`/error roots exist.

A **second, lighter critical chain** runs three-deep through serialization: `0009 → 0010 → {0012, 0019}`. It is all S/M work and fully parallel to networking.

**What parallelizes off the critical path:** essentially the entire stdlib. While the networking chain is in flight, Layer 0 + Layer 1 of the two serialization/integer chains and all eight additive leaves proceed independently. Concretely, three teams can run at once with no contention:
1. **Networking team**: runtime loop → 0007 → 0001 → 0002 (the critical path).
2. **Foundations team**: 0006 (hub) + 0008 + 0009, then 0010/0011/0029/0025/0012/0019.
3. **Utilities team**: 0013/0016/0017/0024/0026/0027/0028/0030/0018/0023/0031/0015/0014 — the leaf batch, 2–3 per docs graduation.

The critical path length, not total work, is what bounds calendar time; keep 0007's runtime substrate staffed first.

---

## 4. Owner-decision gates (the review queue projected onto the plan)

Per spec-change-process §8, the README `Status` column **is** the owner-review queue. Projected onto scheduling, three tiers:

### 4a. Hard gates — cannot be scheduled at all until the owner accepts (the 5 RFCs)

These are **design changes** (§2 of the process): they need a proposal→accept→graduation, not a routine landing. All are deletion/demotion-shaped (mostly doc-only, small conformance-corpus deltas):

- **0035** event-member demotion (#1550) — decide delete-§24 vs. keep-as-expansion; the static fork proposes a new `SharedEventSource<T>` and engages the **live #1647 rule-5 alias gap** head-on. Precondition #1536 closed.
- **0036** owning-inline-array demotion (#1551) — decide the Core/Extended re-partition; resolve **#1385** (whole-value reassignment) either way.
- **0037** remove object-initializer blocks (#1648) — decide retiring `new T(args) with…end` on classes; ~23/27 test sites are class-source (conformance-corpus checklist owed).
- **0038** retire `ReadOnlyAsciiByteSpan` (#1649) — decide deleting the 6th span type; no migration (compiler never implemented it, #971).
- **0039** one specificity rule for extensions (#1650) — decide deleting §26.5.1 rules 2–3; zero stdlib sites resolve through them.

### 4b. Embedded sub-question gates — schedulable, but one owner decision gates *part* of the work

The proposal is accepted/draftable and most of it can proceed, but a load-bearing sub-decision blocks a specific piece:

- **0007** — **OpenSSL 3.x TLS-seam ruling**: the sockets/`Dns`/`TcpListener` half proceeds; `TlsClientStream` waits on the backend ruling.
- **0011** — **kill-on-dispose**: the "disposal SIGKILLs+reaps" semantics is the load-bearing owner question; the rest of the surface is settled.
- **0012** — **instant-order equality overriding §9.6 structural** (first stdlib record to do so) + the full-offset wire form.
- **0020** — **engine family (RE2) + `Match` shape** (RFC): the whole proposal is gated on these two.
- **0022** — **discovery fork** (the `test` modifier, RFC): the `Assert` library + `using var` fixtures ship under any fork; only the discovery endpoint waits.
- **0032** — **Stage 1.5** needs the §28.13 definitional-widening ratification (q4); **Stage 2** needs owner rulings q5–q9. Stage 0/1 need none. Also: **the 0003 amendments 0032 relies on are not yet landed in 0003's file** — a prerequisite sub-task.
- **0033** — **Stage B layout-promise ratification** (q4, the language's first normative layout promise) + the six §32 amendments must land as spec first.
- **0034** — the four **§2.8 rulings** (identity/closure, artifact interface, transitive deps, program assembly) are a spec-only design change needing owner sign-off; plus the v0.181 entry-point ruling needs rebase (PX-03).
- **0010** — mechanism choice (Option A, compiler-provided conformances, recommended) awaits owner accept as part of graduating this draft.

### 4c. Routine-accept gates — the plain drafts

Every proposal in §1b is `draft` and therefore technically on the owner's accept queue, but these are low-risk stdlib additions the roadmap already treats as accepted-in-spirit. They need a routine accept, not a design decision: 0001, 0002, 0005, 0010, 0014, 0015, 0016, 0017, 0019, 0021, 0024, 0025, 0026, 0029, 0031.

### 4d. Confirm-delegated

The 15 `accepted (delegated)` proposals (§1a) carry "owner review pending" in their headers. They are blessed to schedule; the pending review is a confirmation, not a scheduling block.

---

## 5. Reconciliation with the existing roadmap (deltas only)

The roadmap's R0→R8 sequence and this DAG **agree** on the big structure: R0 soundness first; D1 foundations (0008/0009/0010/0006) before D2/D3 consumers; networking as R4 in the order 0007→0001→0002; 0032 as R5, FFI as R6, modules as R7, RFCs as R8 owner-gated. The roadmap already records 0012→0010, 0011→0006, 0029→0008, 0019→0006. Four deltas the dependency view surfaces:

1. **0006 should start FIRST in R1, not last.** R1 lists the order `0008 → 0009 → 0010 → 0006`, but by the DAG **0006 has zero proposal-dependencies and the highest fan-out (blocks 0011, 0021, 0019, 0029-streams, 0001, 0002)**. Sequencing it last delays every consumer. 0006, 0008, 0009 are all Layer 0 and should run in parallel at the front of R1; only 0010 is genuinely sequenced (after 0009). **Recommendation: pull 0006 to the front of R1 (or a dedicated early lane).**

2. **0032 → 0003 is a proposal-to-proposal edge the roadmap does not reflect.** R5 describes 0032's dependencies as internal stages + owner rulings q4–q9, but the interaction map records that **0032 hard-depends on `0003`-as-amended** (Locked/RwLocked joins the synchronized set 0032 Stage 1.5 widens; per #1404 comment). Moreover **those 0003 amendments are not yet landed in 0003's file** — a prerequisite sub-task R5 omits. **Recommendation: add "0003 (amended)" as an explicit R5 predecessor, and file/track the 0003-amendment sub-task.**

3. **0018 async-file is deliberately independent of 0006.** R2 groups 0018 after the D1 streams and the hub narrative implies it needs 0006, but the interaction map is explicit: **"none hard (deliberately independent of 0006)"** — 0018 only *shares surface* via the optional StreamReader encoding path. It can land in Layer 0 alongside the leaves, before 0006. Minor, but it frees 0018 from the D1 gate.

4. **0021 logging's dependencies span waves; its end-to-end scenario can't be exercised until R4.** R3 places 0021, but 0021's stated "first consumer" is **0002 (R4)**. 0021 is buildable at Layer 1 (needs only 0006), but the http-server-logging scenario that validates it is a Layer 2 / R4 event. **Recommendation: build 0021 in R3 as planned, but schedule its integration test with 0002 in R4.**

No delta contradicts the roadmap's wave assignment; these refine intra-wave order and surface two under-recorded edges (0032→0003, and 0006's hub priority).

---

## 6. This run's additions (2026-07-11)

- **v0.184 (ownership-coherence) + v0.185 (stdlib + currency sweep) landed**, taking the docs from v0.183 → v0.185. These bumped the normative baseline the compiler must catch up to.
- **~52 new `#16xx` tracking issues** were filed for the v0.184/v0.185 spec-ahead-of-impl deltas (per `phase8-scoreboard.json`; `total_open_issues` now 187). These are **R0 (soundness catch-up) work** and, like the earlier v0.179–v0.183 trackers, **precede all proposal implementation** — the compiler must implement the landed rulings before the stdlib waves are meaningful. (Residual older R0 opens are small: v0.179 {#1523, #1525, #1527, #1531}, v0.180 {#1538}, v0.181 {#1565}, v0.182 {#1571}, v0.183 none.)
- **Three new RFCs added (owner-gated): 0037, 0038, 0039** — all simplification/deletion design-changes (remove object-initializer blocks; retire `ReadOnlyAsciiByteSpan`; prune extension-specificity rules 2–3). They join 0035/0036 as the R8 owner-review batch (§4a). Each carries a conformance-corpus checklist per spec-change-process §6 (the systematic blind spot the 0037–0039 batch surfaced).
- **0035 was deepened** (owner-directed) to a two-line existing-mechanism floor + a sealed `ISharedEvent<out T>`/`SharedEventSource<T>` static pair engaging the live #1647 rule-5 alias gap.

**Net effect on the plan:** the critical path's true head is not 0007 but **R0** — the ~52 #16xx items plus the residual v0.179–v0.182 opens. Until R0 is discharged the spec is normative and the compiler is behind; every proposal in Layers 0–3 assumes the R0 rulings are implemented. Sequence R0 first, then the three parallel teams of §3.
