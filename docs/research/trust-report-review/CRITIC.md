# v0.350 critic and completion — proposal-0131 review train

> **Landing note (2026-10-28).** Paths under `scratch/` name working files that were not committed.


Base: origin/main `5a984a691` (spec v0.349; the docs are byte-identical to the review's `293b9278c`, the one commit between them touches only `papers/master-plan.md`). Tree: `scratch/v0350/work/tree` (exported, `git init`, `v0350.json` applied, proposal copied in). Compiler: r35 `bezelc 0.0.0-f6863e62`, `--checked-disposal --report-trust-admissions`. Nothing posted, committed, or edited under `docs/`/`tests/` of any real checkout.

## Verdicts

| Item | Class | Verdict |
|---|---|---|
| E1 `r0131-rationale-5.31-comparer` | editorial | **ACCEPT** (unchanged) |
| E2 `r0131-spec-28.11-local-collection-not-reported` | clarification | **ACCEPT-WITH-FIXES** (narrowed; a hole as drafted) |
| E3 `r0131-fcg-4.6-comparer` | editorial | **ACCEPT-WITH-FIXES** (composed with E2's exception and 0210's entry, plus the residual) |
| E4 `r0131-readme-row-0131-currency` | editorial | **ACCEPT-WITH-FIXES** (extended with the v0.350 items) |
| E5 `r0131-proposal-0131-currency-note` | editorial | **ACCEPT-WITH-FIXES** (item (v) rewritten, landing date, tracker numbers, new item (viii)) |
| Proposal 0210 | design change (report-only) | **ACCEPT-WITH-FIXES** — clearly an improvement once re-scoped: no verdict moves, zero measured migration, and it replaces a false sentence (§28.3.9 clause 1 / K4) with a site plus a precisely stated residual |

## Findings

**F1 (blocking, E2). The drafted local-collection exception opened a hole in the zero-report claim.** It exempted every parameter-, receiver- or field-path-sourced element stored into a non-escaping local collection, on the ground "bounded by the scope comparison (K2.3)". K2.3 bounds the element's *scope*, not its *object*: a body can take a field's value out with its own replacing `move` and transfer it while the local collection still holds the element — exactly 0210's shape with a collection holder:
`tmp.Add(_f); var c := move _f with new Q(); Domain.Spawn(W, move c); tmp[0].M();`. The current text reports the store; the drafted text would have made silence normative, and 0210 (keyed on a direct-read *local*) would not catch it either. Also §28.11's residual paragraph just above the sentence still attaches the residual and the domain clause to "each admitted element". MEASURED with r35-compilable twins (the `move` in a same-class method, since r35 cannot parse the one-body form): `work/probes/f02_field_elem_local_coll_take.bzl` and `f03_…` — accepted, 0 report lines (r35 also misses these field-spelled sources, #7467), runs race (exit 0 / `DisposedAccessException` across 12 runs each). **Fix:** the exception covers a plain parameter or `this`, *the binding itself*, for which the body holds no obligation and so no kill in it can transfer the object; an element read through a field path stays reported, with the reason stated. This still covers proposal 0131 §3's traced row and b07 (`tmp.Add(item)`), and it is narrower than 0131 §1.2's parenthesis (recorded in E5 (v) and the entry). Classification stays clarification (it states 0131 §3's row; it decides nothing beyond it); the stricter reading is why it was narrowed rather than widened.

