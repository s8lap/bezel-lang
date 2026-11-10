# Ergonomics study: 40 everyday programs against r40

*November 2026. Compiler r40 (`bezelc-0.0.0-a30bb11d`, origin/main a30bb11df), checked disposal mode. Spec and stdlib v0.355. Evidence only: no proposals.*

## One page for the owner

I wrote 40 small programs of the kind a services-and-tools developer writes, the way someone coming from C#, Kotlin, Rust or TypeScript would write them after reading `Bezel_For_Code_Generation.md` §§1–6. Then I compiled and ran each one, and fixed it until the output matched the task. Every draft is kept.

**Outcome.** 38 of 40 programs reached the expected output. One of these, t24, needed a data-model compromise. t40 is half done: the mutex half cannot be expressed today. t15 never ran correctly, because of compiler bug B11. Only 11 first drafts ran correctly. There are 90 drafts in all.

**Headline counts.** There are 96 friction rows (`data/frictions.tsv`). Each row has one cause:

| Cause | Rows |
|---|---|
| COMPILER-BUG | 39 (22 distinct bugs, `data/compiler-bugs.md`; 8 are silent wrong output) |
| LANG-RULE | 24 |
| MISSING | 15 |
| DIAGNOSTIC | 8 |
| VERBOSE/AWKWARD | 8 |
| USER-ERROR | 2 |

There are also 30 "felt good" rows (`data/good.tsv`).

**The ten design themes.** They are ranked by frequency × cost. Compiler bugs are excluded here and ranked separately below.

