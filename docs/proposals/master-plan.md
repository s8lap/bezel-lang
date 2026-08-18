# All-proposals master implementation plan + dependency graph

*Capstone deliverable of the 2026-07-11 spec-quality run. Dependency-first view over **all 39** numbered proposals (0001–0039). Complements — does not replace — [`implementation-roadmap-2026-07.md`](implementation-roadmap-2026-07.md), which sequences the same work as R0→R8 run-shapes. This document adds the explicit **proposal-to-proposal DAG** the roadmap does not carry.*

*Sources: `docs/proposals/README.md` (status ground truth, per spec-change-process §8), `phase8-interactions.json` (the 37-item verified interaction map — primary dependency data), `phase8-scoreboard.json` (issue state), and the headers of the ten most-entangled proposals. Written against docs **v0.185**; **refreshed 2026-07-12** for the graduation run (docs now **v0.188**, main `ebfa7e0`). **Nine proposals have now graduated into the normative docs** (`superseded`): 0003/0032/0033 (v0.186), 0035 (v0.187), 0034/0036/0037/0038/0039 (v0.188) — see §1's graduated ledger. The graduation is docs-only; no *implementation* has landed yet, so the R1–R8 build tracks are unchanged in size — but the nine graduated proposals are now spec-complete and their remaining work is pure compiler catch-up (impl trackers #1705–#1737).*

