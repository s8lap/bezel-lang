# Tracker measurement for the master plan — 2026-10-27

*Companion to [`master-plan.md`](../master-plan.md). Every number here was produced by the scripts under
`work/` from a full REST pull of the `s8lap/bezel-lang` tracker taken 2026-10-27 (1613 open, 5339
closed, 6952 issues; PRs excluded) and from `git log origin/main` (shallow clone: 1735 commits,
2026-10-04 → 2026-10-27). Evidence tiers: **MEASURED** = this lane ran it; **QUOTED** = read from issue
text or a document. Counts over issue titles/labels/bodies are QUOTED evidence about the tracker, not
about the compiler.*

## How to re-run

```bash
cd docs/proposals/papers/master-plan/work
./fetch_issues.sh all              # ~70 REST pages, explicit page=N (GraphQL blocked, --paginate unreliable)
python3 -I measure.py > out/measurements.txt      # labels, areas, severity, §-chapters, age, siblings,
                                                  # spec-tracking, weekly open/close, clusters, landing rhythm
python3 -I campaigns.py > out/campaign-members.md # exclusive campaign assignment + member lists
python3 -I campaigns.py --tsv > out/campaign-members.tsv
python3 -I sept_plan_outcome.py                   # state today of every issue the 2026-09 defect plan named
```

`measure.py` writes `out/clusters.tsv` (one row per open issue with its overlapping keyword clusters)
and `out/weekly.tsv`. The raw pull is in `work/data/` (`issues-open.jsonl`, `issues-closed.jsonl`,
`pages/`). The scripts are QUOTED-evidence tools: a cluster is a title regex, and the campaign
assignment is a triage aid whose verdict the campaign's entry check replaces (plan §2.2).

## 1. Headline numbers