| # | Theme | Evidence (rows / tasks) | Size |
|---|---|---|---|
| 1 | **Diagnostics point at the wrong place or cascade.** Examples: the B1 internal warning is followed by an error at an unrelated `for`; 60 parse errors follow a nested `case`; 5 conversion errors hide the real call-bounded cause (t28). | 9 / 9 | compiler only |
| 2 | **Ownership friction outside resource code** (for the ownership lane; no fixes designed here). Fresh tasks cannot go into an `Array`, so `WhenAll([...])` takes 5 lines. Nested dictionaries need `OwningDictionary`. A query bound to a local warns. A closure cannot move a captured sender. | 8 / 7 | — |
| 3 | **Text-processing stdlib gaps.** There is no whitespace split, no regex (proposal 0020 is accepted but not landed), no CSV reader, and `ReadToEnd` is broken (B1). | 3 MISSING + 2 bug / 5 | stdlib only |
| 4 | **Closures cannot outlive their call.** Comparer combinators (`By`, `Then`, `Reverse`) are impossible, because a plain function parameter is call-bounded and a static method cannot return `scoped`. | 2 / 2 (high cost) | large |
| 5 | **No ad-hoc products or multiple returns.** One-use records appear only to carry a projection; `Partition` uses two `out` parameters. Tuples are deferred, and D-0002's bar is "evidence from real code". | 2 + 1 implicit / 3 | large |
| 6 | **Counting and upsert.** Code uses `d[k] := (d.GetOrNull(k) ?? 0) + 1` and an init-then-`+=` dance. `Counter<K>` is specified but not implemented (#3848). | 3 / 3 | stdlib only |
| 7 | **Enum and case names.** A 7-arm `case` maps each constant to its name: `NameOf` is missing (B10), there is no values list (#3819), and `ToString` renders the type name by design. | 5 / 3 | stdlib + small |
| 8 | **Expression-form limits.** A bare `Ok(...)` cannot sit in an `if`-expression (#3042, recorded "liftable"). Expression `case` arms cannot hold statements. A nested `case` needs a `do` wrapper. | 4 / 3 | small language change |
| 9 | **Query surface.** There is no `MaxBy`/`MinBy`. `Array.Count(pred)` is shadowed by the `Count` property; `CountWhere` is "declined, evidence-gated", and this study is that evidence. | 4 / 4 | stdlib only |
| 10 | **Serialization shape.** A record cannot hold a list. There is no end-of-object probe for hand-driven JSON. `JsonOptions` has five positional booleans with no defaults. | 5 / 2 | small + stdlib |

**Compiler bugs, ranked by cluster.**

1. **Silent wrong query results: 6 tasks.** `OrderBy` ignores string, double and decimal keys (B4). Decimal `Sum` is wrong (B13). Sorting `DictionaryEntry` arrays fails (B5). An untyped string comparer does nothing (B7).
2. **Strings freed with their source: 5 tasks.** This is a use-after-free (B6). Separately, `Select(x => $"{x}")` is rejected (B17).
3. **Members the stdlib specifies but r40 lacks: 8 tasks.** These are `ParseOrNull`, `NameOf`, `ReadToEnd`, the stream span overloads and `DateOnly.ToString(format)`.
4. **Ownership false positives: 6 tasks.** These are must-use warnings at trivial-dispose types, and a hard error in `async` bodies (B2, B12, B20).

**Keep these.** The `*OrNull` + `?.` + `??` tier, choices with exhaustive `case` and guards, `Result` with `AndThen` method references, interpolation with alignment and format, cancellation scopes, channels that close when the last sender is dropped, `Domain.Spawn` with value results, deterministic destructors, reflection-free serialization synthesis, and FFI without attributes. 11 first drafts ran unchanged.

---

## Method

- **Tasks.** `tasks/` holds 40 one-paragraph statements, written before any code, with expected output. Two errata were found while running. Neither changes a program: 2026-11-10 is a Tuesday (t38), and the error position in t35 is 9, not 10.
- **Drafts.** `programs/<task>/v1.bzl … vN.bzl` and `final.bzl`. Each revision carries a comment at the changed line naming the reason (`// r40: …` marks a compiler-bug workaround). I did not read §7 of the code-generation reference until all rows were classified.
- **Build.** `bezelc --checked-disposal -o <exe> <file>`, then run. The final run of every program, including diagnostics, is in `data/final-outputs.txt`.
- **Classification.** Each row has exactly one cause. A suspected bug was reduced to a minimal repro before it was called COMPILER-BUG. Two first readings did not survive a re-check, and were reclassified as DIAGNOSTIC: a `String.Equals` cascade (t09) and a `SkipValue` misreading (t25).
- **Ranking.** Frequency counts distinct tasks per theme. Cost is weighted 1 when characters were added, 2 when lines were added or code restructured, and 3 when the design changed or the task could not be done. The weights are judgement; the rows are the evidence.

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
| t16 inventory | 1 | ok | upsert dance; query must-use |
| t17 parse-or-fail | 1 | ok | — |
| t18 retry | 2 | ok | B12 |
| t19 Result pipeline | 2 | ok | bare case in if-expression; B9 |
| t20 exceptions | 1 | ok | — |
| t21 file numbering | 2 | ok | B16 (literal silently writes nothing) |
| t22 binary file | 2 | ok | B15 (span I/O); manual byte encoding |
| t23 directory list | 2 | ok | `Count(pred)`; no file size |
| t24 JSON round-trip | 3 | ok, compromised (tags fixed at 2) | record cannot hold a list; B19; facade NYI |
| t25 JSON hand-read | 5 | ok via a declared record | no object-end probe |
| t26 plugin registry | 1 | ok | unused-result warning |
| t27 LRU cache | 2 | ok | `??` across types; unused results |
| t28 comparer | 3 | ok without combinators | closures cannot be returned |
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

- **Evidence:** F004, F018, F027, F060, F066, F073, F077, F085, F095. Nine tasks.
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
  - F038: binding a lazy query to `var` draws the must-use warning.
  - F074, F081: fresh tasks cannot go into a borrowing `Array`, so `await Task.WhenAll([F(1), F(2), F(3)])` becomes five lines. The class-typed result also needs a named task.
  - F078: an async lambda cannot move a captured owned sender, so the channel pipeline gave up on `TaskScope`.
  - F079: owned parameters that rely on teardown warn.
  - F089: `move` is required at an owned argument.
- **Bug-amplified (COMPILER-BUG):** F015, F030, F040, F052, F080, F088, through B2, B12 and B20. These warnings and errors appear where the spec says nothing should.
- **Weight:** 7 of 40 programs that are not about resources still met an ownership rule. The two costliest were the task fan-out and the captured-sender case. `using var` and the `move` markers themselves were cheap. The diagnostics usually named the fix.
- **Peers:**
  - C#: `await Task.WhenAll(F(1), F(2))`.
  - Kotlin: `awaitAll(async { … }, …)`.
  - Go: `go produce(ch)` with `close(ch)`.
- **Decline status:** §7.2 of the code-generation reference documents the task-holder and fresh-store shapes. Proposal 0082's `--experimental-scoped-await-bind` touches F081.
- **Size:** not assessed (the ownership lane owns it).

### 3. Text-processing stdlib gaps

- **Evidence:**
  - F001: no whitespace-run split.
  - F006: no regex and no split-by-predicate. Tokenizing on non-letters took a 12-line `StringBuilder` loop, with the flush written twice.
  - F016: no CSV reader; the quote-aware splitter took 17 lines.
  - F002, F003: B1, the broken `ReadToEnd`.
- **Peers:** `s.split()` (Python), `split_whitespace()` (Rust), `Regex.Matches`, `csv.reader`.
- **Decline status:**
  - Regex: proposal 0020 was accepted 2026-07-07 and is still absent from the stdlib (§11 lists it deferred).
  - Whitespace split, split-by-predicate and CSV: never considered. The proposal 0062 batch covered other string members.
- **Size:** stdlib only.

### 4. Closures cannot outlive their call

- **Evidence:** F065 (t28). `static method By<T, K>(key: (T) -> K): (T, T) -> int => lambda(a, b) => key(a).CompareTo(key(b))` is rejected: "parameter `key` is call-bounded; declare it `scoped` to store it". `scoped` on a static method needs an `owned` return, which a function type cannot have, so no combinator library can exist. The program fell back to one inline comparer.
- **Related:** F078. An async lambda cannot carry an owned value into `TaskScope.Spawn`.
- **Peers:** Java `Comparator.comparing(f).thenComparing(g)`, Kotlin `compareBy(…)`, Rust `impl Fn` returns.
- **Decline status:** spec §28.14 (v0.177) made call-bounded the default for function parameters, and rationale §4.2 records the closure-as-borrow-carrying-value model. Returning a closure over a *function* parameter is not discussed as a decline anywhere I found. DECLINED records only the generic-`T` variants (§28.3).
- **Size:** large language change, adjacent to ownership. The evidence suggests asking whether a capture-free-over-parameters return, where the closure captures only function parameters that are themselves capture-free, could be admitted.

### 5. No ad-hoc products or multiple returns

- **Evidence:**
  - F034: a `Summary` record declared only to carry three values out of a `Select`.
  - F070: `Partition` returns two arrays through two `out` parameters. A `Parts<T>` record cannot hold arrays either.
  - Implicit: t30 declared `Slice(Lo, Hi)` only because `Domain.Spawn` takes one argument.
- **Peers:** C# `(yes, no) = …` and anonymous types; Kotlin `Pair`/`Triple` with destructuring; Rust and TypeScript tuples.
- **Decline status:** rationale §18 *deferred*: "an ergonomic convenience, not a capability gap". DECLINED D-0002 sets the reopening bar as "evidence from real code". Anonymous types are declined at D-0002, behind tuples. This study gives that evidence in 3 of 40 tasks. It is modest but real; two of the uses are in query projections, where peers reach for tuples first.
- **Size:** large language change. A record cannot hold heap collections, so a tuple of arrays would hit the same §9.7 rule.

### 6. Counting and upsert

- **Evidence:**
  - F005, F036: `counts[w] := (counts.GetOrNull(w) ?? 0) + 1;`, duplicated for the trailing word.
  - F039: `if not d.ContainsKey(k) then d[k] := 0; end; d[k] += delta;`.
- **Peers:** `Counter(words)`, `groupingBy().eachCount()`, `entry().or_insert(0) += 1`, `d.merge(k, 1, Int::plus)`.
- **Decline status:** `Counter<K>` is specified (proposal 0062, graduated v0.215) but not implemented (#3848). Ref-returning indexers ("would let `dict[key]++` be one operation") are deferred in rationale §18.
- **Size:** stdlib only (implement `Counter`). A `GetOrDefault`-style upsert on `Dictionary` would cover the non-counting case.

### 7. Enum and case names

- **Evidence:**
  - F026, F093: `NameOf` and `FromNameOrNull` are missing (B10), so I wrote a 7-arm `case` twice.
  - F029: no list of values, because `typeof(E).EnumValues` is not reachable (#3819). The loop was `for var i in 0..6` with a hard-coded bound.
  - F028: `ToString` renders the type name by design.
  - F025: a choice has no case-name string, so `Name()` was written by hand.
- **Peers:** `Weekday.values()`, `Enum.GetName`, Kotlin `::class.simpleName`, Rust `#[derive(Debug)]`.
- **Decline status:** DECLINED row "Enum name-parse over the serialized-name table … with the instance `Name` property and the §8.5 enum `ToString` default change" was declined at proposal 0124. Its `ToString` bar is "a real program showing the qualified-name default is a defect". This study does not meet that bar: `NameOf` is the designed answer and is simply unimplemented. A static `E.Values` and choice case names were never considered.
- **Size:** compiler (B10), stdlib (`E.Values`), and a small language change for choice case names.

### 8. Expression-form limits

- **Evidence:**
  - F042: `return (if even then Ok(n) else Error(…))` is rejected, and becomes an `if` with two `return`s.
  - F083: expression-`case` arms cannot run a statement, so the state machine became a statement `case` in a helper.
  - F084: an arm whose body is a nested `case` statement needs `do … end`.
  - F086: `=> return x,` does not parse, and becomes `=> do return x; end,`. Yet `x := v,` without a `;` was accepted.
- **Peers:** Kotlin `when` branches are blocks whose last expression is the value; Rust match arms take `{ stmt; expr }`.
- **Decline status:** branch-level target typing is a *rejected* item in DECLINED that is recorded **liftable** (#3042), with "no evidence bar stated". That fits a small revisit. Statement-bearing expression arms are not recorded as a decline. Spec §19.2 states the rule.
- **Size:** small language change.

### 9. Query surface: MaxBy and the `Count(pred)` shadow

- **Evidence:**
  - F024, F068: no `MaxBy`/`MinBy`. The code-generation reference offers `OrderBy(f).First()`, which is O(n log n), and here also wrong because of B4.
  - F035, F050: `arr.Count(pred)` is unreachable on `Array`, `Set` and `Dictionary`, so I wrote `.Where(pred).Count()`.
- **Peers:** .NET 6+ `MaxBy`, Kotlin `maxBy`, Rust `max_by_key`.
- **Decline status:** rationale §15: "`CountWhere` is declined, evidence-gated … The reopening bar: field evidence that the collision costs real users real time on the `Count`/`Contains` names specifically". The collision hit 2 of 40 tasks, and the code-generation reference §7.9 lists it as a trap, so the evidence suggests revisiting. `MinBy`/`MaxBy` appear only in the code-generation reference's absent list and were never considered.
- **Size:** stdlib only (`MaxBy`). The shadow needs either the `*Where` tier or the deferred extension-call disambiguation syntax.

### 10. Serialization shape

- **Evidence:**
  - F053: `record Person(…, Tags: Array<string>): ISerializable` is rejected. Heap collections are class components only (spec §9.8).
  - F055: the class form was also rejected (B19), so the record took `[2]string`.
  - F059: `IDeserializer` cannot iterate an object's fields of unknown count ("driven by the object's textual extent", but no API). Hand-driven tolerant parsing went through a declared record instead.
  - F057: `new JsonOptions(false, 2, false, true, 64)`. Positional records admit no defaults.
  - F061: no naming policy, so record components were written in lower case to match `"id"`.
  - F054: the one-shot facade `Json` is not implemented (#2349).
- **Peers:** `@Serializable data class` (Kotlin), serde `#[serde(rename_all)]`, `JsonSerializerOptions { … }`.
- **Decline status:** the record clause is a deliberate §9.8/§9.7 rule. The other items were never considered.
- **Size:** stdlib (object-end probe, options with defaults, naming policy). A small language change would be needed for defaults in positional records.

### 11–20: lower-ranked themes

- **11. Unused-result warnings on mutators** (F058, F062, F064; F046 is B14). `_ :=` was needed before `Remove`, `AddLast`, `BeginArray` and `Dispatch`; the warning appeared 6 times in 4 tasks. Proposal 0063's fluent exemption covers only same-type returns, and its alternatives were declined. Size: small.
- **12. Recursive sum types** (F087). `choice Expr … Bin(left: Expr, right: Expr)` is rejected (§35.1). The AST became an abstract class plus three sealed subclasses with owned fields, constructors and `move left`: about 25 lines grew to about 45, and an exhaustive `case` gave way to virtual dispatch. Never considered as a decline. Rust needs `Box`, but keeps the enum. Size: large language change (an indirection marker), or a documented idiom (`closed class` hierarchy with exhaustive type patterns).
- **13. Shared mutable state across domains** (F094). `Locked<T>` is not implemented (#1721). `Shared<T>` is sendable only at stdlib synchronized types, so a user counter under a `Mutex` cannot be shared at all. Size: implementation.
- **14. I/O gaps** (F049, F051; B15). No file size without opening a stream. Byte accessors exist only on spans.
- **15. Generic inference with lambdas** (F069). `MaxBy<Person, int>(…)` needed explicit type arguments (B22 is the Range case).
- **16. Dates** (F091). `DateOnly` has no `AddDays`, `DayOfWeek` or day difference, so the program used `DateTime`.
- **17. `??` across types** (F063). `{v ?? "miss"}` with `v: int?` needs an if-expression; this matches C# and Kotlin rules.
- **18. Exception wrapping boilerplate** (F044). A primary constructor cannot take `(message, cause)`.
- **19. Collection-literal annotations** (F096). There are 20 annotated literals in 18 programs. They are cheap, documented, and declined with reasons (rationale §5.16). The evidence does not suggest revisiting.
- **20. Small rules.** `out` is reserved (F076). A generic comma is not allowed inside an interpolation hole (F067; the diagnostic is excellent). An auto-property needs an initializer (F056).

## Compiler-bug clusters

Full repros are in `data/compiler-bugs.md`. Ranked by tasks hit and severity:

1. **Wrong query results with no diagnostic: 6 tasks.**
   - B4: OrderBy-family keys other than `int` are ignored.
   - B13: decimal `Sum` is wrong (`34` for `7.00`).
   - B5: `Array<DictionaryEntry>` sorts do nothing, and segfault when built with `Add`.
   - B7: an untyped string comparer does nothing.

   These cost the most debugging time in the study. Every one looked like a logic error in my program.
2. **String lifetime: 5 tasks.**
   - B6: a string passed from an element straight into a record constructor is freed with its source (use-after-free; garbage output, then crashes).
   - B17: interpolated strings are rejected as a "launder".
3. **Specified members that are missing: 8 tasks.** B1 (`ReadToEnd`), B9 (`ParseOrNull`), B10 (`NameOf`), B15 (stream spans), B21 (`DateOnly.ToString(format)`).
4. **Ownership false positives: 6 tasks.** B2 and B20 are spurious must-use warnings. B12 is a hard error for a trivial-dispose local after an `await`.
5. **Inference crashes and misses: 4 tasks.** B3 (IR verification failure on an untyped `GroupBy` key), B22, B18.
6. **Checked-mode false fault: 1 task, unfinished.** B11.
7. **Silent no-op and narrowing.** B16: a literal passed to `IEnumerable` writes nothing. B8: narrowing is lost through an if-expression. B14, B19.

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

Of the 96 rows, 10 are documented there fully or in part; the column `in_codegen_s7` in `data/frictions.tsv` marks them. A documented trap is still friction: I wrote every one of these 10 after reading §§1–6. None of the 39 compiler-bug rows is documented, because §7 describes the language, not r40.

## What I could not do

- **t15 never ran correctly** (B11). I could not reduce it: it changes with unrelated edits. It needs a sanitizer build, and the ASan runtime was not present on this box (`--sanitize=address --link=system` failed to link).
- **t40's mutex half cannot be expressed** until `Locked<T>` lands.
- **Some workarounds hide a second bug.** I sometimes switched to a typed lambda before checking whether the untyped form failed for the same reason. The B3, B4 and B7 boundaries, which I tested with both forms, are stated in the repros.
- **Performance was not measured.** t33's concurrency check is a timing threshold.
- **No issues were filed** (study constraint).
