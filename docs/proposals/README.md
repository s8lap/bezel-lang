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
