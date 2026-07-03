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

Supporting records (not numbered proposals):

| Record | Purpose |
|--------|---------|
| [Stdlib gap review 2026-07](stdlib-gap-review-2026-07.md) | 31 ranked gaps across 9 domains + 8 declined-with-reasons + wave sequencing — the map the 0004+ proposals draw from ([#1403](https://github.com/s8lap/bezel-lang/issues/1403)) |
| [Networking design comparison](networking-design-comparison.md) | Recorded pros/cons of 0001/0002 vs .NET, fetch, Node/Express, Go, Rust — the two structural bets with costs stated plainly |
| [Exploration: compile-time DI](exploration-compile-time-di.md) | What D-0001's named remedy (generated wiring) would look like — the ownership-tree insight, three mechanisms (external tool viable; annotations and language construct blocked/premature), keep-in-mind list. Not a proposal. |
| [Exploration: UI approaches](exploration-ui.md) | The design-space map: MVU-on-a-domain + record-tree diffing fits like a glove; immediate mode strong; SwiftUI look-not-machinery; React meaning-not-hooks; auto-tracking and reflection-binding fall out. Not a proposal. |
| [DECLINED.md](DECLINED.md) | The declined-with-reasons register (D-0001: DI container; D-0002: anonymous types; pointers to the gap review's eight) — revisiting requires new evidence, not re-asking |
