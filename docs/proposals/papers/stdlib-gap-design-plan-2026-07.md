# Stdlib gap DESIGN plan (2026-07) — closing the review's unmet proposal commitments

| | |
|---|---|
| **Status** | Recorded plan (owner-directed 2026-07-04) — design work only; no implementation is scheduled by this document |
| **Origin** | The 2026-07-04 coverage audit: the [gap review](stdlib-gap-review-2026-07.md) promised "top items become their own proposals/issues," but ~25 of its 31 items have no artifact beyond umbrella [#1403](https://github.com/s8lap/bezel-lang/issues/1403). This plan records what "designed" means for each and in what order. |
| **Not in scope** | The eight gap-review declines (settled), items already designed (channels, TaskScope, Try-convention, formatting, linked CTS), and language-design passes (#1405 method-local records, #1409 property patterns, #1408 — tracked on their own issues). |

## Definition of "designed"

An item is designed when it has ONE of: (a) a numbered proposal in this folder (for anything with API-shape choices worth arguing), (b) a drafted stdlib-section text + tracking issue (for small additive surfaces with an obvious shape), or (c) a decline/defer record in [DECLINED.md](../DECLINED.md) or the gap review. Umbrella membership in #1403 does **not** count — that was the audit's finding.

## Wave D1 — foundations (unblock the most; design these first)

| Item (gap-review #) | Artifact | Why first / dependencies | Size |
|---|---|---|---|
| **Concrete stream family** (#1): StreamReader/Writer, MemoryStream, CopyToAsync | numbered proposal | The review's own top item; blocks async-file (#22), Process stdio (#5), 0002's bodies, SplitSpans consumers. Must settle: ownership of the wrapped stream, encoding defaults, buffer policy. | M |
| **System.Net substrate**: sockets, Dns, TcpConnection, NetworkStream, TLS backend | numbered proposal | Explicitly promised by [0001 §"substrate — separate proposal to follow"](../0001-networking-http.md); 0001/0002 cannot graduate without it. **TLS backend RULED (owner, 2026-07-04): OpenSSL 3.x**, wrapped runtime-internally behind a narrow **sans-IO seam** (memory-BIO byte pumps; ~8 `bz_tls_*` entry points; the scheduler owns all real I/O) so the backend stays swappable. rustls is the named second backend if TLS-layer memory safety becomes a priority (blocked today: `rustls-ffi` unpackaged on Fedora + Rust-toolchain debt + separate trust-store verifier); GnuTLS the runner-up; declined: s2n (unpackaged), mbedTLS (embedded focus), BoringSSL (no stable ABI), libtls (packaging). System trust store via OpenSSL defaults; full decline-with-reasons table goes in the substrate proposal. | L |
| **Base64 + hex encodings** (#3) | stdlib-section draft + issue | Trivial surface; prerequisite for `ISerializer.WriteBytes` (#17). | S |
| **Integer toolkit** (#9): wrapping/saturating ops, BitOperations, HashCode accumulator | numbered proposal | Prerequisite for checksums (#27); wrapping arithmetic interacts with §17 checked-arithmetic semantics, so it needs a real design pass, not just signatures. | M |
| **Serialization eligibility conformances** (#4): Guid/DateTime/TimeSpan/DateOnly/TimeOnly as ISerializable + `WriteBytes` token (#17) | stdlib-section draft + issue | Wire-format decisions only; unblocks serializing any time-bearing record. After Base64. | S |

**D1 rulings recorded (2026-07-04).** The four substantive D1 open questions are resolved (owner-accepted): (1) **0006** borrowed-stream ctor → the **scoped-construction rule** (generalize §28.14 `scoped` to class-typed borrow params), filed as spec RFC [#1439](https://github.com/s8lap/bezel-lang/issues/1439); v1 stays owned-only. (2) **#1438** async-span defect → borrow-bearing record params/returns forbidden on `async` methods (the two §7.1 span overloads removed; `CancellationToken` the enumerated exception), drafted for the next docs revision. (3) **0010** `DateTime` wire format → **conditional `Z` suffix** (Z iff `Kind.Utc`, bare otherwise), not blocked on `DateTimeOffset`; `WriteBytes` completes and freezes the token vocabulary. (4) **0008** `HashCode` → **immutable-chaining value record** (`Start()`/`Add`-returns-new/`Value`); `Checked*` naming, the int/uint/long/ulong cut, and per-process seeding all confirmed. See each proposal's open-questions section and the tracking issues (#1433/#1435/#1436/#1437/#1438).

## Wave D2 — platform and process

| Item | Artifact | Notes | Size |
|---|---|---|---|
| **Process** (#5): spawn, stdio pipes, exit, kill | numbered proposal | Stdio as streams → after D1 streams. §30 discipline: no blocking waits (decline already recorded). | M |
| **Environment surface** (#12): CurrentDirectory (get), ProcessId, OperatingSystem.Is* | stdlib-section draft + issue | Small; the setter decline is already recorded. | S |
| **Signal hooks** (#14): SIGINT/SIGTERM → Channel | stdlib-section draft + issue | Dependency (bounded channels) shipped in v0.174 — no longer blocked. Design point: which domain owns delivery. | S |
| **PeriodicTimer** (#13) | stdlib-section draft + issue | The #1378 timer thread exists; this is API shape only (owned timer, await-tick, CT interaction). | S |
| **Monotonic time** (#26): Instant, Stopwatch ticks, TimeSpan.FromNanoseconds | stdlib-section draft + issue | One decision: ns resolution vs TimeSpan's tick. | S |
| **TimeZoneInfo + DateTimeOffset** (#10) | numbered proposal | The promised tzdata-packaging RFC is the hard part; DateTimeOffset itself is small. | M |
| **Temp files + Path/Directory utilities** (#25) | stdlib-section draft + issue | TempFile RAII is a nice §28 showcase; needs the discharge-by-move ruling the review mentioned — fold it into the draft. | S |
| **Async File family** (#22): ReadAllBytesAsync/ReadAllLinesAsync etc. | stdlib-section draft + issue | Mechanical after D1 streams. | S |

## Wave D3 — library breadth (design when a consumer appears, or batch opportunistically)

| Item | Artifact | Notes | Size |
|---|---|---|---|
| **JSON DOM / JsonValue** (#21) | numbered proposal | **Load-bearing for the D-0002 decline** (it routes "dynamic data" here) — highest priority in this wave. | M |
| **Regex** (#19) | numbered proposal | Engine choice (backtracking vs RE2-style) is an RFC-grade decision; scope v1 deliberately small. | L |
| **Structured logging / ILogger** (#8) | numbered proposal | Interacts with domains + sendability; design after 0002 experience. | M |
| **Task.WhenEach** (the ungated half of #15) | stdlib-section draft + issue | Was promised as a co-design with TaskScope; TaskScope shipped without it. Streaming-completion semantics vs §30.13. | S |
| **Test framework** (#24): Assert library + `test` modifier | numbered proposal (RFC-track) | The `test` modifier is a language change — needs the RFC discipline, owner ruling. | M |
| **PriorityQueue (borrowing)** (#11), **BitArray** (#28), **Random extensions** (#30), **LINQ additions** (#18: indexed Select/Where, Chunk, MinBy/MaxBy), **SplitSpans** (#20), **Unicode Normalize(form)** (#31), **CRC-32/xxHash** (#27, after #9), **Verify/Unreachable/Debug.Fail** (#23) | one batched "small utilities" issue per item, stdlib-section drafts | Obvious shapes; the design content is mostly "which overloads" — batch 2-3 per docs revision. | S each |

**D3 designed (2026-07-04).** All thirteen wave-D3 items now have artifacts: proposals **0019** (JSON DOM, #1461), **0020** (Regex — RFC, #1449), **0021** (structured logging, #1460), **0022** (test framework — RFC, #1453), **0023** (Task.WhenEach, #1450), **0024** (PriorityQueue, #1451), **0025** (BitArray, #1452), **0026** (LINQ additions, #1457), **0027** (SplitSpans, #1458), **0028** (Random extensions, #1459), **0029** (checksums, #1454), **0030** (Unicode normalization, #1455), **0031** (Verify/Debug, #1456). Two carry owner rulings before graduation: **0020**'s engine family (recommended: linear-time RE2) and **0022**'s discovery fork (recommended: the `test` modifier, gated on the silent-no-run footgun materializing). **With D1–D3 complete, every gap-review item now has a design artifact, a decline record, or a tracking issue — the coverage gap the 2026-07-04 audit found is closed.**

## Process notes

- Each numbered proposal follows the README conventions (status header, tracking issue, real-syntax APIs, no invented current-stdlib citations).
- Drafted stdlib sections land through the normal docs workflow (revision bump + changelog); a proposal graduates the same way.
- Owner rulings before drafting: ~~the TLS backend (substrate)~~ **ruled 2026-07-04 — OpenSSL 3.x behind the sans-IO seam (see the D1 table)**; still open: the `test` modifier (language change), the Regex engine family.
- Suggested cadence: one D1 item per design session alongside normal fix runs; D1 is ~5 sessions of design work total.
