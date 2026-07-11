# Implementation roadmap — 2026-07 (post spec/design run)

*Supporting record (not a numbered proposal). Written 2026-07-08 at the close of the 2026-07-07 spec/design run; docs at v0.183, main @ c1665ee. This is the sequenced work-list for future implementation runs: what to build, in what order, and why the order. Sizes are from the per-proposal audits (S/M/L/XL). Owner-review pending items are marked.*

> **Refreshed 2026-07-11** at the close of the second spec-quality run (docs v0.183 → **v0.185**, main @ `dd33a1c`). Two things changed: (1) the R0 tracker set below is now **mostly closed** — the intervening implementation runs landed the v0.179–v0.183 rulings, leaving only **7 open** (see the R0 scoreboard added below); and (2) two new revisions landed — **v0.184** (the ownership-coherence settlement) and **v0.185** (stdlib-coherence & currency) — adding **~52 new compiler-delta trackers (#16xx)** that are **new R0 work** preceding all proposal implementation. The proposal-to-proposal **dependency graph** promised for "implement all of them" now lives in its own companion, [`master-plan.md`](master-plan.md); this roadmap keeps the run-shaped R1–R8 sequence. The process that produced these revisions is written down in [`spec-change-process.md`](spec-change-process.md).

## R0 — Soundness catch-up (the compiler implements the new rulings) — FIRST

The run landed five semantic docs revisions; the compiler implements ~v0.177. Until R0 lands, the spec is normative and the compiler is behind on exactly these tracked items.

> **R0 scoreboard (2026-07-11).** Of the 61 v0.179–v0.183 trackers below, **54 are closed**; **7 remain open**: #1523 (async Domain.Spawn entries — 0032 Stage 0), #1525 (async-iterator token routing), #1527 (call-bounded escape sink), #1531 (DisposeAsync reclamation), #1538 (binding/assignment owned marker), #1565 (v0.181 conformance pack), #1571 (Pattern-A await confinement). **New R0 work from this run:** the **v0.184** ownership-coherence settlement and **v0.185** stdlib+currency batch landed ~52 compiler-delta trackers (search `is:issue "[v0.184]" OR "[v0.185]" in:title` and the 07-11 batch #1628–#16xx). These are the freshest R0 layer and should be triaged first — several are soundness-grade (e.g. the construction-window `this`-observation rule, the memory-model edges, the who-may-Dispose rule, self-static-init as ordinary execution). The v0.184/v0.185 changelog entries in the rationale are the authoritative spec-side reference for each.

**v0.179 async×borrows (#1518–#1531):** reject borrow-bearing record params/results on async (#1518) and locals-across-suspension (#1519); abandoned-continuation DAE reporting (#1520); async owned-param frame transfer/disposal (#1521); blocking-acquire discipline (#1522); async Domain.Spawn entries (#1523 — also 0032 Stage 0); receive-abandonment (#1524); async-iterator token routing (#1525); deferred-activation temporary rejection (#1526); call-bounded escape sink (#1527); faulted-task exception ownership (#1528); WhenAll/WhenAny contracts (#1529); Task<T>.Result rule (#1530); DisposeAsync reclamation (#1531).

**v0.180 borrow-liveness/capture (#1537–#1547):** consume-site freeze (#1537, closes #1465); binding/assignment owned marker (#1538); escape checks (#1539); record-storage rejections (#1540); event capture admission + scoped Add (#1541); static-event capture-free rule (#1542); primary-ctor owned capture leak (#1543); function-value equality/assignability (#1544); scoped-construction fallout (#1545); miscellany (#1546); parameter-sourced borrow-return (#1547).

**v0.181 statics/ctors (#1552–#1565):** out-var definite assignment (#1552); static-init cycle backstop (#1553); out-owned overload clash (#1554); once-only field initializers + chaining unwind (#1555); §22.12 rule-2 forms (#1556); static-class owned fields (#1557); nested-type visibility (#1558); variance-marker rejection (#1559); override widening (#1560); defaulted-arity tie-break (#1561); operator candidate set (#1562); entry-point scoping (#1563); enum static surface (#1564); conformance pack (#1565).

**v0.182 types/nullability (#1566–#1579):** §7.4 closure-capture invalidation (#1566 — soundness); cleanup-order inversion (#1567); throw-operand rules (#1568/#1569); Pattern-A await confinement (#1570/#1571); auto-equality/record-Deserialize (#1572/#1573); bool ordering + unary + (#1574); string? relational crash (#1575); typed-context widening (#1576); duplicate enum values (#1577); break/continue-in-finally (#1578); conformance sweep (#1579).

**v0.183 small rulings (#1580–#1587):** extension-resolution diagnostics (#1580); static extension members (#1581); enum operators (#1582) + enum Object-surface members (#1583); range patterns in alternatives (#1584); the `when true` out-postcondition + stdlib sweep (#1585); Range<char> surrogate gap + negative-magnitude guard (#1586); lambda destructuring (#1587).

**Pre-existing disposal tail (from the prior run, all SOLO+ASan):** #1466 nullable out-owned reclaim · #1483 generic owned-T param disposal · #1486 scope-end string-local leak · OwningSet #1474–#1478 · #1468 dtor-static-member-on-unwind · #1467 (out `_` discard — note v0.18x rulings may now settle it; re-check) · #1549 move-only reassignment warning.

**Wrong-code / crash cluster from this run's probes:** #1487 transitive-type garbage · #1488 module mangling · #1489 alias backtracking · #1490–#1493 spawn paths · #1495–#1505 (constrained dispatch, ranges, task discards, INumber crash, static-factory leak, WhenAny-Receive segfault, static-event async, task-result disposal, owned-array reject, generic catch) · #1511 library-main collision · #1512 IntPtr.Zero · #1513 cross-artifact generic statics · #1514 · #1536 EventSource nameability.

Gate discipline: disposal/exception/async/layout fixes take FULL SERIAL suite + asan_sweep (standing rule).

## R1 — Stdlib wave D1 (accepted; foundations)
Order: **0008** integer toolkit (M; wrapping mul has no workaround today; prerequisite of 0029) → **0009** Base64/hex (S; prerequisite of 0010) → **0010** value-builtin serialization + WriteBytes (S) → **0006** stream family (M; StreamReader/Writer/MemoryStream/CopyToAsync; borrowed-ctor v2 now unblocked by v0.180's #1439 landing).

## R2 — Stdlib wave D2 (accepted)
**0013** Environment (S) · **0016** monotonic time (S) · **0017** TempFile/Path (S) · **0015** PeriodicTimer (S; rides #1378 timer substrate) · **0014** signal hooks (S; rides bounded channels, landed v0.174) · **0018** async File (S) · **0011** Process (M; hard dep 0006) · **0012** DateTimeOffset/TimeZoneInfo (M; dep 0010; system tzdata).

## R3 — Stdlib wave D3 (accepted unless noted)
Small-utilities batch (land 2–3 per docs graduation): **0024** PriorityQueue (M) · **0025** BitArray (S; PopCount rides 0008) · **0026** LINQ additions (S) · **0027** SplitSpans (S) · **0028** Random extensions (S) · **0030** Unicode normalize (M) · **0031** Verify/Debug (S). Then: **0023** Task.WhenEach (M) · **0029** checksums (M; hard dep 0008) · **0021** structured logging (M; dep 0006 for file sink; async spawn entries landed v0.179) · **0022** test framework (L; Assert lib first; `test` modifier ruled in; Throws needs the v0.182 generic-catch rule + #1505 fix) · **0019** JSON DOM (L; dep 0006 for streaming emit) · **0020** Regex (XL; engine family ruled: linear-time RE2 class).

## R4 — Networking stack (accepted; sequenced)
**0007** System.Net substrate (L; runtime nonblocking event loop + OpenSSL sans-IO seam) → **0001** Http client (L; amended to v0.179) → **0002** Http server (L; worker-domain story rebuilt on per-worker route builders). Unblocks/closes: #645, #1394 along the way.

## R5 — Parallel extensions (0032; #1510)
Stage 0 async spawn entries (**already normative**, tracked #1523) → Stage 1 `Parallel.Map/Reduce/ForEach` (compiler-known; zero new checking rules) → Stage 1.5 `FrozenArray<T>` + `Shared<FrozenArray<T>>` (needs the §28.13 definitional-widening ratification, 0032 q4) → Stage 2 `Parallel.Join` (design ratification first: owner questions q5–q9). Pre-work bugs #1490–#1493 sit on exactly these paths.

## R6 — FFI v2 (0033; #1516)
Stage A `Ptr<T>` foreign memory + errno (unblocks sqlite3/curl-class bindings) → Stage B C-compatible records + layout engine (medium-high; the run's first normative layout promise, ratification q4) → Stage C variadic clause + link-name alias → Stage D by-value/float-field records (Win64 divergence documented). Unblocks the runtime-internal routings in 0007/0011/0014. Pre-work: #1512.

## R7 — Module identity (0034; #1517) + build driver
The §2.8 rulings (identity/closure, artifact interface, transitive deps, program assembly) + **#1398** SCC build driver. Evidence bugs to fix with it: #1487, #1488, #1511, #1513, #1514, #1515-spec-side (landed v0.183, closed). 

## R8 — Owner-gated (decide before scheduling)
RFC **0035** event-member demotion (#1550; **deepened 2026-07-11** — the post-demotion idiom is now a two-line floor, statics reworked to a sealed `ISharedEvent`/`SharedEventSource` pair engaging the live rule-5 alias gap #1647) · RFC **0036** owning-inline-array demotion (#1551) + **#1385** whole-value reassignment (recommended regardless) · **RFC 0037** remove object-initializer `with`-blocks on class types (#1648, new 2026-07-11) · **RFC 0038** retire `ReadOnlyAsciiByteSpan` — six span types → five (#1649, new; re-routes #971 item 3) · **RFC 0039** one extension-specificity rule, delete §26.5.1 rules 2–3 (#1650, new; re-routes #1307) · **#1535** out-owned narrowing (B) · **#1647** static-event capture-freedom alias gap (needs a resolution under *every* 0035 outcome) · **0004** MVU (XL; explicitly unscheduled) · **0005** self-hosting collections (XL; multi-run; refresh banner applied) · #1405 method-local records, #1409 property patterns (design passes owed) · #1427 unbounded tier (evidence-gated) · #1534 container crossing-mode ruling.

> The five simplification/demotion RFCs (0035–0039) are the run's proposed *net-subtractions* from the language surface — each removes a rule or a type without losing capability. They are grouped here because none can be scheduled until the owner rules; `master-plan.md` §4 projects them onto the dependency graph as decision gates.

## Cross-cutting maintenance
#1380 conformance-corpus regen (hand-annotation preservation; the extractor drift grew this run — every [EXAMPLE] fix and new marker-syntax example is invisible to the committed corpus until regen) · #1414 compiler-refactor stages (pre-existing plan; Stage 4 Phase A next) · #1506–#1509 §11.8/TCS follow-up promotions · #1520/#1546-style conformance batches ride their owning lanes.

## Suggested run-shapes
1. **"R0 soundness run"** — the freeze/marker/async-settlement checkers (Fable lanes, serial+ASan gates). Biggest correctness payoff; everything else assumes it.
2. **"D1+D2 stdlib run"** — R1+R2 in fan-out (S/M items parallelize well; the audit says all are spec-accurate post-amendment).
3. **"Networking run"** — R4 end-to-end (plus 0032 Stage 0/1 riding the same async substrate).
4. **"Platform run"** — R6 FFI-v2 Stage A + R7 module identity (they share the linking/symbol seam).
