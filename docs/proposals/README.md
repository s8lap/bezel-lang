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
- **0002 — The `Http` server** (`0002-http-server.md`): minimal-API/Express-shaped routing over the fetch-symmetric Request→Response vocabulary of 0001; handlers on worker domains with compile-time sendability; draft for owner review.
- **Stdlib gap review 2026-07** (`stdlib-gap-review-2026-07.md`): 31 ranked gaps across 9 domains + 8 declined-with-reasons + wave sequencing; the map the 0003+ proposals draw from.
- **Declined register** (`DECLINED.md`): declined-with-reasons directions (D-0001 DI container; pointers to the gap review's eight).
- **Networking design comparison** (`networking-design-comparison.md`): recorded pros/cons of 0001/0002 vs .NET, fetch, Node/Express, Go, Rust — the two structural bets (fetch symmetry; guarantees over batteries) with costs stated plainly.