*Currency note 2026-07-14: docs are now **v0.191** (defect-sweep revisions v0.189–v0.191, main `1469b21`); no proposal status changed — the delta is six new R0-layer compiler trackers (#1781–#1786, headlined by the §28.3 Task-family ownership ruling, #1786) and one new design-review item (#1787, iterable-frame ownership). The roadmap's 2026-07-14 refresh note carries the detail; this plan's dependency graph is unaffected.*

*Currency note 2026-08-11: docs are now **v0.241** (this note's coverage horizon; the in-flight elegance-graduation program, v0.242+ / proposals 0078–0090, is deliberately not covered — its ledger rows land with the program-close banner: **§1·Δ·E below, landed 2026-08-11 at program close**). The gap since 2026-07-14 is large and this plan's ledger no longer describes the proposal population: the 2026-07-18 future-directions program added **0041–0054** (plus the separately-filed **0040** build-system sketch), the later trains added **0055–0077**, and twenty-nine proposals graduated in the interval, several partially (0044/0045/0051 Part A; 0061 §1; 0075 stages 1–2; 0076's criterial rule; 0077 stage 1) — the v0.198–v0.204 batch (0041–0046, 0048–0051 Part A; recorded in the roadmap's 2026-07-19/20 banners) plus the v0.205–v0.238 batch (0023, 0055–0059, 0061–0067, 0072–0077; recorded in the roadmap's 2026-08-11 banner). §1·Δ below carries the bucket-by-bucket deltas. Two standing §1 claims are corrected by the deltas: "no proposal is implemented yet" is no longer true (the 2026-07-20 impl run landed sixteen features of the v0.198–v0.204 layer — see the roadmap's banner), and the header's "all 39 numbered proposals (0001–0039)" now describes only this document's original DAG scope, not the ledger population. The dependency graph of §2 is unchanged for the 0001–0039 set; the new drafts' edges are recorded in §1·Δ(d) rather than merged into §2a's table. The 2026-07-28 proposals-syntax-currency sweep modernized retired spellings across all non-superseded proposals (38 sites, 15 files) — read it before picking up any pre-v0.217 draft.*

*Currency note 2026-08-18 (this re-baseline): docs are now **v0.261** (main at the v0.261 merge, `ee8c82db`). The gap v0.250 → v0.261 is the 2026-08-12 defect-resolution batch (v0.251–v0.254), the 2026-08-18 spec-question batch (v0.255), and the 2026-08-18 **review program** (the defect train v0.256, then proposals 0093–0102 + 0094 stage 2 graduating across five trains, v0.257–v0.261), all ruled under the owner's 2026-08-18 delegation. **§1·Δ·G below is this re-baseline's ledger delta and compiler-work inventory** — executed as the review program's closing step per the owner's direction ("adjust the master implementation plan with the needed compiler implementation work from today's spec work run"); the run-shaped sequencing lives in the roadmap's matching 2026-08-18 banner (this plan carries the what/priority, the roadmap the ordering — cross-referenced, not duplicated). One carrier fact: per the v0.260 changelog entry's Migration lead, **#3248 and this plan are the named carriers of the 0100/0102 compiler surfaces** — §1·Δ·G(b) item 5 is their record.*

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
| 0036 | Demote owning inline arrays to Extended | v0.188 (`ebfa7e0`) | #1737 (conformance staging); #1385 stays open *(since closed — resolved by v0.260's A13 retirement; residuals ride #3263, §1·Δ·G(c))* |
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

### 1·Δ·E. Ledger delta at program close, 2026-08-11 (docs v0.241 → v0.246): the elegance graduation, proposals 0078–0090

**(a) Graduated (`superseded`) — eleven proposals across five composed trains** (owner-ruled 2026-08-11, all questions as recommended; per-train detail in the v0.242–v0.246 changelog entries; every graduation is docs-only at landing, and the impl-tracker column — the compiler catch-up — is sequenced by [`catchup-run-sequence-2026-08-12.md`](catchup-run-sequence-2026-08-12.md), the run's starting page):

| # | Proposal | Graduated | Impl trackers (compiler catch-up) |
|---|---|---|---|
| 0078 | Checked enum conversions | v0.242 | #3044 |
| 0079 | Shift-count domain | v0.242 | #3045 (the four masking-pin files flip) |
| 0080 | Exception-derived throwables | v0.243 | #3046 (~215-site base sweep lands ahead of the §31.1 flip as a no-op commit; two permissive-rule pin files flip polarity) |
| 0081 | Abandonment lints (both) | v0.243 | #3047 (zero existing corpus sites fire either lint) |
| 0083 | Reserved discard | v0.244 | #3049 (the A.1 `Identifier` split + 128-site `var _ :=` migration; subsumes #2679, closed against its landing) |
| 0084 | `equality skip` | v0.244 | #3050 (the one-directional eligibility gate as landed — gate-corrected) |
| 0085 | Pure-body `for` elision | v0.244 | #3051 (the `pure_body_22_15` rejection pin flips; the elision is mandatory inside pure bodies) |
| 0086 | Consistency & record-keeping batch (all eight items) | v0.242 | #3052 (incl. the 86-site `sealed abstract` → `closed` test respell) |
| 0087 | Statics vocabulary | v0.245 | #3053 — **zero compiler work**; the Q3 check_docs derivation check (3b) landed with the train; **closed** |
| 0089 | Generic-leaves retirement | v0.245 | #3055 (~1,000 sema lines delete; six test files flip) |
| 0090 | Transfer-marker unification | v0.246 | #3056 (the ~633-line four-regex-family corpus respell; the owned-slot-store family drops its marker rather than respelling it) |

**(b) 0088 — accepted; stage 1 graduated v0.243, stage 2 owner-gated.** Stage 1 (rationale §5.7's concrete-selector cliff record; the declaration-site generify-remedy diagnostic is compiler-side, #3054, XS) graduated with train B. Stage 2 — the `sourced` return-position marker — stays in the proposal, gated on the first real concrete-selector want (the 0051 Part B evidence posture). The proposal does not flip to superseded.

**(c) 0082 — accepted pending C3 (README row: `under-review — drafted 2026-08-11`).** Owner-accepted 2026-08-11 with its staging recommendation honored: graduation runs as its own small Train F (v0.247+) only if the C3 prototype passes. Stage 0 measured 2026-08-11: C1 PASS (7/11 strict; every single-choice reading passes), C2 pass on the letter (#2042); C3 — the scoped await-bind desugaring prototyped behind a flag, verified to be the §28.10 cleanup-stack slot and nothing else — is the #3048 local-lane handoff. C3-fail → 0051 §3.5's decline path (a DECLINED.md record closing the v0.182 liftable). Train F does not block program close (owner sequencing ruling).

**(d) Tracking-issue states at close.** Two closed: **#3053** (zero compiler work — the check_docs derivation check was the train's own deliverable) and **#3060** (the §30.5/A7 seam, resolved by train E's docs). The rest — **#3044/#3045/#3046/#3047/#3049/#3050/#3051/#3052/#3054/#3055/#3056** — are open as labeled (`bug`, the #2654 spec-ahead-of-impl convention) catch-up trackers, and **#3048** is open as the design-validation lane feeding Train F; all are sequenced, with the three recorded ordering constraints and the no-soundness-grade classification, by the run-sequence page above.

**(e) Two owner-queued open questions** (recorded at v0.246, question-only — not ruled; they queue for the owner now that Train E has landed): **(E-f1)** the §22.6 owned-overloading ban / stdlib Try-pair naming — `M(move x)` vs `M(x)` is now call-site-distinct with no declaration-token ambiguity left, satisfying the ban's own carve-out criterion; **(E-f2)** the §3.14 named-destructure no-transfer liftable — the unified marker gives named slots a natural transfer spelling for the first time (0090 carries it as gap G5). Both are recorded in the rationale beside their home records (§3.10, §3.14). *(Both closed at v0.250 — §1·Δ·F row (b) below.)*

### 1·Δ·F. Ledger delta 2026-08-12 (docs v0.246 → v0.250): Train F and the Train E follow-on train

**(a) Graduated (`superseded`) — three proposals across two trains** (per-train detail in the v0.249 and v0.250 changelog entries; graduations docs-only at landing, compiler catch-up sequenced by [`catchup-run-sequence-2026-08-12.md`](catchup-run-sequence-2026-08-12.md)); v0.247/v0.248 were the guide-revalidation and spec-example-defect revisions (changelog entries; no proposal ledger movement):

| # | Proposal | Graduated | Impl trackers (compiler catch-up) |
|---|---|---|---|
| 0082 | Pattern A scoped bind (Train F; owner-ruled 2026-08-11, C1/C2/C3 gates passed) | v0.249 | #3048 (production trigger; the conditional-producer predicate; the Q4 teaching diagnostics; #3074 ordered first — the `const` mis-accept) |
| 0091 | Owned-overload lift (E-f1 executed; resolved 2026-08-12 under the owner's delegation, all questions as recommended) | v0.250 | #3086 — **after #3056**: the applicability filter, the erasure-guard clash, the reworded §22.6 diagnostics, the `WhenAll`/`WhenEach` recognition re-keying (47 name-keyed `src/` sites + the runtime symbol), the 49 gtest `*Owned` helper-line renames |
| 0092 | Named-destructure transfer (E-f2 executed; resolved 2026-08-12 under the owner's delegation, all questions as recommended) | v0.250 | #3087 — after #3056, with/behind #1409 (no shipped named-slot parser exists); the move-only-value-payload gap is spec defect #3094 |

**(b) The E-f1/E-f2 questions of row (e) are closed** — resolved 2026-08-12 under the owner's delegation ("Please work to resolve all questions the right way as you see fit. Once resolved, graduate both proposals into the docs"), all fifteen questions as recommended; the rulings and their reasoning live in rationale §3.10 (the lift record) and §3.14 (the marked-slot record), the migration in the v0.250 changelog entry. #3086/#3087 join the R0-adjacent catch-up sequence behind #3056 (items 20–21 of the run-sequence page).

### 1·Δ·G. Ledger delta 2026-08-18 (docs v0.250 → v0.261): the review program — re-baselined as its closing step

*Stamped 2026-08-18, the review program's closing step, ruled under the owner's 2026-08-18 delegation. This section is the program's compiler-work inventory — the **what and the priority**; the **sequencing** (ordering constraints, run shape) is the roadmap's matching 2026-08-18 banner — cross-referenced, not duplicated. Every item below was verified against its changelog entry's Migration/Spec-ahead-of-impl ledger (the authoritative per-revision work statement) and carries its issue number and spec revision.*

**(a) Graduated (`superseded`) — ten proposals across five trains, v0.257–v0.261** (per-train detail in the changelog entries; every graduation docs-only at landing; the impl-tracker column is the compiler catch-up). The interleaving revisions: v0.251–v0.254 were the 2026-08-12 defect-resolution batch — their small impl arms ride the standing **#2045**/**#2053** trackers, no new rows (except #3117, row (b) item 3) — and v0.255 (the four-ruling spec-question batch) and v0.256 (the program's defect train) filed the pre-program tail of row (b).

| # | Proposal | Graduated | Impl trackers (compiler catch-up) |
|---|---|---|---|
| 0093 | Kernel vocabulary & kill table (charter B1+B2) | v0.257 | **zero compiler work** (five optional `src/` diagnostic-string conforms recorded at 0093 §5, untracked) |
| 0095 | Census rename + `defaultable` (B4+B5) | v0.257 | **zero compiler work** (zero diagnostic-string edits, measured at landing) |
| 0096 | Iterable calls-mint unification (C1) | v0.258 | **#3257** — the program's headline structural item; supersedes #3246's sema arm (#3246 closed) |
| 0097 | Superseded-exception snapshots (C3) | v0.259 | **#3261** (0097 arm — incl. the gate-measured sema/ABI surface) |
| 0098 | Stdlib surface program (C7) | v0.259 | **#3261** (0098 arm — the rename surface, `ElementAtOrNull`, `Sum`) |
| 0099 | `ScopeGuard` (C5) | v0.259 | **#3261** (0099 arm) |
| 0100 | The body seam, examined and closed (C12; the owner's own seam ruling) | v0.260 | **#3248** + this plan (row (b) item 5) — keyword reservation first |
| 0101 | A13 retirement study — conducted, concluded in **Rule B** | v0.260 | **#3263** — the memory-safety headline (row (b) item 1) |
| 0102 | Named-move lift + small records (C4/C2/C6/C8) | v0.260 | this plan (row (b) item 5) — the test flip behind **#3056** |
| 0094 | Enumeration dedup + announcement table (B3; stage 2, re-derived post-0096) | v0.261 | **zero compiler work** ("no compiler surface moves and no test migration arises") |

**(b) The compiler-work inventory, in priority order.** Two headline entries lead — **#3263**'s memory-safety defects and **#3257**'s unification — then the pre-program sema tail, the feature/stdlib layer, the parser/lexer lane, hygiene; **#3262** is dev-box-gated.

1. **#3263 (v0.260, 0101) — memory-safety codegen at owning-inline-array whole-value reassignment.** Three defects measured at HEAD, all in `lowerOwningInlineArrayReassign`'s territory: **(i)** the **literal-RHS ordering UAF** — the collection-literal path disposes displaced slots *before* evaluating literal elements, so a throwing element observes disposed slots (violates the K9.5 RHS-first shape; the main path is verified RHS-first); **(ii)** the **moved-from-LHS double-free** — the lowering consults no moved-from state for the LHS, so `var b := a; a := X;` disposes slots whose obligations left with the move (invisible to the suite today: the pinning test is sema-only); **(iii)** **no slot nulling in the per-slot pass** — disposed slots dangle on the exception edge, contradicting §28.7's "left empty", and the §28.4 multi-value-cleanup continuation is unimplemented. Each needs a `runAot` + ASan regression citing spec v0.260. Adjacent, same tracker: verify the lowering does **not** run at static-field targets (the §22.13 posture requires the exclusion; its type-keyed guard appears to admit them); the **#1549 warning** retires at locals/instance fields and is retained at the form-(iv) static locus (its two positive assertions flip to no-warning-plus-disposal pins; `no_warning_when_whole_array_moved_out_first` additionally becomes a `runAot` disposal pin); re-cite `move_only_inline_array_reassign_drop_on_overwrite_1385_test.cpp`'s phantom "§28.7 v0.215" header to v0.260 (the stale comment cite rides the same tracker); sema transfers per-slot moved-from state on whole-value moves; and the new **K5.2 freeze-reach sema arm** — a whole-value reassignment under a live borrow of the displaced value, slot-object borrows included, newly rejected — whose zero-corpus figure (the read-only 15-site/4-file reconstruction) **the type checker must confirm**, the recorded obligation.
2. **#3257 (v0.258, 0096) — the iterable calls-mint unification.** The v0.258 entry's Migration ledger is the normative work order. **Core rule:** calls-mint recognition re-keys to the **mint-announcing families** (Task + the eight iterable/enumerator heads + the lazy query) at every callee kind, keyed on the **declared, pre-substitution** return type — on the caller side *and* for lambda admission at function-type targets (the K2.4 keying); the v0.223 owning-head instance/extension rejection and the instance/extension `owned`-marker requirement **retire** (**42** currently-rejected declarations become legal as written). **New rejections/mechanisms landed at the v0.258 gate:** stored-view borrow returns rejected with the property-face remedy (the v0.190 stored-task wording); §27.6's formation bar for lazy-query-kinded callees at derived-interface return slots (the `OrderBy`/`OrderByDescending`/`ThenBy`/`ThenByDescending` quartet) with the `IOrderedEnumerable<T>` → `IEnumerable<T>` adapter bridge; property references over iterable-family-typed properties rejected at formation (stdlib measured clean — zero such properties); §27.1.1's pre-substitution reading at **storage loci** — a mint-announcing-returning function value stored into a field/property whose declared type carries a bare-type-parameter return slot is rejected at family substitutions, **Task family included** (the one recorded Task-side verdict move); **§28.14(2)** generalized to both families (a call-bounded callback's minted frame consumed to its K6.1 bound); the quartet's direct call reclassifies **borrow → mint**; **§29.11's layered model** — the container obligation discharged by the standard rules, the affine enumeration right spent at consumption points, K3.14's must-use warning **suppressed at statically-visible consumption points** and otherwise **newly warned** on plain-`var`-bound queries; **§29.1's uniform owned contract** — the shipped runtime alias guard (v0.235, #2384) already conforms, so the obligation is **verify no regression at the 7 self-returning corpus sites**, not new machinery. **Test migration (16 sites):** the 7 self-returning sites need no respell (the runtime guard covers them); the 5 owning-head view pins (`owning_iterable_head_borrow_return_2369` ×3, `deferred_activation_frame_return_1660` ×2) re-point at the new rule; the 4 `Items()` stored-view declarations in `ienumerable_vtable_linq_995_test.cpp` respell to the property face **with a property-face behavioral companion for each pinned behavior**. Plus the stdlib runtime's `Drain()` fresh-frame change. **#3255** is the open static receiver-returning seam (a design record, deliberately not part of this catch-up). **Supersedes #3246's sema arm — #3246 closed**; a fence implementation landing now would implement rejected text.
3. **The pre-program sema/regression tail (v0.254–v0.256)** — independent of #3257 and of each other: **#3117 (v0.254)** — the §12.7 boxed-location write rejection, sema: three position families rejected (lvalue-chain statements extending the narrowed binding, `ref`/`out` arguments over such chains, by-address `extern` inline-array arguments), the bare-binding compound/`++`/`--` forms staying accepted (the gate-corrected refinement); cite §12.7, v0.254. **#3124 (v0.255)** — value receivers are borrows, sema: member-body stores landing in the **receiver's own storage** rejected (accessor bodies included; the mediating class-hop shape stays accepted; constructor-bodies carve), plus the blanket settable-interface-conformance rejection; measured flip surface **33** record setter bodies across 14 files + **2** method-body sites (both inside already-rejected programs — no green flip) + **2** green conformance tests on the conformance arm alone (`xmod_interface_property_setter_itable_arc_2506`, `xmod_interface_named_local_setter_arc_2535`); cite §9.4, v0.255. **#3240 (v0.255)** — the compound-arithmetic throwable widening: a compound assignment or `++`/`--` whose desugared form is throwable (`/=`, `mod=`, checked `+=`) is an unrestored exception edge — a small widening in `findThrowable28_8_1`, riding the **#3080** family's existing surface; cite §28.8.1, v0.255. **#3242 (v0.255)** — the `Range` generic-substitution `ToString` rendering residue (`Range<char>` through an unresolved `T` renders code points) — a codegen bug under the existing each-component rule; cite §8.5, v0.255. **#3245 (v0.256)** — the ctor-unwind destructor-body **regression test** (implemented behavior already matches; the test is the missing pin: the throwing class's own destructor body does not run on constructor unwind, the base's does); cite §22.9, v0.256.
4. **The feature/stdlib layer:** **#3261 (v0.259, 0097–0099)** — *0097:* the runtime `SupersededChain` field + the append helper at the two supersession drop paths in `src/codegen/LlvmBackend_Disposal.cpp` (**earliest-first**: chain-then-snapshot, the deliberate mirror of `CauseChain`'s order), the best-effort §17.7 allocation carve-out, the self-supersession no-op + per-raise clearing for owner-owned instances, the two report printers (§33.3, §32.8) + `ToString()` rendering — **plus the gate-measured sema surface** superseding 0097 §7's "no new classification, no new diagnostic": the §31.4 exception header is a **fixed layout baked into the compiler** (`src/sema/Symbol.hpp`), so the new property is a **cross-module ABI change**; `BuiltinExcMember` is a closed enum whose three consumers (member lookup, `base.X`, §12.2 hiding — #2971/#2983/#2984) each need the new case; and the unknown-member diagnostic enumerates the surface by name (`ExprTypecheck_Calls.cpp`). *0098:* `ElementAtOrNull`; the `OwningSet` `Take`/`TryTake` → `Pop`/`TryPop` rename — **10** test call sites, **27** name-keyed recognitions across **8** `src/` files, the **#2582** family gate re-keyed with the `Pop` arity split (the 0091 `WhenAll` shape), the **4** LINQ-control sites verified unchanged; `Sum` → the two `INumber` generics with the byte-family acceptance and the overflow-checked-in-element-type behavior. *0099:* the `ScopeGuard` implementation + the freeze and dismiss-path behavior tests. Cite v0.259. **#3247** (charter C9) — the three §11.8 promotions: `FromCollectionLiteral` → a real `ICollectionLiteralTarget` static interface member (honest caveat: shrinks-not-vanishes — the dictionary form stays outside, the built-in list becomes conformance-derived); `IDeserializable.Deserialize` → a true static interface method; §16.3's `Range` element restriction → an `ISteppable` constraint. Any order among the three; the spec-side recognition prose simplifies as each lands. **#3249** — the `FrozenArray` implementation (comment-only in `src/` today; stdlib §4.19 surface + the §22.12 suppression-set/synchronized-set wiring), the **2** test respells (`enum_property_restrictions_2499_test.cpp` — whose `.Length` nit the migration incidentally fixes — and `static_fields_test.cpp`), and the `spec_0031` conformance-row flip out of the `blocked-on-#1723` skip class.
5. **The parser/lexer lane (0100 + 0102 — carried by #3248 and this plan, per the v0.260 entry):** *0100 (v0.260)* — reserve `requires`/`ensures`/`invariant` in the lexer/parser with the reserved-word diagnostic; the 2053 contextuality pin (`contract_clauses_2053_test.cpp:280`, `requires_as_parameter_name_not_a_marker_spec_22_16`) flips to a negative test; then the **0055-§3 diagnostic's final form** (#3248 item 3) — a non-abstract member parsing body-less *while carrying clauses* draws the clauses-precede-the-body note — with **keyword reservation FIRST** (the sequencing constraint: reservation is what fixes the note's final content, the parenthesize-or-rename remedy half being obsolete under it). *0102 (v0.260)* — sema **deletes** the named-argument rejection for move-rooted arguments and its §22.5-citing diagnostic (overload resolution untouched by construction); `owned_marker_transfer_test.cpp:81` (`named_owned_argument_rejected_spec_a7`) respells to `item := move x` and **inverts to an accept — sequenced behind #3056**, as the charter directs; new spec-cited `runAot` regressions per 0102 §1.4 (named move accepted and consuming; the pair's owned member by name; field/slot take-outs; `out var`/`scoped` named forms still rejected; the mixed-ordering pin).
6. **#3262 — dev-box-gated: the conformance-corpus merge-regeneration.** Three revisions behind at filing (v0.257–v0.259: net `spec_0324` + 213 drifted files, then +7 cases with ~109 renumbered `stdlib_NNNN` ids), with v0.260's 10 content-changed case files stacking on top (v0.261 adds nothing). **Merge, not overwrite**: every hand-derived `manifest.tsv` annotation carried forward, remapped by (doc, section, content) — never by id; the seven new cases classified against a freshly built `bezelc`, the oracle's build recorded (the v0.247 protocol). **Needs the dev box** — the remote container's LLVM 18/20 cannot build the classification oracle against HEAD's newer `llvm::Triple` API. Fold in while there: the **177-vs-178** active-count off-by-one (committed manifest vs `tests/conformance/README.md`) and the extractor **argv guard** (`extract_doc_examples.py` treats `--help` as an output root). Should **precede any large test-migration batch** so baselines are current.
7. **Hygiene — #3248:** the two stray Pascal `begin` residues absorbed by bare `hasErrors()` assertions (`vacuous_lint_factory_call_chain_claimed_3017_test.cpp:332`, `pure_residuals_2061_test.cpp:337`); the harness-lint proposal over the measured ~1,039-assertion absorption class (error-expecting tests assert message substrings); item 3 is the 0055-§3 diagnostic of item 5 above. **The runJit ratchet, flagged here per the charter:** the program charter recorded the count measured UP at **61** against CLAUDE.md's 60 pinned 2026-08-08; re-measured at this re-baseline's HEAD (the v0.261 merge), the CLAUDE.md recipe reproduces **exactly the pinned 60 `runJit(` + 3 `runJitChecked(`** — the 61 does not reproduce, so per that file's own suspect-the-recipe rule the ratchet is treated as **intact**, and the discrepancy is recorded against #3248 for a recipe-pinned re-verify rather than a raised count.

**(c) Retired / superseded on the record.** **#3246** — its sema arm is **superseded by #3257** at v0.258 (the landed fence text was removed, not amended); the issue is closed. **#1385** — closed-completed, and this plan's "stays open" rows were stale: v0.260's A13 study found the Rule-B codegen **already shipped** since the #1385 fix (`lowerOwningInlineArrayReassign` — reverse slot order, RHS-first, null-guard skip), the spec amended to match through the delegated study; §1·0's 0036 row and §4a's 0036 bullet are annotated in place, and the residual work is **#3263**'s three defects, not #1385. **#1549** — re-scoped at v0.260: the reassignment warning retires at locals/instance fields (it warned of a leak that no longer happens) and is retained at the form-(iv) static locus; the test flips ride #3263 (the roadmap's R0 disposal-tail row is annotated in place). **Open design records, deliberately not schedulable build rows:** **#3255** (§29.1's static receiver-returning protocol form), **#3258** (§22.13's throwing exit-time disposer — four open questions on both exit paths), **#3259** (the §31.4 rendering promise's declaring member).

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
- **0036** owning-inline-array demotion → **graduated v0.188** (`ebfa7e0`); #1551 closed, conformance staging #1737; **#1385** (whole-value reassignment) stays open, orthogonal. *(Since resolved: v0.260's A13 retirement adopted per-slot drop-on-overwrite, the codegen already shipped — §1·Δ·G(c); residual defects ride #3263.)*
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
