# docs/research — research records (non-normative)

Investigations, explorations, design comparisons and completeness audits. Nothing here is normative: the language is defined by the documents in `docs/`, and a research finding changes the language only through a numbered proposal ([`docs/proposals/`](../proposals/README.md)).

## Completeness matrices

Each matrix crosses kinds of value with positions they can reach and records, per cell, the spec text that decides it. The run files are the record; the pages are generated views (`matrix-page/build_page.py`).

| Matrix | What it audits |
|---|---|
| [Borrow flow](borrow-flow-matrix/README.md) | whether a value can reach a position and a borrow it carries outlive its source |
| [Disposal](disposal-matrix/README.md) | whether every owned value is disposed exactly once on every path |
| [Generics](generics-matrix/README.md) | whether ownership and borrow rules hold at every substitution of a type parameter |

## Explorations and design comparisons

| Record | Purpose |
|---|---|
| [Exploration: UI approaches](exploration-ui.md) | The design-space map: MVU-on-a-domain + record-tree diffing fits like a glove; immediate mode strong; SwiftUI look-not-machinery; React meaning-not-hooks; auto-tracking and reflection-binding fall out. Not a proposal. |
| [Exploration: immediate-mode UI](exploration-ui-immediate-mode.md) | The API surface and traced usage code for the immediate-mode lane (owner question 2026-09-18, v0.306): three compiler-checked reasons the paradigm is *more* at home here (call-bounded closures make "the library retains nothing" a type fact; `ref` write-back without pointers; no global context, non-sendable by construction), the frame model, a `Ui`/`Response`/`WidgetId`/`DrawCommand` vocabulary, two widgets in full, five usage examples (counter, login form, list with loop-salted IDs, worker→channel→frame, headless test), typed widget memory without `Any`, the platform seam with no callback inversion, and six open questions. Not a proposal. |
| [Exploration: a VCL/LCL-style component library](exploration-ui-vcl-retained.md) | A retained, Delphi-style component tree: the Owner/Parent graph maps onto owned fields and borrowed back-references, teardown is the disposal cascade, and `OnClick` is the stdlib event pattern |
| [Exploration: wrapping a native toolkit](exploration-ui-toolkit-binding.md) | Toolkit candidates for a first native wrap, a designer-first (Lazarus) workflow, and its work breakdown; nothing scheduled |
| [Exploration: compile-time DI](exploration-compile-time-di.md) | What D-0001's named remedy (generated wiring) would look like — the ownership-tree insight, three mechanisms (external tool viable; annotations and language construct blocked/premature), keep-in-mind list. Not a proposal. |
| [Networking design comparison](networking-design-comparison.md) | Recorded pros/cons of 0001/0002 vs .NET, fetch, Node/Express, Go, Rust — the two structural bets with costs stated plainly |
| [Capture mechanism memo (#808)](capture-mechanism-808.md) | The researched general mechanism for deferred callback capture: 'scoped' parameters + storage-bounded capture (new §28.14), unifying events/TCS/TaskScope — replaces per-API carve-outs. **Superseded** — landed as spec §28.14 (v0.175) + the v0.177 call-bounded default; tracked residue #1432/#1425/#1427. |

## Investigations

| Record | Subject |
|---|---|
| [2026-08-10](2026-08-10-dual-oracle-gate-investigation.md) | Dual-oracle gate investigation (2026-08-10) |
| [2026-08-19](2026-08-19-gate-performance-audit.md) | Gate performance audit (2026-08-19) |
| [2026-08-20](2026-08-20-null-lifting-investigation.md) | Null lifting in Bezel: investigation and decision |
| [2026-08-21](2026-08-21-gate-performance-measured.md) | Gate performance: the audit's model, measured (2026-08-21) |
| [2026-09-02](2026-09-02-in-memory-test-execution.md) | Running the test suite from memory: what the disk is actually doing (2026-09-02) |
| [2026-09-29](2026-09-29-scripting-tier-and-direct-emitter.md) | A scripting tier for Bezel without a garbage collector, and a direct-emit backend (2026-09-29) |
| [2026-10-27](soundness/README.md) | How to know an ownership-rule change has not regressed soundness: a rule-to-test pin map, a toy executable model of the ownership kernel with rule mutation, and a staged plan (matrix gate, executable model, proposal 0047's proofs) |
| [2026-10-27](certain-faults/REPORT.md) | Certain faults the language admits or only warns about: programs whose failure is certain on every run that the compiler could reject with analysis it already has, ranked, measured on release r35 in both disposal modes, with the near-misses and the compiler bugs found |
| [2026-10-28](trust-report-review/REVIEW.md) | A review of proposal 0131's trust-admission report against spec v0.349 and release r35: what is stale, which report sites r35 implements (3 of the spec's set), which compile-time candidates hold up, with 53 probes; led to v0.350 and proposal 0214 |
