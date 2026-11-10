# Ergonomics study: 40 everyday programs against r40

*November 2026. Compiler r40 (`bezelc-0.0.0-a30bb11d`, origin/main a30bb11df), checked disposal mode. Spec and stdlib v0.355. Evidence only: no proposals.*

## One page for the owner

I wrote 40 small programs of the kind a services-and-tools developer writes, the way someone coming from C#, Kotlin, Rust or TypeScript would write them after reading `Bezel_For_Code_Generation.md` §§1–6. Then I compiled and ran each one, and fixed it until the output matched the task. Every draft is kept.

**Outcome.** 38 of 40 programs reached the expected output. One of these, t24, needed a data-model compromise. t40 is half done: the mutex half cannot be expressed today. t15 never ran correctly, because of compiler bug B11. Only 11 first drafts ran correctly. There are 90 drafts in all.

**Headline counts.** There are 96 friction rows (`data/frictions.tsv`). Each row has one cause:

| Cause | Rows |
|---|---|
| COMPILER-BUG | 45 (24 distinct bugs in `data/compiler-bugs.md`, 9 of them silent wrong output; plus 6 rows on documented not-yet-implemented items) |
| LANG-RULE | 23 |
| MISSING | 10 |
| DIAGNOSTIC | 8 |
| VERBOSE/AWKWARD | 7 |
| USER-ERROR | 3 |

There are also 30 "felt good" rows (`data/good.tsv`).

**The ten design themes.** Each theme scores the sum, over the distinct tasks it touched, of the highest row weight (1/2/3) in that task. Only LANG-RULE, MISSING, VERBOSE/AWKWARD and DIAGNOSTIC rows count. Ties go to the higher maximum weight, then to more tasks. `python3 data/rank.py data/frictions.tsv` re-derives both rankings. Compiler bugs are ranked separately below, by the same formula.