**F2 (blocking, 0210). The draft's rule did not cover its own example.** §1's program, §3 row 1 and the "MEASURED" line used `var b := a.F` (a property) and `a.Take()` (the `move` inside a callee) — e02, the r35-compilable proxy — while the rule text and §7 key on a borrow and a `move` in one body, and §5 called the rule body-local. The cross-body form is not one-body decidable at all (the caller cannot see that `F` returns `_f` or that `Take` moves it; the callee has no local borrow). So the draft's thesis "the K4 enumeration sentence is true" was false. **Fix:** examples are #7434's one-body program (`var b := a.f; var c := move a.f with new Q(); …`); the callee form is a silent §3 row; the residual (aliases/closures/borrow-bearing values over the local, member reads, a callee's `move`) is stated in §28.3.9 clause 1, in K4's sentence ("… at every site, the slot kill by move's residual that §28.3.9 *Scope* clause 1 states apart") and among the zero-report claim's exclusions. That is not DECLINED 0157-option-B's "narrowed claim" (that row's ground was a hole one body decides; this one no body decides) — recorded in the rationale paragraph and in the alt-1 decline row.

**F3 (0210 measurement).** "accepted in both builds, 0 report lines, runs to exit 0 (the race read returns the payload)" — re-run 20× (flag build) and 20× (default build): 10 exit 0 / 10 exit 70 `DisposedAccessException` in each, i.e. a race between the spawned domain's auto-disposal of `q` and the read. Also `probes/results.md`'s e02 row says REJECT (both builds) with an empty diagnostic list: it predates the probe's `sealed` payload (the `.noflag.compile.txt` shows the old §30.14 sendability rejection) — stale. Corrected in the proposal.

**F4 (0210 precision).** "Live" was undefined → *used (read, member-accessed, passed, re-borrowed, captured) on a path from the transfer*; the borrow was "a local borrow of the slot's current value" → exactly §28.7's *Use after a dominating field write* local (`var|const b := r.f`, `r` this/local/non-`ref` parameter, one segment, `r` not assigned between); a `move` standing directly in a transfer position (`Domain.Spawn(W, move a.f with new Q())`) had no site → it is the site; Q2's owning-collection slot leg (recommended yes) → declined for v1 (index identity is not a path fact; §28.7 is field-only; 0206 left the slot leg to run time). Table row "`this._peer := a.f`" was rejected-not-reported when `a` is a local → qualified "`a` a plain parameter".

**F5 (0210 reach).** The draft's `grep Domain.Spawn` (236 sites) did not measure the shape. `work/scan0210.py`: 743 slot-`move` sites in `tests/` and `docs/`; 10 with a direct-read local of the same path within 25 lines before; none transfers the taken value while that local is used. The only instance is the bzcore README's own F1 witness. Zero report lines.

**F6 (agreement checks, 0210).** K4.3: derivation, no new clause (the slot kill is §28.3.9's already-recorded obligation). §28.3.12: the entry reuses 0157's rows and "freeze leaving it admitted" construction; inserted after 0157's sentence, so the 13 rule cases' `rule:` quotes (all prefixes ending at "K3.12)" or at "K3.11, each runtime-checked under §28.2)") are untouched — `corpus.py lint --strict` clean. 0157: same transfer-not-kill ground; 0210 adds a liveness condition 0157 lacks, justified (a local's uses are body-local, a collection's retention is not — rationale §5.31's own sentence). #7434: its witness is now the proposal's first row; option 2 is the deferred row. DECLINED: no decline reopened — row 46 (slot overwrite posture, flag-backstopped) concerns overwrites, untouched; 1072 (freezing the write) and 1073 (widening 0206) concern errors, and 0210 cites 1073's ground for not reaching aliases; 790–793 (0157 options) are consistent.

**F7 (E3/FCG).** "every borrow of a parameter, `this` or a field you store in … a collection" became false under E2 → "(a method-local collection holding the parameter or `this` itself excepted)"; 0210's entry added; "a unit that stays silent … holds no trusted lifetime at all" given the residual. The edit stays inside the existing sentence (the document's maintenance rule forbids appended sentences, not edits within one).

**F8 (E5).** "(a v0.349 review edit proposes it)" would be stale on landing → item (v) rewritten for the narrowed exception; "landed on 2026-10-13" → 2026-10-12 (`cd21669f8` committer date; author date 2026-10-09); "the review's issues name them" → #7465–#7471; new item (viii) for 0210. E1's citation (proposal 0138 §7, v0.327) and E4's facts (#5604; `s28_3_0162`–`0167` are 0157's cases; v0.334/0169; v0.346/0207) verified.

**F9 (numbering).** 0210 had no file on main but was on the README's never-used list, reserved for the v0.346 train. At the owner's direction (2026-10-28) the proposal keeps 0210, and the never-used list drops 0210–0213: they were reserved and never allocated, so removing them leaves the numbering chronological. The pinned v0.346 changelog line stays as dated history.

**Not in scope / unchanged.** Appendix C lists no report sites (checked); Guide 01 §2's report sentence is generic and stays true; §28.8.1's "bounded afterwards by the taker" needs no change; the review's other candidates (callee-side certain dangles, always-on) stay declined on recorded grounds and get no new rows.

## What I produced

- `scratch/v0350/v0350.json` — 17 edits (5 review edits, 3 of them revised, + 12 for 0210 incl. README/DECLINED), one sequence, every `old` unique at the base; `apply_doc_edits.py`: 17/17 clean. Generator: `work/build_json.py`.
- `scratch/v0350/proposals/0210-report-a-transfer-of-a-slot-displaced-value.md` — final proposal, status `accepted — accepted (2026-10-28, delegated ruling) …`, §10 records this critique.
- README: 0210 row in *Accepted* (5 columns, after 0106); never-used sentence. DECLINED: 4 `declined-at-proposal` rows (narrow the claim for every slot kill; a line at the `move`; the slot leg; reaching the residual) + 1 `deferred` (freezing the transfer, #7434 option 2, bar #7432); index size 1091 → 1096 (137 deferred, 337 declined-at-proposal).
- `scratch/v0350/entry.md` — the v0.350 entry (v0.346 shape for the proposal, v0.349 shape for the editorial items), lists #7465–#7472, the #5702 comment, the #4602 third correction, and `{{ISSUE:0210-impl}}`.
- `scratch/v0350/trackers.json` — the 0210 tracker (`key` `0210-impl`; labels `bug`, `ownership`, `spec-tracking`, `campaign:borrow-provenance`, all existing; campaign 9 of master-plan §3).
- `work/flip_0210.py <N>` — the landing flip (proposal Status → `graduated-v0.350 — superseded — …`, row moved to *Graduated* after 0209 in the 4-column form, tracker number filled in).
- Probes: `work/probes/f01–f03` (+ re-runs of e02/e03/e09/b07/b08/b10/b11/a01/a04/c01/c03/e01, consistent with results.md except e02, F3).

## Checks

On `work/tree` (edits applied, proposal copied): `check_docs.sh` OK (EXTRACT 553, BOOK clean); `check_proposals.sh` OK (201 ↔ 201; accepted 22 / graduated 151); `check_src_citations.py` clean; `corpus.py lint --strict` 3,693 / 0 problems; `corpus.py check` up to date; `build_book.py --check` clean.

Landing simulation (copy; `{"0210-impl": 9999}`; `finalize.py 0.349 0.350`; the flip): `check_docs.sh` OK (VERSION v0.350 single entry, PIN v0.350, EXTRACT 553, BOOK 198 pages clean); `check_proposals.sh` OK (accepted 21 / graduated 152); src citations, corpus lint/check and book clean. Re-run after switching to `flip_0210.py`: `check_docs.sh --quick` and `check_proposals.sh` OK (the full run was not repeated: the root volume hit 0 MB free mid-run — another copy I was making failed and was deleted at once; the simulation copy was then removed to free 176 MB).
