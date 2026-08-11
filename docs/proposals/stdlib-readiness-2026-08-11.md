# Stdlib-wave readiness sweep — 2026-08-11

*Supporting record (not a numbered proposal). Post-program item 4 of the docs agenda, executed under the owner's autonomy grant ("work autonomously and address the findings — no confirmations, no rulings; I will review the changes in the repo"; the delegated-ruling precedent of the 2026-07-07/2026-07-24 rounds). Charter: sweep every accepted and draft proposal in the 0001–0031 range (plus the accepted later ones, 0054/0088) for flagged owner questions, deferred decisions, and "owner Q" markers, and **resolve** them so no stdlib implementation lane stalls on an open design question — recording each ruling decline-with-reasons-style in the proposal itself (a dated "Delegated rulings 2026-08-11" section, the 0012-precedent style). Questions genuinely unresolvable without external facts are recorded as evidence-gated with an exact bar, not forced. The four tier-2 flags of [`proposals-currency-2026-08-11.md`](proposals-currency-2026-08-11.md) §3 are OWNER-QUEUED and were deliberately not resolved (cross-referenced where touched). Docs at v0.247; `README.md` untouched (row updates reported in §4 for the coordinator).*

## 1. The complete open-question inventory (and each item's disposition)

Sweep population: 0001, 0002, 0004, 0005, 0006, 0007, 0008, 0009, 0010, 0011, 0012, 0013, 0014, 0015, 0016, 0017, 0018, 0019, 0020, 0021, 0022, 0024, 0025, 0026, 0027, 0028, 0029, 0030, 0031 (0003/0023 superseded, skipped) + accepted later: 0054, 0088. Every "Open questions" section, header flag, sweep flag, and planning-paper gate was walked.