| # | Theme | Score | Rows / tasks | Size |
|---|---|---|---|---|
| 1 | **Diagnostics point at the wrong place or cascade.** Examples: the B1 internal warning is followed by an error at an unrelated `for`; 60 parse errors follow a nested `case`; 5 conversion errors hide the real call-bounded cause (t28). | 13 | 8 / 8 | compiler only |
| 2 | **Ownership friction outside resource code** (for the ownership lane; no fixes designed here). Fresh tasks cannot go into an `Array`, so `WhenAll([...])` takes 5 lines. Nested dictionaries need `OwningDictionary`. A closure cannot move a captured sender into `TaskScope.Spawn`. | 10 | 7 / 5 | — |
| 3 | **Serialization shape.** A record cannot hold a list. There is no end-of-object probe for hand-driven JSON. `JsonOptions` has five positional booleans with no defaults, and there is no naming policy. | 6 | 4 design + 2 bug / 2 | small + stdlib |
| 4 | **Query surface.** There is no `MaxBy`/`MinBy`; proposal 0026 D2 is under review. `Array.Count(pred)` is shadowed by the `Count` property; `CountWhere` is "declined, evidence-gated", and this study is that evidence. | 5 | 4 / 4 | stdlib only |
| 5 | **Text-processing stdlib gaps.** There is no whitespace split, no regex (proposal 0020 is accepted but not landed), and no CSV reader. | 5 | 3 / 3 | stdlib only |
| 6 | **Expression-form limits.** A bare `Ok(...)` cannot sit in an `if`-expression (#3042, recorded "liftable"). Expression `case` arms cannot hold statements. A nested `case` needs a `do` wrapper. | 4 | 4 / 3 | small language change |
| 7 | **No ad-hoc products or multiple returns.** One-use records appear only to carry a projection; `Partition` uses two `out` parameters. Tuples are deferred, and D-0002's bar is "evidence from real code". | 4 | 2 / 2 | large |
| 8 | **Closures cannot outlive their call.** Comparer combinators are impossible. This is spec §28.14's own *Known v1 limitation*; the deferred unbounded tier's bar is partly met. | 3 | 1 / 1 | large |
| 9 | **Enum and case names.** `ToString` renders the type name by design, and a choice has no case-name string. The bigger cost, `NameOf` and the values list missing, is implementation (B10, #3819). | 3 | 2 design + 3 bug / 3 | small |
| 10 | **Unused-result warnings on mutators.** `_ :=` is needed before `Remove`, `AddLast`, `BeginArray` and `Dispatch`. | 3 | 3 / 3 | small |

Counting and upsert fell out of the top ten. Most of that friction is `Counter<K>`, which is specified but not implemented (#3848), so it is ranked with the bugs. What is left by design is the non-counting upsert in t16 (score 1).

**Compiler bugs, ranked by cluster** (same formula over COMPILER-BUG rows):

1. **Members the stdlib specifies but r40 lacks: score 11, 7 tasks.** These are `ParseOrNull`, `NameOf`, `ReadToEnd`, the stream span overloads (#1394) and `DateOnly.ToString(format)` (#2240).
2. **Documented not-yet-implemented items: score 10, 6 tasks.** These are `Counter`, the `Json` facade, `Locked<T>`, `typeof(E).EnumValues` and stored `GroupBy` results.
3. **Silent wrong query results: score 10, 5 tasks.** `OrderBy` ignores string, double and decimal keys (B4). Decimal `Sum` is wrong (B13). Sorting `DictionaryEntry` arrays does nothing (B5). An untyped string comparer does nothing (B7).
4. **Strings freed with their source: score 9, 5 tasks.** This is a use-after-free (B6). Separately, `Select(x => $"{x}")` is rejected (B17, contested).
5. **Inference: score 8, 4 tasks.** B24 is silent: `MaxBy(people, lambda(p) => p.Age)` returns the wrong element. The others are B3, B22 and B18.

**Keep these.** The `*OrNull` + `?.` + `??` tier, choices with exhaustive `case` and guards, `Result` with `AndThen` method references, interpolation with alignment and format, cancellation scopes, channels that close when the last sender is dropped, `Domain.Spawn` with value results, deterministic destructors, reflection-free serialization synthesis, and FFI without attributes. 11 first drafts ran unchanged.

---

## Method

- **Tasks.** `tasks/` holds 40 one-paragraph statements, written before any code, with expected output. Two errata were found while running. Neither changes a program: 2026-11-10 is a Tuesday (t38), and the error position in t35 is 9, not 10.
- **Drafts.** `programs/<task>/v1.bzl … vN.bzl` and `final.bzl`. Each revision carries a comment at the changed line naming the reason (`// r40: …` marks a compiler-bug workaround). I did not read §7 of the code-generation reference until all rows were classified.
- **Build.** `bezelc --checked-disposal -o <exe> <file>`, then run. The final run of every program, including diagnostics, is in `data/final-outputs.txt`.
- **Classification.** Each row has exactly one cause. A suspected bug was reduced to a minimal repro before it was called COMPILER-BUG. Two first readings did not survive a re-check, and were reclassified as DIAGNOSTIC: a `String.Equals` cascade (t09) and a `SkipValue` misreading (t25).
- **Ranking.** Each row carries a `weight`: 1 when characters were added, 2 when lines were added or code restructured, 3 when the design changed or the task could not be done. Each row also carries a `report_theme` (design rows) or a `bug_cluster` (COMPILER-BUG rows). A theme's score is the sum, over its distinct tasks, of the highest weight in that task. Ties go to the higher maximum weight, then to more tasks. `data/rank.py` computes it. The weights are judgement; the rows are the evidence.
- **Not-yet-implemented items.** Six rows meet a feature the spec already defines and r40 has not implemented, with a tracker: `Counter` #3848, `Json` #2349, `Locked` #1721, `typeof` values #3819, stored `GroupBy` #755. They are all COMPILER-BUG (cluster `C-documented-nyi`), because the design question is answered and the fix is implementation.
- **Review.** A non-author critic reviewed the first version (ACCEPT-W-NOTES). This version applies its notes: three reclassifications (F038, F069, F049), the NYI rule above, the weights, corrected counts and citations, and existing-issue links in `data/compiler-bugs.md`.

## Per-task outcome

| Task | Drafts | Outcome | Main friction |
|---|---|---|---|
| t01 wc | 1 | ok | no whitespace split |
| t02 word frequency | 6 | ok | B1, B4, B5, B6, B7; no Counter |
| t03 CSV | 4 | ok | B3, B4, B6; hand-written CSV |
| t04 grep | 3 | ok | B8 (narrowing in an if-expression) |
| t05 INI | 2 | ok | owned field; nested `OwningDictionary`; B9 |
| t06 log summary | 2 | ok | B6 |
| t07 Money record | 1 | ok | — |
| t08 shapes (choice) | 2 | ok | B4; no MaxBy |
| t09 enum | 2 | ok | B10; no values list |
| t10 validation | 1 | ok | B2 warning |
| t11 group-by | 3 | ok | B3, B13, B4; projection record |
| t12 top-N | 1 | ok | — |
| t13 join | 2 | ok | `Count(pred)` shadowed |
| t14 histogram | 1 | ok | tally dance |
| t15 sets | 6 | **failed** | B11 (false `DisposedAccessException`) |
| t16 inventory | 1 | ok | upsert dance; B23 (query must-use) |
| t17 parse-or-fail | 1 | ok | — |
| t18 retry | 2 | ok | B12 |
| t19 Result pipeline | 2 | ok | bare case in if-expression; B9 |
| t20 exceptions | 1 | ok | — |
| t21 file numbering | 2 | ok | B16 (literal silently writes nothing) |
| t22 binary file | 2 | ok | B15 (stream span I/O); manual byte encoding (the `ArraySpan` accessors were available) |
| t23 directory list | 2 | ok | `Count(pred)`; no file size |
| t24 JSON round-trip | 3 | ok, compromised (tags fixed at 2) | record cannot hold a list; B19; facade NYI |
| t25 JSON hand-read | 5 | ok via a declared record | no object-end probe |
| t26 plugin registry | 1 | ok | unused-result warning |
| t27 LRU cache | 2 | ok | `??` across types; unused results |
| t28 comparer | 3 | ok without combinators | closures cannot be returned; B24 |
| t29 partition | 2 | ok | no tuples; B17, B22 |
| t30 worker pool | 3 | ok | fresh tasks into an Array |
| t31 channel pipeline | 3 | ok without TaskScope | closure cannot move an owned sender |
| t32 timeout | 1 | ok | — |
| t33 fan-out | 2 | ok | `WhenAll` over fresh tasks; B12 |
| t34 state machine | 3 | ok | statement arms; nested `case` |
| t35 calculator | 3 | ok | recursive choice rejected; B20 |
| t36 connection pool | 2 | ok | `move` at owned argument |
| t37 table | 2 | ok | B13 |
| t38 dates | 2 | ok via DateTime | DateOnly has no arithmetic; B10, B21 |
| t39 FFI | 1 | ok | — |
| t40 lock counter | 2 | **half** (atomic only) | `Locked<T>` not implemented |

## The ranked theme map

Row ids (`F0nn`) refer to `data/frictions.tsv`. "Decline status" is what rationale §17/§18 and `docs/proposals/DECLINED.md` say about the theme.

### 1. Diagnostics: correct rule, wrong place

- **Evidence:** F004, F018, F027, F060, F066, F077, F085, F095. Eight tasks, score 13. F073, an internal warning, is counted under bugs (B18).
- **Pattern 1: the real cause is buried.** t28 reported five "not implicitly convertible" errors before the call-bounded cause. t40 reported "Shared<T> requires T to be a proper class type: `<unknown>`" and four "not sendable" errors after one unresolved name. t34 reported about 60 parse errors after one correct first line.
- **Pattern 2: the error is at the use, not the producer.** In t04 the error is at `hay.Contains`, with the remedy "`hay!`". The actual cause is the if-expression above it (B8).
- **Pattern 3: an internal degradation is shown as a warning, and the error appears somewhere else.** In t02, `ReadToEnd` became zero, and the error was reported at a `for` statement.
- **Pattern 4: end-of-structure is reported as something else.** In t25, the closing `}` was reported as "Unknown field" or "Unexpected JSON token in skip".
- **Counter-evidence: many messages were excellent.** Examples: §26.5 `Count`, the interpolation-hole generic comma, the `move` marker, and the fresh-store messages. The fresh-store messages name the remedy exactly.
- **Peers:** rustc and the Kotlin compiler suppress follow-on errors from an unresolved name, and attach notes to the root cause.
- **Decline status:** none.
- **Size:** compiler only. Error recovery and suppression of poisoned types; no language change.

### 2. Ownership friction outside resource code (record only, for the ownership lane)

- **Evidence (LANG-RULE):**
  - F019: a field initialized with a fresh collection needs `owned`.
  - F020: `Dictionary<string, Dictionary<…>>` must become `OwningDictionary`, and then has no entry iteration.
  - F074, F081: fresh tasks cannot go into a borrowing `Array`, so `await Task.WhenAll([F(1), F(2), F(3)])` becomes five lines. The class-typed result also needs a named task.
  - F078: an async lambda cannot move a captured owned sender, so the channel pipeline gave up on `TaskScope`.
  - F079: owned parameters that rely on teardown warn.
  - F089: `move` is required at an owned argument.
- **Bug-amplified (COMPILER-BUG):** F015, F030, F038, F040, F052, F080, F088, through B2, B12, B20 and B23. (F038, the query must-use warning, was first filed here; the spec says the query was spent, so it is B23.) These warnings and errors appear where the spec says nothing should.
- **Weight:** score 10, 7 rows in 5 tasks (t05, t30, t31, t33, t36), none of them about resources. The two costliest were the task fan-out and the captured-sender case. `using var` and the `move` markers themselves were cheap. The diagnostics usually named the fix.
- **Peers:**
  - C#: `await Task.WhenAll(F(1), F(2))`.
  - Kotlin: `awaitAll(async { … }, …)`.
  - Go: `go produce(ch)` with `close(ch)`.
- **Decline status:** §7.2 of the code-generation reference documents the task-holder and fresh-store shapes. Proposal 0082's `--experimental-scoped-await-bind` touches F081.
- **Size:** not assessed (the ownership lane owns it).

### 3. Serialization shape

- **Evidence** (6 rows in 2 tasks; F053, F057, F059 and F061 score 6; F054 is documented NYI and F055 is B19):
  - F053: `record Person(…, Tags: Array<string>): ISerializable` is rejected. Heap collections are class components only (spec §9.8).
  - F055: the class form was also rejected (B19), so the record took `[2]string`.
  - F059: `IDeserializer` cannot iterate an object's fields of unknown count ("driven by the object's textual extent", but no API). Hand-driven tolerant parsing went through a declared record instead.
  - F057: `new JsonOptions(false, 2, false, true, 64)`. Positional records admit no defaults.
  - F061: no naming policy, so record components were written in lower case to match `"id"`.
  - F054: the one-shot facade `Json` is not implemented (#2349).
- **Peers:** `@Serializable data class` (Kotlin), serde `#[serde(rename_all)]`, `JsonSerializerOptions { … }`.
- **Decline status:** the record clause is a deliberate §9.8/§9.7 rule. The other items were never considered.
- **Size:** stdlib (object-end probe, options with defaults, naming policy). A small language change would be needed for defaults in positional records.

### 4. Query surface: MaxBy and the `Count(pred)` shadow

- **Evidence** (4 rows, 4 tasks, score 5):
  - F024, F068: no `MaxBy`/`MinBy`. The code-generation reference offers `OrderBy(f).First()`, which is O(n log n), and here also wrong because of B4.
  - F035, F050: `arr.Count(pred)` is unreachable on `Array`, `Set` and `Dictionary`, so I wrote `.Where(pred).Count()`.
- **Peers:** .NET 6+ `MaxBy`, Kotlin `maxBy`, Rust `max_by_key`.
- **Decline status:** rationale §15: "`CountWhere` is declined, evidence-gated … The reopening bar: field evidence that the collision costs real users real time on the `Count`/`Contains` names specifically". The collision hit 2 of 40 tasks, and the code-generation reference §7.9 lists it as a trap, so the evidence suggests revisiting. `MinBy`/`MaxBy` are proposal 0026 D2 (*LINQ additions*), under review (#1457), with `MaxByOrNull` forms.
- **Size:** stdlib only (`MaxBy`, already drafted in 0026). The shadow needs either the `*Where` tier or the deferred extension-call disambiguation syntax.

### 5. Text-processing stdlib gaps

- **Evidence** (3 rows, 3 tasks, score 5; B1, the broken `ReadToEnd`, hit the same tasks and is counted under bugs):
  - F001: no whitespace-run split.
  - F006: no regex and no split-by-predicate. Tokenizing on non-letters took a 12-line `StringBuilder` loop, with the flush written twice.
  - F016: no CSV reader; the quote-aware splitter took 17 lines.
- **Peers:** `s.split()` (Python), `split_whitespace()` (Rust), `Regex.Matches`, `csv.reader`.
- **Decline status:**
  - Regex: proposal 0020 was accepted 2026-07-07 and is still absent from the stdlib (§11 lists it deferred).
  - Whitespace split, split-by-predicate and CSV: never considered. The proposal 0062 batch covered other string members.
- **Size:** stdlib only.

### 6. Expression-form limits

- **Evidence** (4 rows, 3 tasks, score 4):
  - F042: `return (if even then Ok(n) else Error(…))` is rejected, and becomes an `if` with two `return`s.
  - F083: expression-`case` arms cannot run a statement, so the state machine became a statement `case` in a helper.
  - F084: an arm whose body is a nested `case` statement needs `do … end`.
  - F086: `=> return x,` does not parse, and becomes `=> do return x; end,`. Yet `x := v,` without a `;` was accepted.
- **Peers:** Kotlin `when` branches are blocks whose last expression is the value; Rust match arms take `{ stmt; expr }`.
- **Decline status:** branch-level target typing is a *rejected* item in DECLINED that is recorded **liftable** (#3042), with "no evidence bar stated". That fits a small revisit. Statement-bearing expression arms are not recorded as a decline. Spec §19.2 states the rule.
- **Size:** small language change.

### 7. No ad-hoc products or multiple returns

- **Evidence:**
  - F034: a `Summary` record declared only to carry three values out of a `Select`.
  - F070: `Partition` returns two arrays through two `out` parameters. A `Parts<T>` record cannot hold arrays either.
  - Not a row: t30 also declared `Slice(Lo, Hi)` only because `Domain.Spawn` takes one argument.
- **Peers:** C# `(yes, no) = …` and anonymous types; Kotlin `Pair`/`Triple` with destructuring; Rust and TypeScript tuples.
- **Decline status:** rationale §18 *deferred*: "an ergonomic convenience, not a capability gap". DECLINED D-0002 sets the reopening bar as "evidence from real code". Anonymous types are declined at D-0002, behind tuples. This study gives that evidence in 2 of 40 tasks (3 counting t30). It is modest but real; two of the uses are in query projections, where peers reach for tuples first.
- **Size:** large language change. A record cannot hold heap collections, so a tuple of arrays would hit the same §9.7 rule.

### 8. Closures cannot outlive their call

- **Evidence:** F065 (t28; weight 3, score 3). `static method By<T, K>(key: (T) -> K): (T, T) -> int => lambda(a, b) => key(a).CompareTo(key(b))` is rejected: "parameter `key` is call-bounded; declare it `scoped` to store it". `scoped` on a static method needs an `owned` return, which a function type cannot have, so no combinator library can exist. The program fell back to one inline comparer.
- **Related:** F078. An async lambda cannot carry an owned value into `TaskScope.Spawn`.
- **Peers:** Java `Comparator.comparing(f).thenComparing(g)`, Kotlin `compareBy(…)`, Rust `impl Fn` returns.
- **Decline status:** the spec names this case. Spec §28.14, *Known v1 limitation*: "combinators that compose received callbacks into a returned **function value** (`Compose(f, g)` returning `lambda(x) => g(f(x))`) are not expressible over call-bounded parameters — the returned closure would capture them beyond the call, and no bounding frame exists. The missing third tier, an *unbounded* marker, is deferred". The DECLINED row *Unbounded retention tier for function-typed parameters* is `deferred` (#1427, closed, declined with reasons). Its bar: "a corpus program … that must return a **composed function value** capturing received callbacks, where no bounding object exists to make it a `scoped` store … **together with** a stated rule for how `unbounded` participates in §27.1's function-type identity and in §28.14's method-reference formation bar". That row measured, in 2026-09, that the only `Compose` in the tree is a negative test.
- **t28 partly meets that bar.** It is a real program that must return composed comparers with no bounding object. It does not supply the second half: the function-type-identity rule.
- **Size:** large language change, adjacent to ownership.

### 9. Enum and case names

- **Evidence** (design rows F025, F028, score 3; the larger cost is implementation):
  - F026, F093 (bug rows): `NameOf` and `FromNameOrNull` are missing (B10), so I wrote a 7-arm `case` twice.
  - F029 (documented NYI row): no list of values, because `typeof(E).EnumValues` is not reachable (#3819). The loop was `for var i in 0..6` with a hard-coded bound.
  - F028: `ToString` renders the type name by design.
  - F025: a choice has no case-name string, so `Name()` was written by hand.
- **Peers:** `Weekday.values()`, `Enum.GetName`, Kotlin `::class.simpleName`, Rust `#[derive(Debug)]`.
- **Decline status:** DECLINED row "Enum name-parse over the serialized-name table … with the instance `Name` property and the §8.5 enum `ToString` default change" was declined at proposal 0124. Its `ToString` bar is "a real program showing the qualified-name default is a defect". This study does not meet that bar: `NameOf` is the designed answer and is simply unimplemented. A static `E.Values` and choice case names were never considered.
- **Size:** compiler (B10), stdlib (`E.Values`), and a small language change for choice case names.

### 10. Unused-result warnings on mutators

- **Evidence:** F058, F062, F064 (3 tasks, score 3). F046 is B14, a warning on a unit-returning call.
- `_ :=` was needed before `Remove`, `AddLast`, `BeginArray` and `Dispatch`; the warning appeared 6 times in 4 tasks.
- **Decline status:** proposal 0063's fluent exemption covers only same-type returns, and its alternatives were declined.
- **Size:** small.

### 11–18: lower-ranked themes

- **11. Small rules** (score 3). `out` is reserved (F076). A generic comma is not allowed inside an interpolation hole (F067; the diagnostic is excellent). An auto-property needs an initializer (F056).
- **12. Recursive sum types** (F087, score 2).
  - `choice Expr … Bin(left: Expr, right: Expr)` is rejected (§35.1).
  - §35.1 gives the idiom in its next sentence: "Recursive sums are expressed with a class node in the cycle (an `owned` class payload whose class holds the nested choice), exactly as recursive records are."
  - I did not use it. I rewrote the AST as a full class hierarchy (about 25 lines grew to about 45, and exhaustive matching was lost). The spec's idiom would have cost roughly one wrapper class (about +8 lines) and kept the choice and its exhaustive `case`.
  - The cost is the indirection class and its `move`s. Rust's `Box<Expr>` is the same idea, written inline.
  - Size: small at most, since the idiom is already in the spec. A teaching example in a guide would have saved this rewrite.
- **13. I/O gaps** (F051, score 2). There is no file size without opening a stream. Proposal 0071 (`FileStatus` and `File.GetStatus`, under review, no tracker yet) covers it. F049, the byte encoding, was my error: `ArraySpan<byte>` has the `Write*LittleEndian` accessors (stdlib §4.14/§4.15).
- **14. Dates** (F091, score 2). `DateOnly` has no `AddDays`, `DayOfWeek` or day difference, so the program used `DateTime`.
- **15. `??` across types** (F063, score 1). `{v ?? "miss"}` with `v: int?` needs an if-expression; this matches C# and Kotlin rules.
- **16. Collection-literal annotations** (F096, score 1).
  - There are 20 annotated literals in 18 programs. They are recorded as one aggregate row, so the formula counts them once.
  - Counted per program they would score 18. Each costs one type name; they are documented and declined with reasons (rationale §5.16).
  - The evidence does not suggest revisiting.
- **17. Exception wrapping boilerplate** (F044, score 1). A primary constructor cannot take `(message, cause)`.
- **18. Counting and upsert, by design** (F039, score 1).
  - What is left by design is `if not d.ContainsKey(k) then d[k] := 0; end; d[k] += delta;`.
  - The tally dance `counts[w] := (counts.GetOrNull(w) ?? 0) + 1` (F005, F036) is `Counter<K>`, which is specified but not implemented (#3848), so it counts as documented NYI.
  - Ref-returning indexers are deferred (rationale §18).
- **Removed: generic inference with lambdas.** F069 is now B24, a silent compiler bug: spec §14.5 phase 2 should infer `K` from the untyped lambda.
- **Removed: shared mutable state.** F094, `Locked<T>`, is documented NYI.

## Compiler-bug clusters

Full repros, and the existing issue for each, are in `data/compiler-bugs.md`. The clusters are ranked by the same formula over COMPILER-BUG rows (`bug_cluster` column):

1. **Specified members that are missing: score 11, 7 tasks.** B1 (`ReadToEnd`), B9 (`ParseOrNull`, #7318), B10 (`NameOf`, #3862), B15 (stream spans, #1394), B21 (`DateOnly.ToString(format)`, #2240).
2. **Documented not-yet-implemented: score 10, 6 tasks.** `Counter` (#3848), `Json` facade (#2349), `Locked<T>` (#1721), `typeof(E).EnumValues` (#3819), stored `GroupBy` (#755). t40's mutex half is the costliest.
3. **Wrong query results with no diagnostic: score 10, 5 tasks.**
   - B4: OrderBy-family keys other than `int` are ignored.
   - B13: decimal `Sum` is wrong (`34` for `7.00`).
   - B5: `Array<DictionaryEntry>` sorts do nothing. The `Add` segfault leg is #6557.
   - B7: an untyped string comparer does nothing.

   These cost the most debugging time in the study. Every one looked like a logic error in my program.
4. **String lifetime: score 9, 5 tasks.**
   - B6: a string passed from an element straight into a record constructor is freed with its source (use-after-free).
   - B17: interpolated strings are rejected as a "launder" (contested: #7170, #7215).
5. **Inference: score 8, 4 tasks.** B24 (silent wrong `MaxBy` element), B3 (IR verification failure, #6627), B22, B18.
6. **Miscellaneous: score 7, 3 tasks.** B8 (narrowing through an if-expression; contested: #6698). B16 (a literal at an `IEnumerable` parameter is accepted silently and writes nothing; §15.4 has no interface target, so it should be rejected). B14. B19 (regresses closed #1065).
7. **Ownership false positives: score 7, 7 tasks.** B2 (#4418) and B20 are must-use warnings at trivial-dispose types. B23 (#8308 family) is a must-use warning on a query spent inside an interpolation hole. B12 (#1772) is a hard error for a trivial-dispose local after an `await`.
8. **Checked-mode false fault: score 3, 1 task, unfinished.** B11 (#6314).

Nine bugs are silent: B1, B4, B5, B6, B7, B11, B13, B16, B24.

## What felt good

These are drawn from `data/good.tsv`.

- **Null tiers.** `byId.GetOrNull(id)?.Name ?? "(none)"`, `sec?.GetOrNull(key)`, and `TryParse(s, out var v)` were each one line and safe (t05, t13, t17).
- **Choices and `case`.**
  - Guards (`Select(var p) when a >= p`), `throw` as an expression in an arm, the binding atom for Heron's formula, and string patterns for argument parsing (t04, t08, t34, t35).
  - `Result` with `AndThen(Steps.CheckEven)` method references and an exhaustive `case` ran as a first draft (t19).
- **Records.** Value equality (`1.0m = 1.00m`), `with`, operators and `ToString` overrides were as short as C# (t07). The synthesized `Deserialize` skips unknown fields without reflection (t25).
- **Formatting.** `$"{name,-10}{qty,4}{price,8:F2}{share,8:P1}"`, `{t:HH:mm:ss}` and `TimeSpan` rendering all worked first time (t06, t21, t37).
- **Concurrency.**
  - A cancellation scope is one `using var cts := CancellationTokenSource.WithCancelAfter(…)` (t32).
  - Channels close when the last sender is dropped, so end-of-stream needs no code (t31).
  - `Domain.Spawn(SumSquares, new Slice(…))` with value results gave real parallelism with no shared state (t30).
  - `Shared<AtomicInteger<int>>` was race-free by construction (t40).
- **Resources.** The pool's connections were closed exactly once, in reverse order, including on the exception path, with no `Dispose` plumbing (t36).
- **FFI.** An `extern "libc"` block with `CString` ran as a first draft and needed no attributes (t39).
- **Many diagnostics.** The §26.5 `Count` message, the `move` and fresh-store messages, and the interpolation generic-comma message each named the exact fix.

## Coverage by §7 of the code-generation reference

I read §7 only after all rows were classified. It already documents these frictions:

- `arr.Count(pred)` (F035, F050), in §7.9.
- Fresh tasks into an `Array` and the named-holder `WhenAll` shape (F074, F081), in §7.2.
- `Add(move x)` at an owned parameter (F089).
- `dict[missing] += 1` throws (F039).
- `Counter` as the tally remedy (F005, F036; partly, since `Counter` is not implemented).
- The fresh-store rule behind the nested dictionary (F020; partly).
- Returning lambdas over a generic body's `T` (F065; partly: the function-parameter case is not there).

Of the 96 rows, 10 are documented there fully or in part; the column `in_codegen_s7` in `data/frictions.tsv` marks them. A documented trap is still friction: I wrote every one of these 10 after reading §§1–6. None of the 45 compiler-bug rows is documented, because §7 describes the language, not r40.

## What I could not do

- **t15 never ran correctly** (B11). I could not reduce it: it changes with unrelated edits. It needs a sanitizer build, and the ASan runtime was not present on this box (`--sanitize=address --link=system` failed to link).
- **t40's mutex half cannot be expressed** until `Locked<T>` lands.
- **Some workarounds hide a second bug.** I sometimes switched to a typed lambda before checking whether the untyped form failed for the same reason. The B3, B4 and B7 boundaries, which I tested with both forms, are stated in the repros.
- **Performance was not measured.** t33's concurrency check is a timing threshold.
- **No issues were filed** (study constraint).
