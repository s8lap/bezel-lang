# All-proposals master implementation plan + dependency graph

*Capstone deliverable of the 2026-07-11 spec-quality run. Dependency-first view over **all 39** numbered proposals (0001–0039). Complements — does not replace — [`implementation-roadmap-2026-07.md`](implementation-roadmap-2026-07.md), which sequences the same work as R0→R8 run-shapes. This document adds the explicit **proposal-to-proposal DAG** the roadmap does not carry.*

*Sources: `docs/proposals/README.md` (status ground truth, per spec-change-process §8), `phase8-interactions.json` (the 37-item verified interaction map — primary dependency data), `phase8-scoreboard.json` (issue state), and the headers of the ten most-entangled proposals. Written against docs **v0.185**; **refreshed 2026-07-12** for the graduation run (docs now **v0.188**, main `ebfa7e0`). **Nine proposals have now graduated into the normative docs** (`superseded`): 0003/0032/0033 (v0.186), 0035 (v0.187), 0034/0036/0037/0038/0039 (v0.188) — see §1's graduated ledger. The graduation is docs-only; no *implementation* has landed yet, so the R1–R8 build tracks are unchanged in size — but the nine graduated proposals are now spec-complete and their remaining work is pure compiler catch-up (impl trackers #1705–#1737).*