| # | Open item (what blocked on it) | Disposition |
|---|---|---|
| 0001 | §8 Q1–Q7 (all unruled; the client lane's whole surface) | **RULED 2026-08-11** — all seven; see §2 |
| 0002 | §7 Q1–Q4 | already ruled 2026-07-24 (#1402); nothing open |
| 0004 | Q1 node representation, Q2 effects shape, Q4 first backend | **design-gated, not rulable** — these need a design pass with prototyping, not a ruling; 0004 is unscheduled (master-plan 1d), so nothing stalls. Q3 was answered by the language (v0.200/§11.7) |
| 0005 | D3 (`Drain()` deferral needs owner acceptance); standing sweep flag 1 (wholesale staleness) | **D3 RULED 2026-08-11: accepted**; flag 1 found already discharged by the 2026-07-30 owner-directed refresh (recorded in 0005) |
| 0006 | §6 Q1–Q5 | all ruled 2026-07-04/07-07; the scoped-construction rule is spec-amendment track (RFC #1439) — a recorded direction, not an open question |
| 0007 | §9 Q1–Q7 (incl. the TLS seam, listener sendability) | all ruled 2026-07-04/07-07; nothing open |
| 0008 | §8 Q1–Q4 | all owner-ruled 2026-07-04; the shift-fault reconciliation is **owner-queued tier-2 flag 1** — not touched, no 0008 ruling depends on it |
| 0009 | none declared | clean |
| 0010 | wire formats + vocabulary freeze | both §Open questions **owner-ruled 2026-07-04** (conditional-`Z`; freeze). Verified still current: stdlib §2.14 at v0.247 remains the 13+15 token set. No open question; the draft status is a ledger matter |
| 0011 | kill-on-dispose (the planning papers' standing §4b gate) + Q2–Q7 | all ruled 2026-07-07; **re-validated 2026-08-11 against v0.238–v0.247 and CONFIRMED FINAL** (premise check recorded in 0011) — the §4b gate is discharged |
| 0012 | Q1–Q3 (equality override, home, v2 scope); lookup-spelling flag | Q1–Q3 ruled (2026-07-07; elevated #1445, 2026-07-24). The `FindSystemTimeZoneById` registry-access spelling is **owner-directed (2026-07-29) to settle at 0012's graduation critique** — scheduled by owner instruction, deliberately not re-opened here; it does not precede graduation, so no lane stalls |
| 0013 | Q1 (`SetEnvironmentVariable`×§22.12), Q2 (naming) | both ruled 2026-07-07 (Q1 folded into #1363); nothing open |
| 0014 | Q1 Windows posture, Q2 restore policy | **RULED 2026-08-11** — loud fault; restore saved disposition; see §2 |
| 0015 | Q2 `Period` setter | **RULED 2026-08-11** — declined for v1, evidence-gated; Q1 ruled 2026-07-07 |
| 0016 | none declared | clean (deferrals are recorded decisions) |
| 0017 | Q1 error model, Q2 naming | **RULED 2026-08-11** — both confirmed; see §2 |
| 0018 | none open | acceptance rulings recorded 2026-07-07; clean |
| 0019 | Q1 peek-vs-freeze, Q2 duplicate keys, Q3 `AsInt` strictness; the §6c §9.7/§9.8-contradiction dependency | **RULED 2026-08-11** — (c)/fault/strict; dependency found resolved in current docs and discharged; see §2 |
| 0020 | engine family + Match shape (the RFC's two gates) + Q3–Q7 | all ruled 2026-07-07; engine family FINAL per #1449 (2026-07-24); nothing open |
| 0021 | §6 Q1–Q5 (ambient fork, sink lifecycle, drop aggregation, formats, naming); sweep flag 3 | **RULED 2026-08-11** — all five; flag-3 slip found already gone; see §2 |
| 0022 | discovery fork (the planning papers' standing gate); §5.4 catch-generic dependency; Q2 mechanism re-cut | fork ruled 2026-07-07 (FINAL; (a) endpoint, evidence-gated); **2026-08-11: the gate is sharpened into an exact bar and the §5.4 spec dependency is discharged** (landed in current spec; #1505 impl-only). The Q2 re-cut is **owner-queued tier-2 flag 2** — cross-referenced, not resolved |
| 0024 | none declared (owning form deferred to #1334 by scope) | clean |
| 0025 | none declared (PopCount dependency note only) | clean |
| 0026 | none declared | clean |
| 0027 | none open | acceptance ruling: promoted as-is; clean |
| 0028 | none open | acceptance ruling: promoted as-is; clean |
| 0029 | none declared | clean |
| 0030 | none open | acceptance rulings recorded; clean |
| 0031 | sweep flag 5 (factory-announcement exemption?), flag 4 (catch spelling), v0.243 fold, CaptureStackTrace deferral | **RULED 2026-08-11** — no exemption, `owned` return (§8 conformed); flag 4 verified already current; v0.243 folds clean; deferral stands; see §2 |
| 0054 | §8 Q1–Q5 | ruled 2026-07-24 (#2041); phase 2/S3 a recorded deferral; clean |
| 0088 | §7 Q1–Q5 | owner-ruled 2026-08-11 at graduation (stage 2 evidence-gated on the first real selector want); clean |

**Net effect:** with the 2026-08-11 rulings landed, **no proposal in the sweep population carries an unresolved design question that its implementation lane would stall on.** The remaining non-ruling items are: two owner-queued tier-2 flags (0008, 0022/0053 — the program's queue, not this lane's), one owner-scheduled graduation-critique item (0012's lookup spelling), and the evidence-gated register of §3.

## 2. The 2026-08-11 rulings (one paragraph each; full records live in the proposals)

- **0001 Q1 (`Json<T>`/`JsonOwned<T>`):** keep the two-name compiler-known split and conform the mechanism to the landed v0.232 facade shape (`where T: IDeserializable` spelled + compiler-known class-`T` direction) — proposal 0073 already faced the identical fork on 0001's own §3.3 dispensation and graduated the split, so the question is answered by landed precedent; unification stays with the (hypothetical) static-factory-constraint story.
- **0001 Q2 (default timeout):** confirmed 30s — that a default exists is the load-bearing decision; 30s clears slow handshakes that ky's browser-tuned 10s would cut; the number stays revisitable at graduation, the existence does not.
- **0001 Q3 (headers):** confirmed insertion-ordered `HeaderEntry` list, no index — honest sendable multimap at real-world header counts; an index is behind-the-surface optimization if ever profiled.
- **0001 Q4 (`Length`/`ContentLength`):** `Length` faults with `InvalidOperationException` when unknown (the 0007-Q2-ruled posture); `ContentLength` **amended** from the `-1` sentinel to `long?` — post-v0.231 the stdlib's absence convention is the nullable tier, and null-for-unknown is type-checked where `-1` invites silent arithmetic.
- **0001 Q5 (chunks):** both `Chunks(max)` and `ReadChunkAsync` stay public — the primitive serves exact-framing protocol readers.
- **0001 Q6 (span bodies):** resolved by the landed spec rule — v0.179/#1438 bars span parameters on async methods (spec §30.1; stdlib §7.1), so the `Array<byte>` form is final for v1 and the floated "consumed-before-first-suspension" rule was effectively declined when the prohibition landed.
- **0001 Q7 (charset):** confirmed — `Text()` honors Content-Type charset, `Json*<T>` always UTF-8 (RFC 8259), malformed input faults per §3.2.
- **0005 D3:** accepted — `Drain()` defers to its own slice; the §4.13 protocol is fully specified so nothing is lost, and folding it into SH-3 would blur the architecture-proof signal. (Housekeeping: sweep flag 1 was already discharged 2026-07-30.)
- **0011 (re-validation):** the seven 2026-07-07 rulings — kill-on-dispose above all — re-checked against v0.238 (trivial-dispose only; `Process` has a destructor), v0.243 (taxonomy is `Exception`-rooted), v0.246 (respells applied; declaration `owned` stands): **CONFIRMED FINAL**; the master-plan §4b gate is discharged.
- **0014 Q1 (Windows posture):** fail loudly — `Subscribe` raises `InvalidOperationException` naming the platform gap; a never-delivering channel is a structural silent no-run (the CodingKeys-class footgun), and the 0007-Q2 precedent covers the type choice (no `PlatformNotSupportedException` minted before a real Windows port exists).
- **0014 Q2 (restore policy):** restore the *saved previous* disposition (sigaction old-action), not unconditional `SIG_DFL` — identical behavior in the common case, and it stops the last dispose from silently clobbering an embedder-/FFI-installed handler; cost is one saved sigaction per kind.
- **0015 Q2 (`Period` setter):** declined for v1, evidence-gated — the setter must define the outstanding-await interaction (re-arm vs old-schedule vs fault), real concurrency design with no consumer; dispose-and-recreate is the explicit v1 spelling; reopens on a named adaptive-interval consumer.
- **0017 Q1:** confirmed — `Create()` raises `IOException` (the `FileStream` throwing-ctor shape; v0.229 band: environmental fault raises); no `TryCreate` until a probing consumer appears.
- **0017 Q2:** confirmed `Directory.CreateTempSubdirectory` — .NET-shaped `Directory` surface consistency; "Subdirectory" states the actual contract.
- **0019 Q1 (peek vs freeze):** (c) write-only in v1, ruled as recommended with the bar made exact — the freeze (owner 2026-07-04) verified in force at v0.247; (b) declined *outright* (format-dependent eligibility breaks §2.14's guarantee at exactly one type); (a) reopens only on a named consumer needing embedded-`JsonValue` round-trip through synthesis, taken then as 0010's defined breaking major event.
- **0019 Q2 (duplicate keys):** fault (`JsonParseException`), no last-wins mode — the typed path's landed §2.15 contract makes duplicates unconditional errors, and the DOM keeps one duplicate discipline across both readers; a lenient knob is evidence-gated and never the default.
- **0019 Q3 (`AsInt` on `1.0`):** strict `FormatException` — `AsInt` asserts integral syntax; `AsDouble()` + explicit rounding is the intent-stating path; consistent with lexeme-preserving `JsonNumber`.
- **0019 (dependency):** the §6c-cited §9.7/§9.8 contradiction is resolved in the current stdlib (records cannot hold class-typed serialized components; the sealed-class envelope is the named remedy) — discharged, no residue.
- **0021 Q1 (ambient fork):** Fork A only in v1; Fork B is not stdlib surface (an `ILogger` static warns **by construction** under the v0.245-derived §22.12 rule 4 — a stdlib member whose canonical use warns is the wrong posture) but survives as the documented user-side idiom; Fork C stays declined; B-as-stdlib revisits on recorded ergonomic pain in real 0002-consumer programs.
- **0021 Q2 (sink lifecycle):** composition-root-runs-the-loop confirmed (§30.15-canonical); the moved-channel isolation variant is the recorded v2 shape with its pre-minted-senders constraint.
- **0021 Q3 (drop aggregation):** v2 — a global total is cross-domain mutable state; design it with the first consumer that alerts on drops.
- **0021 Q4 (formats):** `Line` only in v1; JSON rides `LogEvent: ISerializable` (0010); the pluggable formatter parameter declined (a stored `scoped` callback for a need the named formats cover; additive later).
- **0021 Q5 (naming):** align short — the enum member becomes `Warn` (the proposal already chose `Info` over `Information`; one spelling per level); listings conformed in place.
- **0022 (gate + dependency):** the 2026-07-07 fork ruling stands FINAL; the (a)-promotion gate is sharpened to an exact two-trigger bar (first documented silent-no-run incident, or a suite whose `Add` list outgrows one-sitting review — whichever first, recorded on #1453); the §5.4 catch-generic-type-parameter dependency is discharged (landed in the current spec; #1505 is impl catch-up only).
- **0031 (factory exemption):** no exception-factory exemption from the v0.201 announcement rule — `Verify.Unreachable` returns `owned VerificationException` (§8 conformed); `throw` is an announced owned-consume so the idiom is unchanged, and an un-thrown call becomes compiler-visible instead of silently exempt; v0.243 folds clean; flag 4 (catch spelling) verified already current; the `CaptureStackTrace` deferral stands.

## 3. Evidence-gated register (exact bars; not resolved, by design)

| Item | Evidence bar (verbatim intent) |
|---|---|
| 0022 → the `test`-modifier grammar delta | First documented silent-no-run incident in a real suite, **or** the first `Add` list that outgrows one-sitting review — whichever first; record on #1453 (sharpened 2026-08-11) |
| 0015 → `Period` setter | A named adaptive-interval consumer; the reopening design leads with the outstanding-await rule |
| 0019 → the `PeekValueKind` token (option (a)) | A named consumer needing embedded-`JsonValue` round-trip through the agent pair/§9.8 synthesis; taken as 0010's rationale-recorded breaking major event |
| 0019 → lenient duplicate-key parse knob | A named consumer that must ingest third-party duplicate-key JSON; explicit option, never the default |
| 0021 → Fork B as stdlib surface; drop aggregation; pluggable formatter | Recorded ergonomic pain in real 0002-consumer programs / a consumer that alerts on drops / a consumer the two named formats cannot serve |
| 0014 → a dedicated platform-gap exception type | An actual Windows port landing (that port's decision) |
| 0017 → `TempFile.TryCreate` | A probing consumer (§1 Try-convention applies then) |
| 0012 → tzdata bundling (D6, pre-existing) | A named consumer the host DB cannot serve: version-pinned reproducible builds, or a target with no system DB |
| 0007/0001 → `IPAddress`/`Uri` value types (pre-existing) | The first real programs demonstrating string insufficiency (0070 exists for `Uri`; adoption stays additive) |
| 0088 → stage 2 `sourced` (owner-ruled, listed for completeness) | The first real concrete-selector want filed by an API surface |

Owner-scheduled (not evidence-gated, not this lane's): 0012's registry-access lookup spelling settles at its graduation critique (owner direction 2026-07-29). Owner-queued (the program's queue): currency-paper tier-2 flags 1–4 (0008×shift-fault ground; 0022/0053×hashable premise; 0047×Exception-derived printing; 0047×A7 scope).

## 4. README row updates needed (report only — the coordinator applies)

- **0001** — append: "; §8's seven open questions ruled 2026-08-11 (delegated): split kept per the v0.232 facade precedent, 30s default, ordered header list, `ContentLength: long?` (amended from −1), span bodies closed by the landed v0.179 rule".
- **0011** — the row's "kill-on-dispose is the load-bearing owner question" is stale: replace with "kill-on-dispose ruled 2026-07-07, re-validated FINAL 2026-08-11 — no open questions". (Master-plan §4b's 0011 row likewise reads *discharged*; that paper is another lane's re-baseline, reported here only.)
- **0014** — append: "; the two owner questions ruled 2026-08-11 (loud `InvalidOperationException` on unsupported platforms; save-and-restore disposition)".
- **0015** — append: "; `Period` setter declined for v1 (ruled 2026-08-11, evidence-gated)".
- **0017** — append: "; both owner questions ruled 2026-08-11 (IOException error model confirmed; .NET name confirmed)".
- **0019** — append: "; the three open questions ruled 2026-08-11 (write-only v1 kept, duplicate keys fault, `AsInt` strict); the §9.7/§9.8 dependency discharged".
- **0021** — append: "; the five owner questions ruled 2026-08-11 (Fork A only; root-runs-the-loop; `Warn`)".
- **0022** — append: "; (a)-promotion gate sharpened + §5.4 catch-generic dependency discharged 2026-08-11 (#1505 impl-only)".
- **0031** — append: "; ruled 2026-08-11: `Unreachable` returns `owned` (no factory-announcement exemption), v0.243 folded".
- **0005** — the row may note "D3 (Drain deferral) accepted 2026-08-11; the 2026-07-30 refresh discharged the staleness flag".
- **Supporting-records table** — add this paper's row.

## 5. Provenance

One sweep pass over the 31-file population (every "Open questions"/"Rulings" section read; header ⚠ notes, the two currency papers' flag lists, master-plan §4b, and the gap-design-plan's rulings-needed list cross-walked). Every 2026-08-11 ruling was re-derived against the **current** docs before landing — the specific premise checks: stdlib §2.14's token set (freeze intact), stdlib §2.15's duplicate-field contract (unconditional error), stdlib §7.1's no-async-span note + spec §30.1 (v0.179 landed), the §9.8 record clause (class-envelope remedy normative), spec §19.5/§31.2.1 (type-parameter patterns landed), spec §31.1 (`throw` as announced owned-consume) with §28.3's discharge machinery, spec §22.12 rules 2/4 as re-derived at v0.245 (fact-derived suppression set), and stdlib §2.15's v0.232 `Parse<T>`/`ParseOwned<T>` shape. Rulings land in ten proposals (0001, 0005, 0011, 0014, 0015, 0017, 0019, 0021, 0022, 0031); three mechanical conforms accompany their rulings (0021's two enum listings; 0031's §8 signature). Superseded/declined proposals, planning papers, normative docs, tests/, and `README.md` untouched. `scripts/check_docs.sh` green at landing.
