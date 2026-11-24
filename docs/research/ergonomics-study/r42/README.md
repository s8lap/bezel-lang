# Ergonomics recheck on r42

*November 2026. Compiler r42 (`bezelc 0.0.0-8ccf7ff0`, origin/main 8ccf7ff06), checked disposal mode, compared side by side with r40 (`bezelc-0.0.0-a30bb11d`). Spec, stdlib and guides v0.359. Evidence only: no proposals, no issues filed.*

## One page for the owner

**What is fixed.** Of the first study's 24 compiler bugs, **19 are fixed** and one (B16) is now rejected correctly at compile time. All 9 silent wrong-output bugs except B1 are gone: the OrderBy keys (B4), the `DictionaryEntry` sorts (B5), the string use-after-free (B6), the untyped comparer (B7), decimal `Sum` (B13) and the checked-mode false fault on sets (B11) all behave as the spec says. `ParseOrNull`, `NameOf`/`FromNameOrNull`, the stream span overloads and `DateOnly.ToString(format)` now exist. Every must-use false positive the study met (B2, B12, B20, B23) is gone.

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

The documented not-yet-implemented items are all still missing: `Counter`, `Json`, `Locked`, `typeof`/`GetType`, and stored `GroupBy`.

**What is new.** 23 new tasks (`tasks/n*.md`) and 71 program files. **All 23 reach the expected output, 6 of them through a compromise.** Only 7 first drafts ran unchanged. There are 40 friction rows (`data/new-frictions.tsv`):

| Cause | COMPILER-BUG | LANG-RULE | USER-ERROR | VERBOSE/AWKWARD | MISSING | DIAGNOSTIC |
|---|---|---|---|---|---|---|
| Rows | 22 | 7 | 4 | 4 | 2 | 1 |

These rows hold **17 distinct bugs** (`data/new-compiler-bugs.md`). Ten have no existing issue. The worst:

- **Concurrency hangs and losses.** A bounded channel with `SendAsync` and two producers loses items and hangs (NB11). Four or more tasks waiting in `Semaphore.AcquireAsync` stall the program, timers included (NB12).
- **Generic code.** In a generic class at `K = string`, `Remove` with a key read from the dictionary itself returns false (NB7, silent). `GetOrNull` over a generic-record value emits malformed IR (NB6). `where T: IEntity<K>` is not satisfied by `User: IEntity<int>` (NB9).
- **Serialization and closures.** An array of choices serializes but does not deserialize past its first element (NB5). Forwarding a `scoped` handler into an `EventSource` over-releases the closure (NB13).

**Doc-vs-compiler gaps.** The v0.359 error-handling story is mostly implemented. `ParseOrNull`, the `Result` combinators, bare `Ok`/`Error`, the `??` rule, `OverflowException` from `Parse` and catching by name all work exactly as Guide 2 and the code-generation reference teach. Four things the documents teach without a "not yet implemented" marker fail on r42:

- `IFailure` is an unresolved type name (#8414). The documented boundary shape, a marked user exception taken by an `IFailure` arm after `OperationCanceledException => throw`, cannot be written. It hit four tasks (n01, n03, n05, n13).
- `string.IsEmpty`, which codegen §5.9 lists and uses in its own idiom (#2348).
- `Exception.ToString()` omits the message (#3302).
- Collection-literal locals of records or choices cannot be moved into an `owned` parameter (#8498), although the spec calls the literal a fresh owned production.

The missing-file behaviour (#7057) is documented as not yet implemented, but the idiom built on it (catch `FileNotFoundException` at the call) still silently never runs.

**Issue state mismatches.**

- **Closed, but the repro (or its named family) still fails on r42:**
  - #8383 (B1, via NB16)
  - #6627 (B3; its residual is open #8542)
  - #755 (stored `GroupBy` is still rejected, citing #755)
  - #487 (the reflection message still cites it as the tracker)
- **Open, but the study's repro now passes (hand-close candidates):**
  - #8386 (B6; confirm under ASan, which this box lacks)
  - #8394 (B24)
  - #1394 (B15, span half)
  - #2240 (B21, the format half; the Counter leg cannot pass yet)
  - #6314 (B11; it is an umbrella, so re-check its other legs first)

**Updated top design themes** (first-study rows still present plus the new rows, same scoring formula):

1. **Diagnostics cascade** (14). Some messages are better: the retired `case … end` spelling now gives one error, and `out` names its rule. But a nested case still gives 215 lines (t34 v2) and an unknown `"""` literal gives 17.
2. **Ownership outside resources** (12). The rules are unchanged: fresh tasks into an `Array`, `move` markers, the declared-before rule. The bug amplification is gone.
3. **Serialization shape** (8). The class route now works. Still missing: positional `JsonOptions`, a naming policy, a per-field default for `optional`, and an object-end probe.
4. **Closures cannot outlive the call** (8). Comparers become object graphs (n11), and a capturing handler cannot subscribe through a published `IEvent` property (n17).
5. **Text stdlib gaps** (7).
6. **Expression forms** (5).
7. **Query surface** (5). `OrderBy(f).First()` is at least correct now.
8. **Tuples** (4).
9. **Unused results** (4).
10. **Small rules** (4).

Enum names dropped out of the top ten, because `NameOf` now works.

---

## Method

- **Part 1.** Every program of the first study (`../programs/*/v*.bzl` and `final.bzl`, 130 files) was compiled with both compilers as `bezelc --checked-disposal -o <exe> <file>` and run with the task's input. t01, t02 and t04 take stdin, and t04 takes three argument sets. Scratch paths were redirected to this lane. Output was compared with the first study's `data/final-outputs.txt` and, for t15 and t40, with the task statement (`data/recheck-tasks.tsv`).
- **Bug repros.** Each B1–B24 repro block in `../data/compiler-bugs.md` was run on both compilers. B11, B15 and B18 used the study programs they name. Every issue state was read through the REST API (`data/recheck-bugs.tsv`).
- **Part 2.** I wrote the 23 tasks before any code, from the code-generation reference §§1–6 and the guides. I did not read §7 until every row was classified. Every draft is kept, and each revision carries a comment at the changed line (`// r42: …` marks a compiler-bug workaround).
  - Borrow-holding tasks (n09, n11, n12, n17, n18) were also built with `--report-trust-admissions`.
  - n04 was built with `--checked-contracts`.
  - The final runs are in `data/new-final-outputs.txt` (trust lines truncated).
- **Classification.** The causes and the 1/2/3 weight follow the first study. Each suspected bug was reduced to a minimal repro and searched for with REST (`search/issues`). Three rows of `notes` kind (things that went well, the trust-report observation) are in the text below, not in the TSV.
- **Not done.** There is no ASan runtime on this box (`libclang_rt.asan` is missing), so B6's fix rests on a 300-row stress, not on a sanitizer. NB11/NB12 timings were not explored beyond the threshold. No issues were filed.

## Part 1: per-task recheck

Changed tasks (the rest behave exactly as on r40; full table in `data/recheck-tasks.tsv`):

| Task | r40 → r42 |
|---|---|
| t02 | v2–v5 now correct (B4/B5/B6/B7); v1 still wrong (NB16) |
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
- One gap: a plain primary-constructor parameter stored by a field initializer was not reported (NB14, #8395 family).

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