| Measure | Value | Source |
|---|---|---|
| Open issues | **1613** (5339 closed; 5259 completed, 75 not planned, 5 duplicate) | tracker |
| Open 6 weeks ago (week 38, 2026-09-14..20) | 555 → **1613** today (×2.9) | reconstructed from created/closed dates |
| Opened / closed per week, last 6 full weeks | W39 589/397 · W40 292/283 · W41 431/353 · W42 626/343 · **W43 770/297** · W44 (2 days) 121/98 | `out/weekly.tsv` |
| Opened in the last 7 / 14 / 30 days and still open | 443 / 702 / 1045 | created_at |
| Median open age | **14 days** (30–90 d: 412; 90–180 d: 156; none older; oldest #446 at 143 d) | created_at |
| Self-declared siblings / residuals | **957 of 1613 (59%)** match sibling / residual / found-while / pre-existing / follow-up / twin / same-mechanism; 430 titles name another issue number | title+body regex |
| Same measure on 2026-09-17 | 144 of 552 (26%) | QUOTED, #4453 via `defect-campaign-2026-09.md` §0 |
| Of the 702 opened in the last 14 days | 444 (63%) describe a review / probe / found-while origin, 34 a matrix run, 31 a spec train, 21 the corpus, 172 other | body keywords |
| Time to close (issues closed in the last 30 days) | median **1 day**, mean 3.6, p90 7 days | closed_at − created_at |
| Fix commits on `origin/main`, 2026-10-04..27 (24 days) | **1131 fix/narrow/fix-forward (47/day)**, 315 test/bank, 216 other, 37 infra, 31 spec; 870 distinct issues named in fix subjects | `git log` |
| Fix commits by `src/` area touched | sema 489 · codegen 475 · ownership 85 · runtime 42 · common 26 · module 19 · parser 18 | `git log --name-only` |
| Soundness-grade open | **263 `soundness`/`memory-safety` labelled + 45 by title = 308**; leak 154 + 112 = 266; compiler crash 34 + 60 = 94; "silent" 49; over-accept 37; over-reject 31 | labels, titles |
| `spec-tracking` open | **259**; 14 filed today for v0.345, 22 for v0.339, 18 for v0.338, 24 for v0.334, 18 for v0.321; 111 read as questions | labels, titles |
| Rule corpus (`tests/conformance/rules/baseline.tsv`) | 3396 banked rows: 3156 pass, **240 fail** (recorded divergences); 69 fail rows carry an `issue:` header | baseline.tsv, manifest.tsv |
| Fail rows by document chapter | stdlib §2: 25 · spec §28: 24 (§28.3 9, §28.12 5, §28.14 4, §28.10 2, §28.6 2) · stdlib §6: 23 · stdlib §4: 22 · spec §8: 12 · §9: 9 · §29: 7 · §30: 7 · §32: 7 · stdlib §9: 6 · stdlib §5: 1 | manifest join (212 of 240 join); §28 subsections by case-id prefix |
| Discussion | median 0 comments per open issue (1028 have none); median body 2161 chars (none under 200) | tracker |

**Reading.** Throughput is not the problem: the pool lands ~47 fixes a day and closes an issue a median
of one day after it is filed. The surface grows because intake is 1.5–2.6× closure, and 63% of the
recent intake is produced by the process itself (reviewers' and producers' probes, which the 2026-10-04
review contract made mandatory). Each narrow fix is correct and reviewed, and is followed by a filed
sibling. That is the loop the owner describes as "working, just very slowly".

## 2. Open issues by label and area

Labels (an issue carries several): bug 1057 · codegen 567 · sema 353 · ownership 283 · soundness 262 ·
spec-tracking 259 · leak 168 · stdlib 142 · test-infra 81 · enhancement 79 · async 51 · cross-module 42 ·
compiler-crash 40 · iterators 31 · RFC 28 · documentation 27 · rule-corpus 16 · serialization 12 ·
parser 11 · design-review 7 · runtime 7 · spec 7 · unconfirmed 5 · spec-question 2 · spec-defect 1 ·
needs-owner-decision 0 · **no label 113**.

Area (first component label, else title keywords): codegen 660 · sema 375 · ownership 153 · stdlib 86 ·
test-infra 84 · unclassified 71 · spec 63 · spec-tracking-only 41 · enhancement/design 39 · docs 11 ·
async 11 · cross-module 8 · serialization 3 · ecosystem 3 · parser 3 · iterators 1 · runtime 1.

## 3. Spec chapters cited in open titles

1249 `§` citations in 905 of 1613 titles. Chapters: **§28 273** (§28.3 115, §28.9 43, §28.14 16,
§28.11 16, §28.12 14, §28.6 13, §28.2 12) · §22 102 (§22.6 30, §22.12 15, §22.3 12, §22.10 10) · §9 75
(§9.2 25, §9.7 16) · §14 67 (§14.2 30, §14.1 19, §14.5 13) · §27 64 (§27.6 25, §27.5 23) · §8 63 (§8.6 26)
· §7 58 (§7.4 25) · §12 56 (§12.7 17, §12.1 16) · §30 54 (§30.5 17, §30.1 12) · §2 48 · §5 46 (§5.1 38) ·
§15 36 (§15.4 17, §15.1 11) · §11 32 · §29 32 (§29.2 9, §29.11 9) · §35 31 · §19 29 (§19.3 22) · §4 28 ·
§17 23 · §3 20 (§3.6 14) · §21 17 · §26 16 · §23 14 · §31 11. Stdlib: §5 9, §6.5 8, §4.14 6, §2.2 5.

## 4. Topic clusters (overlapping keyword match over titles + component labels)

| cluster | open | soundness | crash | leak | spec-trk | opened 30d | closed 30d | closed ever | median age d |
|---|---|---|---|---|---|---|---|---|---|
| disposal-scope-end | 349 | 96 | 8 | 136 | 55 | 244 | 241 | 961 | 14 |
| string-format-parse | 287 | 41 | 23 | 68 | 39 | 228 | 213 | 708 | 6 |
| generics-instantiation | 268 | 57 | 18 | 21 | 62 | 200 | 320 | 825 | 14 |
| numeric-ops | 266 | 59 | 28 | 18 | 32 | 219 | 283 | 861 | 7 |
| owned-collections-literals | 222 | 49 | 15 | 58 | 31 | 173 | 191 | 688 | 12 |
| interfaces-dispatch-extensions | 201 | 41 | 12 | 16 | 46 | 162 | 217 | 672 | 13 |
| nullable-narrowing | 192 | 39 | 17 | 14 | 32 | 134 | 183 | 671 | 14 |
| records-equality-hash | 190 | 33 | 10 | 26 | 45 | 141 | 210 | 722 | 14 |
| spans-borrows | 190 | 71 | 3 | 29 | 49 | 127 | 203 | 669 | 19 |
| test-infra-gate | 160 | 18 | 7 | 15 | 14 | 72 | 71 | 309 | 34 |
| closures-captures | 137 | 34 | 8 | 55 | 11 | 82 | 100 | 422 | 19 |
| async-tasks | 137 | 42 | 8 | 27 | 27 | 72 | 80 | 416 | 24 |
| iterators-enumerables | 125 | 41 | 10 | 30 | 16 | 72 | 82 | 404 | 14 |
| diagnostics-wording | 110 | 11 | 4 | 16 | 12 | 76 | 73 | 286 | 10 |
| cross-module | 103 | 21 | 12 | 9 | 10 | 66 | 100 | 457 | 13 |
| statics-init | 96 | 14 | 3 | 17 | 14 | 61 | 100 | 384 | 14 |
| exceptions-unwind | 91 | 17 | 4 | 13 | 21 | 54 | 78 | 408 | 14 |
| choice-match | 87 | 21 | 9 | 14 | 23 | 66 | 68 | 211 | 14 |
| contracts-pure | 33 | 9 | 0 | 2 | 6 | 17 | 17 | 116 | 25 |
| serialization | 21 | 2 | 0 | 0 | 5 | 5 | 7 | 68 | 77 |
| parser-grammar | 19 | 1 | 0 | 1 | 7 | 10 | 30 | 129 | 24 |
| extern-ffi | 18 | 4 | 3 | 1 | 2 | 9 | 12 | 103 | 34 |

Fastest-growing by 30-day intake relative to closure: owned-collections-literals and string-format-parse
(both filed faster than closed), choice-match (66 vs 68), disposal-scope-end (244 vs 241). Generics,
nullable narrowing, cross-module and statics closed more than they opened in the window.

Mechanisms named in titles (camelCase identifiers): `checkRead` 4, `checkAssignable` 3,
`isStringFallbackTarget` 2, `lowerTypeTest` 2, `typeRefEqual` 2, then one each of ~45 others. Source
files named in titles/bodies: `ExprTypecheck_Calls.cpp` **110**, `LlvmBackend_Setup.cpp` **100**,
`Ownership_Checks.cpp` 75, `ExprTypecheck.cpp` 67, `TypeResolution_Checks.cpp` 66, `Ownership.cpp` 57,
`LlvmBackend_Calls.cpp` 40, `LlvmBackend_Stmts.cpp` 40, `LlvmBackend_Assign.cpp` 36,
`bezel_runtime_collections.cpp` 32, `LlvmBackend_Layout.cpp` 28, `LlvmBackend_Disposal.cpp` 23.

## 5. Exclusive campaign assignment (first match wins; `work/campaigns.py`)

| campaign | open | soundness | crash | leak | spec-trk | opened 30d | closed 30d | closed ever |
|---|---|---|---|---|---|---|---|---|
| infra-oracles (enablers) | 166 | 11 | 8 | 7 | 13 | 71 | 86 | 392 |
| using-heads | 24 | 7 | 3 | 5 | 2 | 14 | 8 | 25 |
| enumerable-heads | 125 | 43 | 9 | 27 | 3 | 81 | 117 | 496 |
| producer-temporaries | 148 | 14 | 7 | 98 | 0 | 116 | 140 | 678 |
| builtin-call-admission | 189 | 33 | 53 | 0 | 0 | 159 | 231 | 649 |
| closures-cells | 111 | 24 | 3 | 49 | 0 | 67 | 84 | 299 |
| collection-elements | 77 | 13 | 1 | 30 | 1 | 67 | 54 | 204 |
| nullable-carrier | 54 | 14 | 0 | 0 | 9 | 42 | 96 | 312 |
| generic-boundaries | 88 | 17 | 13 | 3 | 0 | 49 | 160 | 423 |
| borrow-provenance | 96 | 30 | 1 | 17 | 0 | 39 | 99 | 313 |
| choice-payloads | 30 | 10 | 6 | 10 | 0 | 22 | 6 | 19 |
| async-frames | 32 | 16 | 1 | 7 | 8 | 23 | 13 | 81 |
| cross-module | 25 | 9 | 2 | 4 | 1 | 19 | 20 | 164 |
| records-statics-misc | 49 | 9 | 0 | 1 | 0 | 36 | 78 | 387 |
| spec-trains (unclaimed spec-tracking) | 222 | 48 | 3 | 1 | 222 | 177 | 59 | 60 |
| features-proposals | 99 | 0 | 1 | 1 | 0 | 10 | 5 | 180 |
| unassigned (parking lot) | 78 | 15 | 1 | 1 | 0 | 59 | 168 | 657 |

The member lists are in `work/out/campaign-members.md` (seeded members marked `S`; the rest matched by
rule). The `closed ever` column is the strongest evidence for the loop: enumerable-heads has 496 closed
members against 125 open, producer-temporaries 678 against 148, builtin-call-admission 649 against 189.

## 6. What happened to the 2026-09 defect-campaign families

`work/sept_plan_outcome.py`, state today of every issue each family section named (QUOTED lists, MEASURED
states):

| family | named | closed | still open |
|---|---|---|---|
| A capture cells (anchor #2718) | 43 | 17 | **26** |
| B `checkRead` ordering | 10 | 2 | 8 |
| C owned/borrow returns | 7 | 1 | 6 |
| D CFG-opaque walkers (anchor #4146) | 7 | 2 | 5 |
| E literal initializers | 18 | 10 | 8 |
| F boxes | 8 | 3 | 5 |
| G enumerable heads | 21 | 2 | **19** |
| H `await using` | 10 | 0 | **10** |
| I1 substitution walkers (anchor #4212) | 8 | 5 | 3 |
| I2 erased-template layout | 13 | 1 | 12 |
| I3 unit type at a generic formal | 9 | 1 | 8 |
| J classification recursion (anchors #4140/#4125) | 8 | 5 | 3 |
| K silent degrade (anchor #2924) | 24 | 9 | 15 |
| L memoized emitters | 7 | 3 | 4 |
| M release-eligibility predicates | 17 | 2 | 15 |

Anchors **#2718, #4146, #4212, #4140, #4125** closed within four days of the plan (2026-09-18..21); the
oracle-repair items #3733, #3682, #3732, #3892, #3895 closed the same week; #3460 was re-measured and
closed. Still open: #2924 (the `internal:` disposition record), #3493 (the `await using` dispatch
ruling), #1877/#3379 (the §28.9 foundation ruling), #4453 (triage-by-family), #4452 (oracle repair
umbrella), #3917, #4383 (the classification ran: #4555 banked 1736 pass / 413 divergent). Only the I1
lane wrote the lane record §7.6 asked for.

## 7. Spot checks on the pre-release compiler (`bezelc 0.0.0-f6863e62`, static build)

| issue | repro | result | tier |
|---|---|---|---|
| #3469 (G, 61 d) | the issue's first block (lazy-pipeline getter) | now **rejected at compile time** with the §28.6 getter diagnostic — the SIGSEGV is unreachable from this repro; #6451 says the fence, not a fix, is what changed | MEASURED |
| #7297 (builtin arms, 1 d) | `Math.Floor(d)` with `d: double?` | **reproduces**: malformed IR, `IR verification failed` | MEASURED |
| #7322 (using-heads, 0 d) | `await using var t := o.ToString();` | **reproduces**: sema warns the `using var` is vacuous, the binary **segfaults** (rc 139) without ASan | MEASURED |
| #7187 (temporaries, 3 d) | while-condition temporary | exits 65 (200001 mod 256) as the body dictates; the leak itself not observed: `--sanitize=address` is refused by the self-contained link and `--link=system` failed to link in this container | MEASURED (run), leak NOT MEASURED |

Lesson for the plan's entry check: of four headline repros, one has already moved (#3469). Re-measuring
every member on HEAD before a campaign starts is not optional.
