# docs/proposals — design proposals (non-normative)

This folder holds **work-in-progress design proposals** for future Bezel features. Nothing in this folder is normative: the language is defined solely by the five documents in `docs/` (spec, stdlib, rationale, and the two guides). A proposal graduates by being amended *into* those documents through the regular docs workflow (revision bump, changelog, mechanical validation), at which point the proposal file gains a `Status: superseded` header pointing at the landed revision.

Conventions:

- One file per proposal, numbered: `NNNN-short-slug.md`.
- Every proposal carries a header block: `Status` (draft / under-review / accepted / superseded / withdrawn), `Tracking issue`, `Target module(s)`, and the spec/stdlib revision it was written against.
- Proposals must obey the repository's cardinal rule: **no invented current-stdlib APIs** — everything the proposal builds on must exist in `Bezel_Standard_Library.md` (cited), and everything new must be clearly marked as proposed.
- Proposed APIs are written in real Bezel syntax and should compile against the grammar (Appendix A) even though the types don't exist yet.

| # | Proposal | Status | Tracking |
|---|----------|--------|----------|
| 0001 | [Networking and the `Http` client](0001-networking-http.md) | draft | [#1376](https://github.com/s8lap/bezel-lang/issues/1376) |
| 0002 | [The `Http` server](0002-http-server.md) — minimal-API routing over 0001's fetch-symmetric vocabulary; named-method handlers (§2.1); sendability-checked worker domains | draft | [#1402](https://github.com/s8lap/bezel-lang/issues/1402) |
| 0003 | [`Locked<T>` / `RwLocked<T>`](0003-locked-t.md) — data-owning locks joining the §22.12 synchronized set; guard scope = critical section via the existing borrow rules | draft | [#1404](https://github.com/s8lap/bezel-lang/issues/1404) |
| 0004 | [Bezel MVU](0004-ui-mvu.md) — Elm-style UI on a domain; messages-as-sendable-data; owned node trees with cascade teardown; early draft, architecture + code shape only | early draft | [#1410](https://github.com/s8lap/bezel-lang/issues/1410) |
| 0005 | [Self-hosting the stdlib collections](0005-selfhost-stdlib-collections.md) — move collection implementations from the C++ runtime into `.bzl` source so the compiler's name-gated builtin special-casing atrophies; drafted mid-June, committed 2026-07-04 (was untracked at repo root) | draft | [#545](https://github.com/s8lap/bezel-lang/issues/545) |
| 0006 | [Concrete stream family](0006-stream-family.md) — StreamReader/Writer as the §7.4 TextReader/TextWriter concretes (owned-wrapping, §28.4 cascade), MemoryStream (ToArray/TakeBytes, `owned`-marker-distinguished FromBytes), Stream.CopyToAsync; borrowed-stream ctor deferred to a spec ruling (wave D1, gap #1) | draft | [#1433](https://github.com/s8lap/bezel-lang/issues/1433) |
| 0007 | [System.Net substrate](0007-system-net-substrate.md) — the companion 0001 promised: Dns, TcpConnection/NetworkStream/TlsClientStream (domain-local, NOT sendable), TcpListener (sendable + synchronized, the Channel analogue), the owner-ruled OpenSSL sans-IO TLS seam, SocketException taxonomy (wave D1) | draft | [#1436](https://github.com/s8lap/bezel-lang/issues/1436) |
| 0008 | [Integer toolkit](0008-integer-toolkit.md) — Wrapping/Saturating/Checked named methods on the primitive-wrapper records (64-bit wrapping mul has NO workaround today), BitOperations (unsigned, defined faults), HashCode (per-process seed; class pending a §9.4 value-receiver-mutation ruling) (wave D1, gap #9) | draft | [#1437](https://github.com/s8lap/bezel-lang/issues/1437) |
| 0009 | [Base64 + hex encodings](0009-base64-hex.md) — Base64/Base64Url static classes in System.Text (RFC 4648, strict-fault decode), Convert hex methods; `IReadOnlySpan<byte>` in / `owned Array<byte>` out; prerequisite of 0010 (wave D1, gap #3) | draft | [#1434](https://github.com/s8lap/bezel-lang/issues/1434) |
| 0010 | [Serialization for the value builtins + WriteBytes](0010-serialization-value-builtins.md) — compiler-provided ISerializable conformances for Guid/DateTime/TimeSpan/DateOnly/TimeOnly (§2.19-precedent; pinned wire formats) + the WriteBytes/ReadBytes vocabulary tokens (base64 text wire via 0009); two owner questions (DateTime.Kind loss; vocabulary freeze posture) (wave D1, gaps #4+#17) | draft | [#1435](https://github.com/s8lap/bezel-lang/issues/1435) |

Supporting records (not numbered proposals):

| Record | Purpose |
|--------|---------|
| [Stdlib gap review 2026-07](stdlib-gap-review-2026-07.md) | 31 ranked gaps across 9 domains + 8 declined-with-reasons + wave sequencing — the map the 0004+ proposals draw from ([#1403](https://github.com/s8lap/bezel-lang/issues/1403)) |
| [Networking design comparison](networking-design-comparison.md) | Recorded pros/cons of 0001/0002 vs .NET, fetch, Node/Express, Go, Rust — the two structural bets with costs stated plainly |
| [Exploration: compile-time DI](exploration-compile-time-di.md) | What D-0001's named remedy (generated wiring) would look like — the ownership-tree insight, three mechanisms (external tool viable; annotations and language construct blocked/premature), keep-in-mind list. Not a proposal. |
| [Exploration: UI approaches](exploration-ui.md) | The design-space map: MVU-on-a-domain + record-tree diffing fits like a glove; immediate mode strong; SwiftUI look-not-machinery; React meaning-not-hooks; auto-tracking and reflection-binding fall out. Not a proposal. |
| [Refactor plan review 2026-07](refactor-plan-review-2026-07.md) | Adversarial verdict on #1414: stage 5 refuted-and-replaced, stages 1-4 amended, Stage 0 added; the file-geometry-vs-fact-geometry reconciliation. |
| [Capture mechanism memo (#808)](capture-mechanism-808.md) | The researched general mechanism for deferred callback capture: 'scoped' parameters + storage-bounded capture (new §28.14), unifying events/TCS/TaskScope — replaces per-API carve-outs. Awaiting owner go. |
| [DECLINED.md](DECLINED.md) | The declined-with-reasons register (D-0001: DI container; D-0002: anonymous types; pointers to the gap review's eight) — revisiting requires new evidence, not re-asking |
| [Stdlib gap DESIGN plan 2026-07](stdlib-gap-design-plan-2026-07.md) | The recorded plan (owner-directed 2026-07-04) for the ~25 gap-review items that still lack design artifacts: waves D1-D3, artifact type per item, the three owner-rulings-needed-first, and the definition of "designed" |

**Archive** ([archive/](archive/)): consumed working papers, kept for provenance — currently the four #703 type-driven-serialization documents (plan, design note, rationale-§5.22 draft, spec-edits E1-E7) whose content landed as docs v0.141 and whose migration completed (#703 closed). Not indexed individually; nothing in archive/ is live.