*Currency note 2026-07-14: docs are now **v0.191** (defect-sweep revisions v0.189–v0.191, main `1469b21`); no proposal status changed — the delta is six new R0-layer compiler trackers (#1781–#1786, headlined by the §28.3 Task-family ownership ruling, #1786) and one new design-review item (#1787, iterable-frame ownership). The roadmap's 2026-07-14 refresh note carries the detail; this plan's dependency graph is unaffected.*

*Currency note 2026-08-11: docs are now **v0.241** (this note's coverage horizon; the in-flight elegance-graduation program, v0.242+ / proposals 0078–0090, is deliberately not covered — its ledger rows land with the program-close banner: **[ELEGANCE-PROGRAM LEDGER — appended at program close]**). The gap since 2026-07-14 is large and this plan's ledger no longer describes the proposal population: the 2026-07-18 future-directions program added **0041–0054** (plus the separately-filed **0040** build-system sketch), the later trains added **0055–0077**, and twenty-nine proposals graduated in the interval, several partially (0044/0045/0051 Part A; 0061 §1; 0075 stages 1–2; 0076's criterial rule; 0077 stage 1) — the v0.198–v0.204 batch (0041–0046, 0048–0051 Part A; recorded in the roadmap's 2026-07-19/20 banners) plus the v0.205–v0.238 batch (0023, 0055–0059, 0061–0067, 0072–0077; recorded in the roadmap's 2026-08-11 banner). §1·Δ below carries the bucket-by-bucket deltas. Two standing §1 claims are corrected by the deltas: "no proposal is implemented yet" is no longer true (the 2026-07-20 impl run landed sixteen features of the v0.198–v0.204 layer — see the roadmap's banner), and the header's "all 39 numbered proposals (0001–0039)" now describes only this document's original DAG scope, not the ledger population. The dependency graph of §2 is unchanged for the 0001–0039 set; the new drafts' edges are recorded in §1·Δ(d) rather than merged into §2a's table. The 2026-07-28 proposals-syntax-currency sweep modernized retired spellings across all non-superseded proposals (38 sites, 15 files) — read it before picking up any pre-v0.217 draft.*

---

## 1. Status ledger

**Nine proposals have graduated (`Status: superseded`) — spec-complete, compiler catch-up outstanding.** No proposal is *implemented* yet (the R1–R8 build tracks are all still "to build"), but the graduation run landed the spec/stdlib side of nine designs into v0.186–v0.188. Beyond that, the **R0 substrate** partially landed (spec rulings v0.179–v0.185 that some proposals name as prerequisites — e.g. 0006's scoped-construction landed v0.180, 0031's throw-operand rule landed v0.182); those are compiler-catch-up items too. The "Prereq landed" column below flags draft proposals whose *blocking spec ruling* is already in the docs, so implementation can begin the moment the proposal is picked up.

### 1·0. Graduated (superseded) — spec landed, compiler catch-up tracked (9)

| # | Title | Graduated | Impl trackers (compiler catch-up) |
|---|---|---|---|
| 0003 | `Locked<T>` / `RwLocked<T>` | v0.186 (`3d08ec2`) | #1721 |
| 0032 | Parallel extensions | v0.186 (`3d08ec2`) | #1722 / #1723 / #1724; Stage-2 gate #1725; consume-site #1726 |
| 0033 | FFI v2 (+ `env` instance-callback extension) | v0.186 (`3d08ec2`) | #1705–#1720 |
| 0035 | Retire the `event` member kind | v0.187 (`7024955`) | #1727 / #1728 (needs #1646 first); #1704 liftable; #1647 closed |
| 0034 | Module identity + program assembly | v0.188 (`ebfa7e0`) | #1729–#1734 (+ #1398 build driver) |
| 0036 | Demote owning inline arrays to Extended | v0.188 (`ebfa7e0`) | #1737 (conformance staging); #1385 stays open |
| 0037 | Remove object-initializer blocks | v0.188 (`ebfa7e0`) | #1735 |
| 0038 | Retire `ReadOnlyAsciiByteSpan` | v0.188 (`ebfa7e0`) | #1736 |
| 0039 | One specificity rule for extensions | v0.188 (`ebfa7e0`) | #1307 (re-scoped) |

### 1·Δ. Ledger deltas 2026-07-14 → 2026-08-11 (docs v0.191 → v0.241)

**(a) Graduated (`superseded`) since this plan's last refresh — 29 proposals (10 in the v0.198–v0.204 batch + 19 in v0.205–v0.238).** The v0.198–v0.204 batch is summarized (the roadmap's 2026-07-19 banner is its detailed record); the v0.205+ batch is tabled in full. Every graduation is docs-only at landing; the impl-tracker column is the compiler catch-up.

*v0.198–v0.204 (the future-directions graduation run, 2026-07-19):* **0046** ownership kernel → v0.198/v0.199 (zero compiler work; escalated rulings #2042/#2043 — both since ruled at v0.223); **0041** `choice` → v0.200 (#2044–#2047); **0048**/**0049**/**0050** → v0.201 (#2048–#2051); **0042** `pure` → v0.202 (#2052); **0043** contracts → v0.203 (#2053); **0044** spec deltas + **0045** normative deltas + **0051 Part A** → v0.204 (#2054/#2055). Substantially implemented by the 2026-07-20 run.

*v0.205–v0.238:*

| # | Proposal | Graduated | Impl trackers (compiler catch-up) |
|---|---|---|---|
| 0023 | Task.WhenEach — streaming fan-in family | v0.212 | tracker filed at landing (v0.212 entry); input seam #1813 resolved-and-closed v0.222 (#1881/#1655 remain) |
| 0055 | Parenthesized contract clauses | v0.207 | the #2053 slice-1 parser lane (#2070 superseded) |
| 0056 | Arm-position transfer marker | v0.209 | tracker filed at landing (v0.209 entry) |
| 0057 | Paren-delimited expression forms | v0.208 | tracker filed at landing; corpus migrates with the parser |
| 0058 | Readonly-field narrowing | v0.214 | trackers filed at landing (v0.214 entry) |
| 0059 | Uniform trailing commas | v0.214 | trackers filed at landing (v0.214 entry) |
| 0061 | Expression-form studies — **§1 only** (binding atom) | v0.216 | #2098; §2 stays an evidence-gated study |
| 0062 | Small-stdlib ergonomics batch | v0.215 | trackers filed at landing; #2089 closed |
| 0063 | Fluent-discard exemption | v0.215 | trackers filed at landing (v0.215 entry) |
| 0064 | Keyword-led lambdas | v0.217 | #2101 (+#2102 adjacency, closed v0.220) |
| 0065 | Named-destructure patterns | v0.224 | tracker filed at landing, sequenced with the #2079 syntax family (#1409 was the design tracker) |
| 0066 | Method-local type declarations | v0.225 | tracker filed at landing (#1405 was the design tracker) |
| 0067 | Small-stdlib ergonomics batch II | v0.228 | #2242 |
| 0072 | Nullable-return convenience tier | v0.231 | #2347 |
| 0073 | One-shot serialization facade | v0.232 | #2349 (+#2352 piggyback pin; rides #2337) |
| 0074 | Small-stdlib ergonomics batch III (item 4 landed v0.230) | v0.231 | #2348 — carries the one BREAKING item, the `Environment` rename |
| 0075 | Result ergonomics — **stages 1–2** | v0.233 | #2354/#2355; stage 3 = recorded deferrals, R5 gate re-armed |
| 0076 | Error-model study — criterial rule; R1–R6 ruled 2026-07-29 | v0.229 | #2337 (SerializationException surface); #2338 (graduation obligations) |
| 0077 | Silent tracking — **stage 1** | v0.238 | stage 2 = #2654/#2655/#2656/#2660 (landed ahead per Q4) + #2676 behind #2672 |

**(b) New accepted (owner-review batch 2026-07-24, delegated rulings).** **0002** Http server — accepted (was draft; §1b's row and §2's Layer-2 gating unchanged in content); **0054** traceability — accepted, phase 1. Same batch discharged two §4b embedded sub-question gates: **0012**'s v1 scope + equality co-declaration ruled (#1445) and **0020**'s engine family ruled FINAL (#1449) — those two rows of §4b are done in their named parts (0020's `Match` shape is not recorded as ruled).

**(c) New drafts / in-set status flips.** New drafts absent from every §1 bucket: **0040** build system (draft sketch, tooling-only; #1908 — the #1398 SCC kernel is its Stage 0); **0047** mechanized core semantics (draft; #2034); **0052** compiler diagnostics + query API (draft; #2039); **0053** property-based specification (draft; #2040); **0060** channel give-back protocol (draft consolidation stub, created v0.210 — makes the previously-phantom "give-back-protocol RFC" citations real; no design decided); **0068** cryptographic primitives (draft); **0069** compression: gzip + DEFLATE (draft); **0070** first-class `Uri` (draft); **0071** file metadata `FileStatus`/`File.GetStatus` (draft). In-set flips: **0004** MVU refreshed to v0.216 (still early draft, unscheduled); **0005** loses its "(stale)" qualifier in the README (still draft, XL); **0026** gained the 2026-07-28 `TakeLast`/`SkipLast` rider. §1c stays empty; §1·0's original nine rows are unchanged.

**(d) New proposal-to-proposal dependencies declared by the new proposals** (README rows; not merged into §2a — the 0001–0039 DAG is unchanged):

| Proposal | Declares |
|---|---|
| 0068 crypto | the **0007 OpenSSL-seam linkage** is its load-bearing open question; accumulators follow the 0008/0029 chaining shape; `RandomNumberGenerator` is a replay intercept point (0045 seam) |
| 0069 compression | **hard: 0006** (stream wrappers over an owned inner stream); one-shots per **0009**'s shape; system-zlib is the owner question |
| 0070 Uri | additive over **0001/0002** — their string-URL v1 surfaces stay frozen; no build-order edge |
| 0071 file metadata | **0012/0016** bridges recorded for `ModifiedAt`; **zero overlap with 0017** (seam stated) |
| 0053 property testing | parameterized **0022** test methods; **0041**-aware generators; exhaustive mode gated on **0042** purity; **0045** seed/journal determinism; the **0043** contract bridge |
| 0047 Lean model | models **0046**'s kernel (K1–K9) |
| 0060 give-back stub | consolidates the four deferred channel features (stdlib §6.9 material); no edges — nothing decided |

### 1a. Accepted (delegated) — owner-blessed, schedulable now (14)

*(0003 graduated — see §1·0.)*

| # | Title | Status | Tracking | Impl | Prereq landed? |
|---|---|---|---|---|---|
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

### 1b. Draft — needs a routine owner accept before graduation (13)

*(0032, 0033, 0034 graduated — see §1·0.)*

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

*(0004 lives in 1d; count above is the draft build-track set.)*

### 1c. RFC — owner-gated design changes (0 — all graduated)

All five simplification/demotion RFCs graduated in this run (owner-accepted 2026-07-12) and moved to §1·0: **0035** → v0.187; **0036**, **0037**, **0038**, **0039** → v0.188. This bucket is now empty; the owner-decision gate that §4a projected onto the graph is discharged.

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
| 0033 ffi v2 | §32 amendment train *(landed v0.186)* | 0007/0011/0014(§9), 0034(symbol seam) | 0007/0011/0014 runtime routings |
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

### 4a. Hard gates — DISCHARGED (the 5 RFCs graduated 2026-07-12)

These were **design changes** (§2 of the process) needing a proposal→accept→graduation. The owner accepted all five on 2026-07-12 and they graduated into the normative docs; the gate is closed. What remains is compiler catch-up (§1·0), not a decision:

- **0035** event-member demotion → **graduated v0.187** (`7024955`); #1550 closed, #1647 closed by construction, impl #1727/#1728 (after #1646).
- **0036** owning-inline-array demotion → **graduated v0.188** (`ebfa7e0`); #1551 closed, conformance staging #1737; **#1385** (whole-value reassignment) stays open, orthogonal.
- **0037** remove object-initializer blocks → **graduated v0.188**; #1648 closed, impl #1735.
- **0038** retire `ReadOnlyAsciiByteSpan` → **graduated v0.188**; #1649 closed, impl #1736; #971 item 3 retired.
- **0039** one specificity rule for extensions → **graduated v0.188**; #1650 closed, impl re-scoped onto #1307.

### 4b. Embedded sub-question gates — schedulable, but one owner decision gates *part* of the work

The proposal is accepted/draftable and most of it can proceed, but a load-bearing sub-decision blocks a specific piece:

- **0007** — **OpenSSL 3.x TLS-seam ruling**: the sockets/`Dns`/`TcpListener` half proceeds; `TlsClientStream` waits on the backend ruling.
- **0011** — **kill-on-dispose**: the "disposal SIGKILLs+reaps" semantics is the load-bearing owner question; the rest of the surface is settled.
- **0012** — **instant-order equality overriding §9.6 structural** (first stdlib record to do so) + the full-offset wire form.
- **0020** — **engine family (RE2) + `Match` shape** (RFC): the whole proposal is gated on these two.
- **0022** — **discovery fork** (the `test` modifier, RFC): the `Assert` library + `using var` fixtures ship under any fork; only the discovery endpoint waits.
- **0032** — DISCHARGED: graduated v0.186. The §28.13 widening (q4) and 0003 amendments landed; Stage-2 design (q5–q9) is recorded design-now/ship-later (#1725). Implementation only now (#1722–#1726).
- **0033** — DISCHARGED: graduated v0.186. Layout promise (q4) and the six §32 amendments landed as spec; the `env` instance-callback extension landed too. Implementation only now (#1705–#1720).
- **0034** — DISCHARGED: graduated v0.188. The four §2.8 rulings landed as normative spec; the entry-point widening (OQ5) was ruled and recorded in §33.1. Implementation only now (#1729–#1734 + #1398).
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
