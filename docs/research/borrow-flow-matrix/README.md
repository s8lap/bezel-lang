# Borrow-flow matrix: a completeness audit of the spec's borrow rules (2026-10-08)

*Research record. Non-normative. The spec decides every rule; this audit only checks whether it does. The run files here are the single record of the audit. The page is a generated view of them.*

## What it asks

Every cell asks one question: **can a value of a given kind reach a given position, and if it does, can a borrow it carries outlive its source?** There are 9 kinds of value (rows) and 13 positions (columns). A cell counts as *decided* only when current normative text (spec plus stdlib) settles both the verdict and the bound on the borrow, **and** the auditor could not write a program that escapes. Otherwise the cell is *missing*, *conflict* or *ambiguous*, comes with a counterexample, and is filed as an issue.

Some escapes are specific routes inside a cell (a stdlib copy member, a span over a field slot, and so on). Each of those is tracked as a separate *route cell* next to the 117 base cells.

## Runs

| Run | Spec | Base cells open | Route cells open | Outcome |
|---|---|---|---|---|
| 1 | v0.330 | 32 of 117 | — | 10 root causes (#5674–#5683), all decided in v0.331 (proposals 0148–0151) |
| 2 | v0.331 | 21 of 117 | 7 of 7 | #5688, #5708, #5709 and the span routes (#5691, #5693, #5706, #5710), all decided in v0.332 (proposals 0152–0155) |
| 3 | v0.332 | 3 of 117 | 0 of 7 | R6/C4 (#5726, new), R6/C9 and R6/C10 (#5715); also the editorial issue #5727; all decided in v0.333 (proposals 0156–0158) |
| 4 | v0.333 | 2 of 117 | 2 of 10 | R3/C4 and R6/C4 (#5739 equality search into `Equals(Object?)`, #5740 generic `as T` over a span), routes R6/C3·equality (#5739) and R2–R6/C11·loopvar (#5741); editorial items #5742; all decided in v0.334 (proposals 0159–0169) |
| 5 | v0.334 | 3 of 117 | 1 of 13 | R6/C4, R7/C4, R8/C4 on one root: an `is` test narrows a carrier that announces borrows with no conversion written (#5835); route R7–R8/C6·objmint (#5798) |
| 6 | v0.336 | 0 of 117 | 0 of 19 (one n/a) | targeted re-run (68 cells re-derived, 49 carried with `"carried": true`): v0.334's three R*/C4 cells decided by #5835 and proposal 0175, objmint by 0172; six new routes for the replacing `move`, the own-typed `Equals` slot, the span overloads and `where T: class`; editorial items filed as #5926 |

Each run re-derives every cell from the text current at that revision, rather than carrying the previous verdict forward, and records `prev_status` against the run before. From run 6 a run may be targeted: every cell whose quote is no longer verbatim, every open cell and every cell the revision's trains name is re-derived, and the rest is carried with `"carried": true` after its quotes check verbatim against the current text.

## Files

- `runs/v0.NNN.json`: one file per run.
  - `cells`: the base cells, in the cell contract below.
  - `routes`: route cells, with `route` and `issue` fields.
  - Open issues: `root_causes` (run 1) or `groups` (later runs), each listing the cells it opens.
  - `label` and `meta` for the page.
- `matrix.json`: the page's title, rows and columns. The interactive page is built by the shared builder in `../matrix-page/`: `python3 docs/research/matrix-page/build_page.py docs/research/borrow-flow-matrix scratch/borrow-flow-matrix.html` builds it from every run file. The page offers:
  - a run toggle;
  - a status filter and a "changed since the previous run" filter;
  - per-cell deciding text, notes and counterexamples;
  - the open issues and the route cells.

## How a run is done

There are five lanes, each covering a set of columns: C1–C3, C4–C5, C6–C7, C8 with C11, and C9/C10/C12/C13. Each lane writes its cells in the contract below and quotes every rule verbatim, with the quotes checked mechanically against the current spec and stdlib. The first run added two cross-checking lanes; later runs re-derive from the previous run's cells.

To re-run against a new revision, brief each lane as follows:
1. Start from the latest `runs/*.json`, taking the lane's own columns plus the route cells.
2. Read the newest changelog entry.
3. Re-derive and re-quote every cell at the new revision. A status may move either way, and lanes should actively hunt for cells the new text reopened.
4. Under proposal 0153, treat a runtime-checked route as *decided* (admitted-bounded by the flag), with the recorded K4.3 clause cited.

Then merge the lanes' output into a new `runs/v0.NNN.json` with `prev_status`, file each still-open cell as a spec issue, and add a row to the table above.


## Rows (value kinds)

- **R1**: owned class/interface value (named owned binding, `owned` param, fresh producer)
- **R2**: borrowed class/interface reference (plain param, borrow local, field read, element read, `this`)
- **R3**: borrow-bearing record (record with a class/interface/borrow-bearing component)
- **R4**: borrow-bearing (non-owning) inline array `[N]C`, `[N]Pair`
- **R5**: choice that holds borrows
- **R6**: span: `Span<T>`, `ReadOnlySpan<T>`, `IReadOnlySpan<TElement>` values (and string spans)
- **R7**: capture-bearing function value (lambda/closure/bound method reference with a non-empty source set)
- **R8**: bare type parameter value `T` in a generic body (substitution unknown)
- **R9**: control: runtime-managed (`string`, `Type`, boxed primitive) — expected "no borrow, unrestricted"

## Columns (positions)

- **C1**: instance field store (class), incl. property/indexer setter backing store, `this`-rooted
- **C2**: static field / static property store
- **C3**: collection slot store (Add/indexer set/collection literal element/GetOrAdd) — borrowing and owning collections
- **C4**: implicit/explicit conversion to `Object` or an interface (boxing), incl. as argument, local, `as`, upcast
- **C5**: `ref`/`out` argument location (callee writes into caller's location)
- **C6**: return from a method / getter / lambda (incl. expression body), and bare-T returns
- **C7**: closure capture (lambda/local function captures it; then the closure flows elsewhere)
- **C8**: type-argument substitution: value stored at a generic `T` position (generic field, `Box<T>`, generic method param)
- **C9**: async frame: live across `await`, async method parameter, `Task<T>` result
- **C10**: iterator frame: live across `yield return`, iterator parameter, yielded value
- **C11**: pattern / `as` / `is` binding, and choice-payload / record-component construction (aggregation into a value)
- **C12**: cross-domain: `Domain.Spawn` argument/result, `Channel<T>` element, `Locked<T>` interior (sendability)
- **C13**: `scoped` parameter / call-bounded storage (`scoped` field, §28.14)

## Cell contract (one JSON object per cell)

```json
{"row":"R3","col":"C4","verdict":"rejected|admitted|admitted-bounded|conditional|n/a",
 "rule":"the deciding sentence(s), verbatim quote(s) ≤ 60 words each","cite":"§x.y[, stdlib §a.b]",
 "status":"decided|missing|conflict|ambiguous|n/a",
 "note":"one or two sentences: what bounds the borrow, or why the cell is open",
 "counterexample":"for missing/conflict/ambiguous: a minimal Bezel program (≤ 25 lines) showing the escape or the
                   two readings; else empty"}
```

- **decided**: exactly one rule (or a consistent set) decides the verdict AND the borrow's lifetime bound.
- **missing**: no text decides whether the value may reach the position, or reaches it with no stated bound.
- **conflict**: two passages give different verdicts.
- **ambiguous**: one passage, two defensible readings with different verdicts.
- **n/a**: the kind cannot reach the position by construction (cite why).

Quote verbatim (grep to confirm). Re-locate every citation; never cite from memory. Check Appendix C rows, the kernel
(K-rules, axioms), DECLINED.md (a declined alternative tells you the intended verdict), and the v0.316–v0.330 changelog
entries (current rule only; they're context, not authority).
Hunt actively: for each cell try to write the escaping program. A cell is "decided" only if you could not.
