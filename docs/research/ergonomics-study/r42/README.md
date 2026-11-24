# Ergonomics recheck on r42

*November 2026. Compiler r42 (`bezelc 0.0.0-8ccf7ff0`, origin/main 8ccf7ff06), checked disposal mode, compared side by side with r40 (`bezelc-0.0.0-a30bb11d`). Spec, stdlib and guides v0.359. Evidence only: no proposals, no issues filed.*

## One page for the owner

**What is fixed.** Of the first study's 24 compiler bugs, **19 are fixed** and one (B16) is now rejected correctly at compile time. All 9 silent wrong-output bugs except B1 are gone: the OrderBy keys (B4), the `DictionaryEntry` sorts (B5), the string use-after-free (B6), the untyped comparer (B7), decimal `Sum` (B13) and the checked-mode false fault on sets (B11) all behave as the spec says. `ParseOrNull`, `NameOf`/`FromNameOrNull`, the stream span overloads on `FileStream` (not yet on a `Stream`-typed receiver, #1394) and `DateOnly.ToString(format)` now exist. Every must-use false positive the study met (B2, B12, B20, B23) is gone.

| | r40 | r42 |
|---|---|---|
| final programs with the expected output | 38 of 40 | **39 of 40** (t15 now passes; t40 is still half: `Locked<T>` #1721) |
| first drafts (v1) with the expected output | 11 | **17** |
| all 130 drafts with the expected output | 77 | **95** |
| drafts that got worse | — | **0** |

Of the 96 friction rows, **33 are resolved, 6 changed, 57 still present** (`data/recheck-frictions.tsv`). Every resolved row was resolved by a compiler fix. No design theme of the first study was resolved by a document change: the v0.356–v0.359 revisions (the error-handling story, `ParseOrNull` as doctrine, the `??` rule) touch only F021/F043 (now working) and F063 (the `??` rule is now stated and enforced).

**What is still broken from the first study.** B8 and B17 (both contested spec questions). Two bugs changed rather than went away:

- **B1:** `ReadToEnd()` works, but a string member on a local inferred from it (`raw.ToLower()`, `.Trim()`, or the chained `Console.In.ReadToEnd().ToLower()`) still lowers to nothing, with only an internal warning (NB16).
- **B3:** the key lambda works, but an untyped lambda over a group's elements is typed as the key (`g.Sum(lambda(e) => e.Salary)`), so t03 v1/v2 and t11 v1 still fail (NB17, open #8542).

The documented not-yet-implemented items are all still missing: `Counter`, `Json`, `Locked`, `typeof`/`GetType`. Stored `GroupBy` results are still rejected too, but no document marks them as not implemented (a gap, below).

**What is new.** 23 new tasks (`tasks/n*.md`) and 71 program files. **All 23 reach the expected output, 6 of them through a compromise.** Only 7 first drafts ran unchanged. There are 40 friction rows (`data/new-frictions.tsv`):

| Cause | COMPILER-BUG | LANG-RULE | USER-ERROR | VERBOSE/AWKWARD | MISSING | DIAGNOSTIC |
|---|---|---|---|---|---|---|
| Rows | 22 | 6 | 5 | 4 | 2 | 1 |

These rows hold **17 distinct bugs** (`data/new-compiler-bugs.md`). Nine have no matching issue at all; a tenth, NB16, matches only the closed #8383. The worst:

- **Concurrency losses and hangs.** A bounded channel with `SendAsync` and two producers silently loses items: at capacity 4 the sum is 25 instead of 165, in every run. At capacity 1 it usually hangs (NB11). Four or more tasks waiting in `Semaphore.AcquireAsync` stall the program, timers included (NB12).
- **Generic code.** In a generic class at `K = string`, `Remove` with a key read from the dictionary itself returns false (NB7, silent). `GetOrNull` over a generic-record value emits malformed IR (NB6). `where T: IEntity<K>` is not satisfied by `User: IEntity<int>` (NB9).
- **Serialization and closures.** An array of choices serializes but does not deserialize past its first element (NB5). Forwarding a `scoped` handler into an `EventSource` over-releases the closure (NB13).

**Doc-vs-compiler gaps.** The v0.359 error-handling story is mostly implemented. `ParseOrNull`, the `Result` combinators, bare `Ok`/`Error`, the `??` rule, `OverflowException` from `Parse` and catching by name all work exactly as Guide 2 and the code-generation reference teach. These fail on r42, and the documents carry no "not yet implemented" marker for them:

- **A marker is owed** (a stdlib surface, spec-change process §5 item 3):
  - `string.IsEmpty` (#2348): stdlib §9.6 and the reference §5.9, whose own idiom uses it.
- **A marker is probably owed:**
  - `IFailure` (#8414): stdlib §1 and §2.17. It is an unresolved type name, so the documented boundary shape cannot be written: a marked user exception taken by an `IFailure` arm after `OperationCanceledException => throw`. It hit four tasks (n01, n03, n05, n13).
- **No marker is owed; report these as gaps:**
  - `Exception.ToString()` omits the message (#3302). This is a spec §31.4 rule, not a stdlib surface.
  - Collection-literal locals of records or choices cannot be moved into an `owned` parameter (#8498), although the spec calls the literal a fresh owned production. This is an ownership bug.
  - Stored `GroupBy` results are rejected (open tracker #4694). No document marks this, and stdlib §5 and Guide 05 assume a `GroupBy` result can be stored.

The missing-file behaviour (#7057) is documented as not yet implemented. Even so, the idiom built on it (catch `FileNotFoundException` at the call) silently never runs.

**Issue states.** No closed issue's own repro fails on r42. Recommended actions:

- **No reopens.**
  - #8383 and #6627: their own repros pass on r42. The failures above are new shapes.
    - File NB16 as a new issue, citing fad5f5e31; closed #8383 covered only the direct path.
    - Comment NB17 on open #8542, whose own repro fails with the same message.
  - #755 and #487: open successors already exist (#4694 for stored `GroupBy`; #3819 and #4630 for reflection). Repoint the trackers the diagnostics cite.
- **Keep open, and comment:**
  - #1394: the issue's own repro still fails on a `Stream`-typed receiver (`s.Read(b.AsSpan())` and the windowed `s.Read(b, 0, 4)` "produced no value"). Only `FileStream` works (b870b63de), and its Count-window guards do raise.
  - #8394: the typed-lambda form with no type arguments is still rejected, and residual R1 (a same-arity overload) still prints `ann` silently.
- **Keep open; no comment needed:**
  - #6314: the t15 leg passes, but its other 13 legs were not checked.
  - #8386: B6 passes. Close it only after an ASan loop, which this box cannot run; its remaining residuals live in #8484.
  - #2240: no action.

**Updated top design themes** (first-study rows still present plus the new rows, same scoring formula):

1. **Diagnostics cascade** (14). Some messages are better: the retired `case … end` spelling now gives one error, and `out` names its rule. But a nested case still gives 215 lines (t34 v2) and an unknown `"""` literal gives 17.
2. **Ownership outside resources** (12). The rules are unchanged: fresh tasks into an `Array`, `move` markers, the declared-before rule. The bug amplification is gone.
3. **Serialization shape** (8). The class route now works. Still missing: positional `JsonOptions`, a naming policy, a per-field default for `optional`, and an object-end probe.
4. **Closures cannot outlive the call** (8). Comparers become object graphs (n11), and a capturing handler cannot subscribe through a published `IEvent` property (n17).
5. **Text stdlib gaps** (7).
6. **Expression forms** (5).
7. **Query surface** (5). `OrderBy(f).First()` is at least correct now.
8. **Tuples** (4).
9. **Small rules** (4).
10. **Exception boilerplate** (3).

Enum names (3, tied with 10 and placed 11th by the tie-break) and unused results (3 rows; n18's discard turned out to be unneeded) dropped out of the top ten. Enum names fell because `NameOf` now works.

---

## Method

- **Part 1.** Every program of the first study (`../programs/*/v*.bzl` and `final.bzl`, 130 files) was compiled with both compilers as `bezelc --checked-disposal -o <exe> <file>` and run with the task's input. t01, t02 and t04 take stdin, and t04 takes three argument sets. Scratch paths were redirected to this lane. Output was compared with the first study's `data/final-outputs.txt` and, for t15 and t40, with the task statement (`data/recheck-tasks.tsv`).
- **Bug repros.** Each B1–B24 repro block in `../data/compiler-bugs.md` was run on both compilers. B11, B15 and B18 used the study programs they name. Every issue state was read through the REST API (`data/recheck-bugs.tsv`).
- **Part 2.** I wrote the 23 tasks before any code, from the code-generation reference §§1–6 and the guides. I did not read §7 until every row was classified. Every draft is kept, and each revision carries a comment at the changed line (`// r42: …` marks a compiler-bug workaround).
  - Borrow-holding tasks (n09, n11, n12, n17, n18) were also built with `--report-trust-admissions`.
  - n04 was built with `--checked-contracts`.
  - The final runs are in `data/new-final-outputs.txt` (trust lines truncated).
- **Classification.** The causes and the 1/2/3 weight follow the first study. Each suspected bug was reduced to a minimal repro and searched for with REST (`search/issues`). Three rows of `notes` kind (things that went well, the trust-report observation) are in the text below, not in the TSV.
- **Not done.** There is no ASan runtime on this box (`libclang_rt.asan` is missing), so B6's fix rests on a 300-row stress, not on a sanitizer. NB12's threshold was not explored further, and NB11's run counts are the reviewing critic's. No issues were filed.
- **Review.** A non-author critic gave ACCEPT-W-NOTES; every repro and count reproduced. This version applies its notes:
  - the issue-state recommendations above;
  - NB14 is a duplicate of #7467;
  - N033 is reclassified as USER-ERROR;
  - the marker owed/not-owed split;
  - the NB11 and NB12 wording;
  - t02 v1 is rejected, not wrong.

## Part 1: per-task recheck

Changed tasks (the rest behave exactly as on r40; full table in `data/recheck-tasks.tsv`):

| Task | r40 → r42 |
|---|---|
| t02 | v2–v5 now correct (B4/B5/B6/B7); v1 still rejected ("codegen for this for statement" after the NB16 internal warning) |
| t03 | v3 now correct; v1/v2 now stop at NB17 instead of the IR failure |
| t05, t09, t19 | the `ParseOrNull`/`NameOf` errors are gone; the language-rule errors remain |
| t06, t08, t37 | v1 now correct (B6, B4, B13) |
| t10, t16, t23, t35 | the must-use warnings are gone (B2, B23, B20) |
| t11 | v2 now correct; v1 stops at NB17 |
| t15 | **all seven drafts correct**, 20 of 20 runs each (B11) |
| t18 | v1 compiles and is correct (B12) |
| t21 | v1 is now a compile error naming §15.4's concrete targets (B16); the `File.Delete` warnings are gone (B14) |
| t22 | v1 (span I/O) compiles and is correct (B15) |
| t24 | v2 no longer hits B19; it now fails on its own constructor shape |
| t29 | the B22 error and the B18 internal warning are gone |
| t33 | the Stopwatch error is gone (B12); the `WhenAll` rule remains |

**What felt good still holds.** All 30 rows of `../data/good.tsv` reproduce on r42, since every program they cite still compiles and runs correctly. New ones from Part 2:

- n02: `Result` with bare `Ok`/`Error` returns, `??=`, `ParseOrNull`, `{v:F2}`.
- n06: a class with an `owned Array` of records round-trips JSON on the first draft.
- n07: `serialized "name" optional`.
- n04: contracts with `old`, `result` and a `pure` helper; `requires` holds in release builds.
- n13: `TaskScope` with first-fault cancellation and value-index captures.
- n15: deadline cancellation through `WithCancelAfter`.
- n18: a parent back-pointer in an owned tree.
- n22: the `with` copy.
- n23: `double.ParseOrNull`.

Most diagnostics named their fix: the §28.14 *declare `alarm` before `thermo`* message, the retired `case` spelling and the `out` keyword.

## Part 2: new tasks

| Task | Drafts | Outcome | Main friction |
|---|---|---|---|
| n01 config (throwing form + `ParseOrNull` dual) | 2 | ok | `IFailure` NB1; `IsEmpty` NB2 |
| n02 import (Result for decoded errors) | 2 | ok, compromised (`File.Exists` pre-check) | missing file reads empty (#7057) |
| n03 async job runner (boundary arms) | 3 | ok | NB1; `GetType` NYI; `Exception.ToString` NB3 |
| n04 contracts | 2 | ok | user error (`Decimal.Round`) |
| n05 exception hierarchy | 2 | ok | NB1; wrapping needs two explicit constructors |
| n06 nested JSON order | 1 | ok | — |
| n07 tolerant JSON | 2 | ok | positional `JsonOptions`, per-field renames, no `optional` default |
| n08 JSON array of choices | 3 | ok, compromised (record + enum tag) | NB4, NB5 |
| n09 generic TTL cache | 4 | ok | NB6, NB7 |
| n10 generic heap | 2 | ok | `out` reserved; NB8 |
| n11 composed comparers | 1 | ok | closures cannot be returned, so 3 classes |
| n12 generic repository | 2 | ok, compromised (key fixed to `int`) | NB9, NB10 |
| n13 TaskScope fan-out | 2 | ok | NB1 |
| n14 channel fan-in | 3 | ok, compromised (unbounded, no back-pressure) | sender-minting trap; NB11 |
| n15 deadline over five lookups | 1 | ok | five named task holders |
| n16 semaphore limiter | 2 | ok, compromised (two waves) | NB12 |
| n17 observer | 4 | ok, compromised (event source moved to the caller) | §28.14 getter rule; NB13; NB14 |
| n18 tree with parent pointers | 1 | ok | — |
| n19 template renderer | 3 | ok | B8 family in a case arm |
| n20 logfmt parser | 1 | ok | no regex; `Counter` NYI |
| n21 word wrap | 1 | ok | — |
| n22 todo CLI | 3 | ok | `=> return` arm; NB15 |
| n23 stats CLI | 1 | ok | — |

**Trust-admission report (proposal 0212 stage 1).**

- The five borrow-holding finals drew 5 report lines in all: one per back-reference or helper store (n09 1, n11 2, n17 1, n18 1, n12 0). Every line was at a constructor store, which is the shape the code-generation reference §4.4 recommends.
- Each line is about 720 characters, and names the obligation and five static remedies.
- The cheapest remedy worked as named: `scoped clock` at the parameter and the call site silenced n09's line, with no other change.
- One gap: a plain primary-constructor parameter stored by a field initializer was not reported (NB14, an exact duplicate of open #7467).

**§7 after the fact.** The code-generation reference §7 documents only n15's named-holder `WhenAll` shape (§7.2). None of the other new frictions is there:

- the sender-minting trap;
- the getter as a bounding object;
- `IFailure` being unimplemented;
- the `optional` default.

## Files

- `data/recheck-tasks.tsv`: per task, the final and first-draft verdicts on both compilers, per-draft status, and what changed.
- `data/recheck-bugs.tsv`: B1–B24 and the documented NYI items, with dispositions, issue states and mismatches.
- `data/recheck-frictions.tsv`: the 96 first-study rows, re-classified on r42 + v0.359.
- `data/new-frictions.tsv`: 40 new rows (same columns as the first study, plus `existing_issue`).
- `data/new-compiler-bugs.md`: NB1–NB17 with minimal repros and issue matches.
- `data/new-final-outputs.txt`: the final run of every new program.
- `tasks/n*.md`, `programs/n*/v*.bzl` and `final.bzl`.
