# The conformance corpus

Every fenced `bezel` code block in the ten normative documents, extracted into
one `.bzl` case each and swept by the gate: the docs' own examples are executable
claims, and this corpus is where they get checked against `bezelc`.

- **Extractor**: `scripts/extract_doc_examples.py` (also run — against a scratch
  tree, never this directory — by `scripts/check_docs.sh` stage 4, per #1380).
- **Runner**: `conformance.py` via `run_conformance.sh` / `run_conformance.py`;
  wired into the gate by `scripts/gate_full_cycle.sh`. Unit tests:
  `test_conformance.py`.
- **Consumed as**: `manifest.tsv` (TAB-separated: `case-id doc section class
  expectation notes`) + `cases/<case-id>.bzl` + `preamble.bzl` (stub types
  spliced into cases whose `notes` field is exactly `preamble`).

Case ids are `<doc-tag>_<NNNN>` where `NNNN` is the block's position in its
document **at the extraction that produced the corpus** — ids are stable between
regenerations but a regeneration renumbers; treat `manifest.tsv` (id → doc,
section, class) as the identity record, and cite cases together with the corpus
revision (below).

---

## Regeneration record — v0.247 (2026-08-11)

Previous corpus: a v0.138-era extraction (hand-maintained skip lists; kept
syntax-current in place by the v0.208 paren-exprs and v0.217 keyword-lambda
migrations). This regeneration re-extracted everything from the v0.247 docs and
re-derived the skip list from scratch.

**Classification oracle.** `build/bezelc` dated 2026-08-04 (last src/ commit in
that build ≈ `8440cf62`; ten later bugfix commits, `2e42dc9d`..`3fd24a85`, are
in the tree but not in the binary — recorded staleness, accepted because this
lane must not rebuild on the shared box). Every non-skip case was compiled (and
run, where the wrap produces an executable) through the harness's own
`classify_wrap`/`splice_preamble`/`evaluate_run` with the systemd cage swapped
for a plain `timeout 30` + 8 GiB rlimit. Consequence: a handful of verdicts
could flip under a fresh build; the first gate cycle after this lands is the
acceptance run.

**Counts.**

| | old (v0.138-era) | new (v0.247) |
|---|---|---|
| manifest rows | 437 (+6 orphan case files) | 567 |
| active (error/fragment/program) | 100 | **138** |
| skip | 337 | 429 |
| preamble-spliced | 11 | 15 |

Content matching old→new (whitespace-normalized exact, then difflib ≥ 0.70
within the same doc): 370 old cases map into the new corpus (230 exact, 140
fuzzy), 67 retired with their doc blocks, 197 new cases have no predecessor.
Of the 90 old *active* cases that map, 88 still pass; the 2 that do not are
genuine docs-ahead drift (now `blocked-on-#3052` / `#3045`-family rows).
All nine of the old corpus's `blocked-on` issues (#645 #970 #971 #974 #975
#976 #977 #980 #992) are closed; every one of those skips was re-derived
empirically rather than carried — most now compile and are live rows.

**Skip vocabulary and derivation.** Extractor-static labels: `skip:non-bezel`,
`skip:empty`, `skip:ebnf-grammar`, `skip:elided`, `skip:signature`. For every
case that failed the compile pass, rules applied in order (first match wins;
where a case has several blockers the dominant — first-reported — one is
recorded):

1. **R1** ≥2 `module` decls in one block → `skip:multi-module` (a compilation-
   unit example; not compilable as one file).
2. **R2** strict docs-ahead feature match → `blocked-on-#NNNN (docs-ahead: …)`:
   `equality skip` → #3050; `closed`/`mod=` → #3052; identifier-operand
   `move x` + parse error → #3056 (v0.246 marker unification — flips at
   catch-up phase 4); `result`/`old` unresolved in `ensures` → #2942;
   `FromUnderlyingOrNull` member error → #3044; `for`-source rejection in a
   `pure` body → #3051; §6.6 checked write-back compound assignment → #2741
   (cf. #2985).
3. **R3** unresolved name that is stdlib-documented surface →
   `blocked-on-<tracking-issue> (stdlib-unresolved: Name)` where a tracking
   issue is known (verified open 2026-08-11: `Stream`/`StreamReader` #1394,
   `HttpClient` #1376, `Locked`/`RwLocked` #1721, `FrozenArray` #1723,
   `int.ParseOrNull` #2347), else `skip:stdlib-unresolved (Name)` (`Ptr`,
   `Predicate`, `IOrderedEnumerable`, `Parallel`, `string.FromCStringPointer`,
   `IEnumerable<T>` static-receiver method references).
4. **R4** unresolved illustrative name (prose-context types/locals the block
   assumes) → `skip:incomplete-fragment`; single-uppercase meta-names (`U`,
   `E`…) → `skip:signature-listing`.
5. **R5** "does not implement inherited abstract" / "must have a body" /
   reserved-primitive-alias / const-without-init → `skip:signature-listing`
   (API sketches with elided bodies).
6. **R6** `::=` in source → `skip:ebnf-grammar` (grammar the extractor's
   threshold missed).
7. **R7** top-level declarations *and* top-level statements in one block →
   `skip:mixed` (no single wrap can hold both; generalizes #683's
   extension-block finding).
8. **R8** otherwise, the matched old case's finer hand label
   (`skip:syntax-snippet` etc.) carried with provenance.
9. **R9** bodyless-stub density → `skip:signature-listing`.
10. **R10** hand ruling (22 cases), including: `skip:not-implemented` for
    language/FFI features with no tracking issue found (method-local `record`
    declarations; §32 callback env-slot form; §32 trailing-arg extern form;
    extern record-typed param marshalling ×4) and `skip:doc-defect` for
    spec_0291 (§32.8 names an extern parameter `base`, a reserved keyword —
    filed as #3067).

An `error`-class case is live iff the classification compiler rejects it (the
harness checks exit code only, not the specific diagnostic — pre-existing
limitation). All 26 error cases reject; none of the docs' newer reject-side
rules had accepted-anyway examples in fragment form.

**Harness changes shipped with the regeneration** (all in `conformance.py`,
unit-pinned in `test_conformance.py`; `run_conformance.sh` is a thin exec
wrapper, unchanged):

- `choice`, `type` aliases, and `extern` blocks dispatch to the lib wrap
  (TopDecls per spec Appendix A.1; previously fell to the stmt wrap and could
  never compile).
- `public|private|internal extension …` recognized (§26.1 spells visibility).
- `pure` admitted in the method/ctor modifier run of the wrap heuristic.
- Import-led fragments: the leading import block is hoisted and the remainder
  dispatches on its own shape (statements now land inside `Main`; previously
  stranded at module level).
- `splice_preamble` inserts after the module line *and* any leading imports
  (`SourceFile ::= ModuleDecl Import* TopDecl*`, A.1).
- `preamble.bzl`: `Parser` stub gained a `FileStream` constructor and
  `ReadAll()` (the Guide 1/2 `Import` examples). Stubs are only extended for
  cases the old corpus already served or trivial arity gaps — new illustrative
  types are *not* stubbed retroactively (those cases stay
  `skip:incomplete-fragment`).

Of the 138 live rows, 89 are old active cases carried through and 49 are newly
live: 16 flipped by the five wrap/splice fixes plus the preamble extension, the
rest closed compiler gaps since v0.138 (the old `blocked-on` roster) and new
doc blocks that compile as written. Of the old corpus's 100 active rows, 11 are
no longer live: 10 retired with doc blocks the intervening revisions rewrote
(their successors are among the new cases), and one (old `spec_0038` → new
`spec_0046`) is genuine docs-ahead drift, now `blocked-on-#3052`.

**#3028 collision note (implementation run, phase 1).** The catch-up run's
#3028 item plans to "update conformance spec_0062/spec_0256". This regeneration
**renumbered both**: old `spec_0061`/`spec_0062` (§15.4.1 `FromCollectionLiteral`)
are now **`spec_0072`/`spec_0073`**, and old `spec_0256` (§30.11
`LoadResourceAsync`) is now **`spec_0267`** (old `spec_0255` retired with its
doc block; old `spec_0257` → `spec_0268`). The case texts already carry the
v0.241-ruled pre-identifier `owned` spelling — re-extracted from the fixed
docs — so the #3028 lane's remaining corpus work is re-evaluating those rows,
not editing their text. Today `spec_0072` is live (`fragment`), `spec_0073` is
`skip:signature-listing`, `spec_0267` is `skip:incomplete-fragment`
(illustrative `StartLoadInBackground`/`Resource` context).

**Known dormant classes for future flips.** `blocked-on-#3056` (10 cases) is
the largest docs-ahead class and flips wholesale when the marker-unification
catch-up lands; #3056's own corpus-rewrite step should re-run the sweep rather
than hand-edit. The `blocked-on-#1394/#1721/#1723/#1376/#2347` rows flip as
those stdlib surfaces register. `skip:incomplete-fragment` (175) and the
signature/snippet classes are permanent-by-nature unless the docs change shape
or the preamble grows.

**Regeneration procedure** (for the next time): run the extractor into a
scratch tree; content-match against the committed corpus for provenance;
compile-classify every non-skip case through `conformance.py`'s own wrap
functions with the then-current `bezelc`; derive skips by the rules above
(hand-rule the residue — this pass needed 22); verify manifest↔cases bijection,
unit tests, `scripts/check_docs.sh`, and a gate-path smoke of representative
cases; record oracle identity and counts here. Scratch artifacts for this pass
(verdicts, derivation table, per-case failure logs) were left in
`/var/tmp/bezel-scratch/conf-regen/`.
