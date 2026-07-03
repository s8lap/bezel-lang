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
