# Proposal 0208 — A written operand that makes a fault certain is a compile error: a constant index past an inline array's extent, a constant zero divisor, and an open-ended range literal iterated

| | |
|---|---|
| **Status** | graduated-v0.346 — superseded — graduated v0.346 (accepted 2026-10-27 as a delegated ruling under the owner's 2026-10-08 direction ("Please resolve and decide all known spec related issues. Do proposals for issues that need them.") and the 2026-10-27 direction for this train ("draft proposals for all \"clear improvement\" items, judge them yourself whether to accept them. When you accept them, proceed with spec trains to implement them. Fold all other current WIP and take it to completion."), [#7379](https://github.com/s8lap/bezel-lang/issues/7379) and [#7380](https://github.com/s8lap/bezel-lang/issues/7380), the certain-faults hunt's §2, §3 and §5 — a compile-time-constant index outside an inline array's `0..<N`, a compile-time-constant zero right operand of `/` or `mod` at an integer type or `decimal` whatever the left operand (`/=` and `mod=` included), and an open-ended range literal written directly as a `for` source, parenthesized or not, are each a compile error at the site; a computed index, a non-constant divisor, binary-float division and a range reached any other way keep their run-time faults; the `const string` index, the literal at a warning and the widening to every iteration context declined; critique round 1 applied (the divisor reach; `double` division excluded; the parenthesized literal); landed with the v0.346 changelog entry) |
| **Tracking issue** | [#7379](https://github.com/s8lap/bezel-lang/issues/7379) (the index; its `const string` leg declined) and [#7380](https://github.com/s8lap/bezel-lang/issues/7380) (the divisor), the questions filed from the certain-faults research on 2026-10-27 and closed at the v0.346 landing; none open on the literal; the neighbours are [#6961](https://github.com/s8lap/bezel-lang/issues/6961) (open, bug: the all-constant `5 / 0` outside a `const` initializer is not rejected — this proposal's divisor leg is its one-predicate extension) and [#7366](https://github.com/s8lap/bezel-lang/issues/7366) (open, bug: the open-ended literal in a `for` fails codegen with "not yet implemented" — under this proposal that path becomes the error's). Two `spec-tracking` trackers are filed at landing, and the divisor arm is folded into [#6961](https://github.com/s8lap/bezel-lang/issues/6961) (§8, §9 item 10). |
| **Target module(s)** | **spec**: §15.1.2 (the indexer bullet), §6.1 (the divide-by-zero sentence), §6.4 (one sentence), §6.6 (one sentence after the shift inheritance), §16.3 (one sentence), §16.5 *Open-ended ranges* (the lead paragraph and the example's comment), Appendix C.1 (three bullets). **rationale**: §14.7 (one dated paragraph). **code-generation reference**: §7.1 (three *Looks right, is rejected* lines, the blockquote's lead clause, one line of the right spellings). **proposals**: `README.md` (this row), `DECLINED.md` (three `declined-at-proposal` rows). No stdlib text, no guide text, no grammar change, no new keyword. **compiler** (spec-ahead-of-impl): three trackers (§8). **conformance**: one banked rule-corpus row changes expectation (`s16_5_0001`, `warn` → `reject`); no doc-corpus case changes verdict (§5). |
| **Written against** | spec **v0.345** at `d9f11d15f` (`origin/main`, 2026-10-27). Section numbers are v0.345's; every quoted anchor re-verified against that text (`scratch/v0346/S.json` dry-runs clean against a `git archive` of it). |
| **Origin** | The certain-faults research lane (`docs/research/certain-faults/REPORT.md`, 2026-10-27, §2, §3 and §5): programs whose failure is certain on every execution that reaches the site, admitted silently or under a warning, where the compiler already has the analysis. The owner's bar for this train: clearly an improvement — safer or simpler, with negligible measured migration cost, not a taste change. Lane S judged the three legs together because they share one rule shape and one precedent set; the two flow-fact candidates of the same report (§4, §6) are judged separately (proposal 0209; a `rejected` row). |

*Reference convention: unprefixed §-references are language-spec references at v0.345. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the standard library companion declares; none invents an API. "Today" means the release compiler `r35` (`bezelc 0.0.0-f6863e62`), run in both disposal modes; the programs are `docs/research/certain-faults/probes/p01*`, `p02`, `p04*` and this lane's `scratch/v0346/Swork/probes/q*`.*

---

## 0. Summary

The language already decides a fault at the site when a written operand fixes its outcome: a constant operand an explicit conversion would reject (§5.2, v0.300), a constant equal to no declared enum constant (§5.4), a constant shift count outside the domain (§6.5, v0.242), a whole constant expression that faults (§17.14). Three sites with the same certainty were left to run time: a **constant index outside an inline array's `0..<N`** (`a[5]` on a `[3]int`; the extent is in the type, and §15.1.2 already has the compiler fold a constant index to elide the in-range check), a **constant zero divisor with a non-constant dividend** (`x / 0`, `x mod Z`, `d / 0m`; §17.14 reaches only the all-constant `5 / 0`), and an **open-ended range literal written as a `for` source** (`for var i in 10..`; §16.5 mandates a warning, and the loop body cannot run). Each compiles today with no diagnostic (the literal draws the warning) and faults in both modes — `IndexOutOfRangeException`, `DivideByZeroException`; the literal does not even compile (#7366).

This proposal makes each a **compile error at its site**, in the words the sibling rules use, and nothing else: a computed index, a non-constant divisor and a range reached through a variable, a call or any other expression keep their run-time faults exactly as before. The rule is syntactic at every leg, as §6.5's count rule is (q18: `x << 40` is rejected in a branch no execution takes; q02 shows `x / 0` admitted there today). Measured reach: three gtest rows that provoke `IndexOutOfRangeException` through a constant index; three gtest rows that provoke `DivideByZeroException` through a compound `/=` by a constant zero (and nothing else dividing a non-constant by a constant zero outside #6961's all-constant cases); the spec's own §16.5 example and one banked rule-corpus case that expect the warning; nothing in a guide, nothing in the stdlib's examples.

---

## 1. The text today

> Indexer `arr[i]` for read and write. Bounds checking applies; a constant index proven in-range at compile time elides the check.
>
> — §15.1.2, v0.345

> Division and `mod` raise `DivideByZeroException` when the right operand is zero.
>
> — §6.1, v0.345 (§6.4 likewise for `decimal`)

> A count outside the domain — negative, or ≥ width — is a fault: a **compile error** when the count is a compile-time constant ([§17.14]), and `OverflowException` at runtime otherwise.
>
> — §6.5, v0.345

> An evaluation **fault** — integer or `decimal` overflow, division or `mod` by zero, a shift count outside the [§6.5] domain — is a **compile error at the constant expression**, never a deferred runtime fault
>
> — §17.14, v0.345

> Iterating an open-ended range raises `InvalidOperationException` at the call to `GetEnumerator`, before any iteration occurs. The compiler emits a warning when an open-ended range literal appears in an iteration context
>
> — §16.5, v0.345

The posture is stated four times for operands the language already treats as deciding the fault, and the three legs below fall outside each statement by one predicate: §15.1.2 folds the constant index and says what happens when the fold proves the index *in* range, not out; §17.14 reaches `5 / 0` but not `x / 0`, while §6.5 — the operator rule nearest in shape — keys on the count *alone*; §16.5 names the open-ended literal's iteration as a certain fault and then mandates a warning for it.

## 2. The problem, measured

| probe | program | today | under this proposal |
|---|---|---|---|
| p04a | `var a: [3]int := [1, 2, 3]; a[5]` | accepted silently; `IndexOutOfRangeException`, exit 70 (both modes) | **rejected** at the index |
| p04b | `const K: int := 3; a[K]` | same | **rejected** (a `const` reference is a §17.14 constant) |
| q03 | `a[3] := 9;` | same | **rejected** (a write is the same fault) |
| q04 | `a[-1]` | same | **rejected** |
| q05 | `const N: int := 2; a[N + 1]` | same | **rejected** (constant arithmetic) |
| q21 | `const i := 5; a[i]` (compile-time-classified local `const`) | same | **rejected** |
| q22 | `const i := Five(); a[i]` (a runtime `const`, §21.1) | same | unchanged — not a constant expression |
| q06 | `var a: Array<int> := [1, 2, 3]; a[5]` | same | unchanged — a heap array's count is not in its type |
| p01a | `x / 0`, `x` a call result | accepted silently; `DivideByZeroException` | **rejected** at the operator |
| p01b | `x mod Z`, `const Z: int := 0` | same | **rejected** |
| q01 | `x / Z`, `Z` a class-level `const` | same | **rejected** |
| p01c | `d / 0m`, `d` a `decimal` call result | same | **rejected** |
| q14 | `d mod 0.0m` | same | **rejected** |
| q15 | `x /= 0;` | same | **rejected** (§6.6's desugaring) |
| q02 | `(if x > 100 then x / 0 else x)` | accepted; prints `7` | **rejected** — syntactic, as q18's `x << 40` in the same position is today |
| p01d | `var y := 5 / 0;` | accepted; faults (#6961) | unchanged by this proposal — already §17.14's compile error |
| s6_1_0004 | `var z: int := 0; 7 / z` | runs, `divzero` | unchanged — a non-constant divisor |
| p02 | `for var i in 10.. do … end;` | the §16.5 warning, then "not yet implemented: codegen for this for statement" (#7366) | **rejected** at the literal |
| q07 | `var r: Range<int> := 10..; for var i in r` | accepted; prints nothing, exit 0 (a compiler defect: §16.5 says `InvalidOperationException`; `s16_5_0002` is banked `fail`) | unchanged — a variable, the run-time fault |
| q08 | `for var i in (10..).Where(lambda(x) => x > 3)` | accepted; `InvalidOperationException` | unchanged — a chain, not the literal as the source |

## 3. Options

- **O0 — the status quo.** Three certain faults at run time, one of them under a mandated warning for a program that cannot run.
- **A — the three legs as compile errors at the site (adopted).** The sibling rules' words, keyed on the written operand alone.
- **B — the index and divisor legs as errors, the literal kept at the warning.** Declined (§7): the warning's own text states the fault as certain ("before any iteration occurs"), and a mandated warning as the end state of a program with no meaning is the inversion rationale §5.28 reserves warnings against.
- **C — A plus a constant index into a `const string`** (`S[7]` over `const S := "abc"`, the research lane's third probe). Declined (§7): the string's length is a value fact of one constant, not an extent in a type; zero sites.
- **D — A plus every iteration context for the literal** (the receiver of `Where(…)`, an explicit `GetEnumerator()`, an `IEnumerable<T>` argument). Declined (§7): the predicate would read the stdlib's deferred-operator semantics into the spec, which states the fault for `GetEnumerator` alone.
- **E — a general sentence** ("any operation whose written operand fixes a fault is a compile error") in place of three site rules. Not taken: the sibling rules are each stated at their site with their own non-constant arm, and a general sentence would reach sites no one measured (a constant `Substring` start, a constant enum cast through `FromUnderlyingOrNull`'s dual, …); the rationale paragraph names the shape, the normative text names the sites.

## 4. Evaluation

| | Soundness | Simplicity | Consistency | Migration (measured, §5) |
|---|---|---|---|---|
| O0 | sound (deterministic exceptions); the literal does not compile today | — | three silent exceptions beside four stated constant-operand rules | — |
| **A** | sound: nothing newly admitted; every rejected site faults on every execution that reaches it | one sentence per site in the sibling rules' words; the literal's warning paragraph becomes shorter; the compiler's NYI path for the literal becomes the error path | the posture §5.2 states and names its siblings by; §6.5's "the count alone" applied to the divisor; §29.11's and §22.16's "certain → error" | 3 gtest rows (index); 3 gtest rows (divisor, all `/=`); the spec's §16.5 example comment, `spec_0083` (a `skip` extract) and `s16_5_0001` (banked `fail`, re-cut by the tracker) |
| B | sound | keeps a warning plus a run-time path for a shape with no run | the one certain fault that stays a warning | 0 for the literal |
| C | sound | a second extent source (a value) beside the type's | the string indexer is stdlib-stated (§4.6) | 0 |
| D | sound | a predicate over call chains | reads stdlib semantics into the spec | 0 |

A is the smallest rule that closes the three gaps, and the only option under which every constant-operand fault the language names has the same verdict.

## 5. Decision

**Option A.** Stated at each site:

- **§15.1.2.** A constant index ([§17.14]) outside `0..<N` — negative, or `N` or more — is a compile error at the index, for a read and for a write alike, never a deferred `IndexOutOfRangeException`; a non-constant index — a loop variable, a runtime `const` ([§21.1]), any computed value — is bounds-checked at run time. The heap arrays are untouched: `Array<T>`'s count is not in its type.
- **§6.1, §6.4, §6.6.** A right operand of `/` or `mod` that is a compile-time constant equal to zero is a compile error at the operator, whatever the left operand, for an operation performed at an integer type or `decimal` (§6.2: `x / 0` over a `double` `x` promotes the constant and is a binary-float operation, outside the rule); the rule reads the right operand alone (as §6.5's count rule does) and makes the expression no more constant than it was (`5 / 0` is already §17.14's faulting constant expression); `/=` and `mod=` inherit it through their desugaring; a non-constant zero divisor raises `DivideByZeroException`. The binary floating-point types are outside the rule, as outside §17.14's: `x / 0.0` is `±Infinity` or `NaN` and never faults (§6.3).
- **§16.3, §16.5.** An open-ended range literal — `10..`, `..5`, `..` — written directly as the source expression of a `for` statement, parenthesized or not, is a compile error at the literal; the rule reads the source expression as written and nothing else, so the same literal reached through a variable, a call, a parenthesized chain or any other expression is the run-time `InvalidOperationException` at `GetEnumerator`, unchanged. (`await for` over a range is not a shape: `Range<T>` is `IEnumerable<T>` only.)
- **Appendix C.1** gains the three bullets; the code-generation reference §7.1 gains the three *Looks right* lines and the right spellings.

**Why syntactic.** Each leg is keyed on the written operand, not on reachability or on the other operand's value, exactly as the siblings are: today `byte(300)` and `x << 40` are rejected in a branch no execution takes (q18, q20), and §17.14's short-circuit carve-out ("an operand that would not be evaluated at run time is not evaluated at compile time") is a rule about *constant expressions*, which `(if x > 100 then x / 0 else x)` is not. A reader who wants a division that can never run writes it where it cannot be written, or not at all.

**Kernel classification** (process §9): no contact with §28/§30.5. **Body-locality** (proposal 0105 §1): local — each rule reads one expression.

## 6. Measurement

Measured by probe (§2) and by reading `tests/` (gtest program strings, lit `.bzl`), both conformance corpora, the documents and the guides (`scratch/v0346/Swork/`; per-`BEZEL_TEST`-body scans).

- **Constant index, inline arrays.** Three gtest rows flip accept → reject, each written to provoke the exception: `tests/codegen/exceptions_test.cpp` `codegen_cg20.array_bounds_oor_read_throws` (`var a: [5]int; … a[10]`) and `.array_bounds_oor_write_throws` (`var a: [3]int; … a[5] := 42`), `tests/codegen/uncaught_exit_test.cpp` `codegen_uncaught_exit.index_out_of_range_out_of_main_nonzero` (`var a: [5]int; return a[10];`). Each keeps its subject (the run-time bounds check; the uncaught-exit code) by indexing through a non-constant (`var k := 10; a[k]`). Every other hit of the scan is in range (`arr[5]` on `[SZ * 2]int` at `SZ := 3`, `inline_array_test.cpp:310`; the `o[2]`, `a[2]`, `outer[1]` rows over `[3]`/`[3]`/`[2]` arrays). Both corpora: 0 cases (`s15_1_2_0001`, `s15_1_2_0002` state neighbouring rules and stand). Documents and guides: 0 programs index an inline array by a constant past its extent.
- **Constant zero divisor.** **3 gtest rows** newly rejected, each a compound division written to provoke the exception: `tests/codegen/decimal_property_compound_2883_test.cpp` `codegen_decimal_property_compound_2883.divide_by_zero_and_overflow_6_4` (`d.P /= 0m;`, `d[0] /= 0m;`), `tests/codegen/nullable_decimal_compound_2141_test.cpp` `codegen_2141_nullable_decimal.div_by_zero_throws_nonnullable` (`d /= 0.0m;`), and the `runAot` leg of `tests/codegen/nullable_value_compound_1964_test.cpp` `codegen_1964_nullable_compound.int_div_by_zero_throws_nonnullable` (`try i /= 0; … catch DivideByZeroException`; the same row's `rejectedCompoundP("i: int?", …, "i /= 0; …")` leg is already rejected for the nullable target and stays rejected). Each keeps its subject through a `var` zero (`decimal_compound_assign_test.cpp`'s `div_by_zero_throws` already divides by `var z: decimal := 0.0m` and stands). Beyond those, the 59 raw `/ 0` and `mod 0` hits are: binary-float `0.0 / 0.0` and `1.0 / 0.0` (NaN/Infinity producers, outside the rule); the all-constant `1 / 0`, `5 / 0`, `5 mod 0`, `int(1m / 0m)`, `int(5m mod 0m)` in `coalesce_test.cpp`, `s5_2_float_constant_expr_folding_4801_test.cpp`, the four `const_div_by_zero` lit files and `s17_14_0006` — every one already §17.14's compile error (the `coalesce_test` rows are #6961's open divergence, and this proposal changes nothing about them); and C++ noise (`/*stamp=*/0`). `decimal_521_test.cpp`'s `a / b` and the rule cases `s6_1_0004`, `s6_1_0005`, `s6_4_0002` divide by a `var` and stand. Documents: 0 programs.
- **Open-ended literal.** The spec's §16.5 example (a fenced block whose doc-corpus extract `spec_0083` is `skip:elided`; its comment moves from `WARNING` to `ERROR`), and the rule case `s16_5_0001` (`expect: warn`, banked `fail` with no implementing issue — the warning fires but the build fails at codegen): it flips to `reject` and is re-cut by the tracker. No gtest or lit row pins the warning (the research report's two named pins do not mention the shape: `range_default_3108_test.cpp` is `default(Range<T>)`, and the lit file is a closed range). `s16_5_0002` (a range variable; banked `fail`) is untouched by the rule and is the compiler defect q07 shows (a question filed, lane S's `S.md`).
- **Guides.** None of the three shapes appears; Guide 05 iterates closed ranges only.

## 7. Declined alternatives (rows for `DECLINED.md`, `declined-at-proposal`)

| decline | ground | reopening bar |
|---|---|---|
| A constant index outside a `const string`'s length as a compile error beside the inline-array rule (option C) | An inline array's extent is part of its type; a string's length is a value fact of one constant, read by a stdlib indexer whose contract §4.6 states at run time; zero sites | a measured site |
| Keeping the open-ended literal at §16.5's mandated warning (option B) | No execution reaches the loop body, so the program has no meaning the warning preserves; a mandated warning as the end state of a program that cannot run is the inversion rationale §5.28 reserves warnings against; the compiler's codegen for the shape was never written (#7366) | none stated |
| Widening the literal rule to every iteration context reached through a call (option D) | The predicate would read the stdlib's deferred-operator semantics into the spec, which states the fault for `GetEnumerator` alone; the `for` source is the one position the language itself iterates | a measured site |

## 8. The normative edits, changelog, migration, trackers

The exact text is `scratch/v0346/S.json`, ids `S-0208-*`: spec §15.1.2, §6.1, §6.4, §6.6, §16.3, §16.5 (two edits), Appendix C.1 (two edits); rationale §14.7; the code-generation reference §7.1 (three edits); this README row; the three `DECLINED.md` rows. No version header or pin moves; the coordinator's finalize step adds those and the changelog entry.

> ▲ **§15.1.2.** Indexer `arr[i]` for read and write. Bounds checking applies; a constant index proven in-range at compile time elides the check, and a constant index ([§17.14]) outside `0..<N` — negative, or `N` or more — is a **compile error at the index**, for a read and for a write alike, never a deferred `IndexOutOfRangeException`: the extent is part of the type, so the fault depends on no state, and it is decided where it is written — the posture of [§5.2]'s constant-operand rule and [§6.5]'s constant shift count. A non-constant index — a loop variable, a runtime `const` ([§21.1]), any computed value — is bounds-checked at run time.

> ▲ **§6.1.** Division and `mod` raise `DivideByZeroException` when the right operand is zero. A right operand that is a compile-time constant ([§17.14]) equal to zero — `x / 0`, `x mod Z` over `const Z := 0` — is a **compile error at the operator**, whatever the left operand's value, never a deferred `DivideByZeroException`: the fault depends on no value the dividend could take, so it is decided where it is written, the posture of [§6.5]'s constant shift count; a non-constant zero divisor raises at run time. The rule reaches an operation performed at an integer type or `decimal` ([§6.2]); a binary-floating-point operation — `x / 0` over a `double` `x` included — has no division fault ([§6.3]). The rule reads the right operand alone and makes the expression no more constant than it was (`5 / 0` is already a faulting constant expression under [§17.14]); `/=` and `mod=` inherit it through their desugaring ([§6.6]).

> ▲ **§16.5.** Iterating an open-ended range raises `InvalidOperationException` at the call to `GetEnumerator`, before any iteration occurs. An open-ended range **literal** — `10..`, `..5`, `..` — written directly as the source expression of a `for` statement ([§21.4]), parenthesized or not, is a **compile error at the literal**: no execution reaches the loop body, so no program with this shape has a meaning to preserve, and the diagnostic names what the form is for, slicing and patterns. The rule reads the source expression as written and nothing else: the same literal reached through a variable, a call, a parenthesized chain (`(10..).Where(…)`) or any other expression is the run-time fault.

**Changelog bullet (draft; lane S's `S.md` carries the house-style entry).** *§15.1.2, §6.1, §6.4, §6.6, §16.3, §16.5, Appendix C.1; rationale §14.7; the code-generation reference §7.1 — a written operand that makes a fault certain is a compile error at the site: a constant index past an inline array's extent, a constant zero divisor, an open-ended range literal iterated (design change, proposal 0208).* **Migration.** *Newly rejected (spec-ahead-of-impl):* a compile-time-constant index outside an inline array's `0..<N` (3 gtest rows); a compile-time-constant zero right operand of `/` or `mod` with any left operand, integer or `decimal`, `/=` and `mod=` included (3 gtest rows, all compound `/=` demonstrations; nothing else beyond #6961's all-constant cases); an open-ended range literal written as a `for` source (the §16.5 example; `s16_5_0001` warn → reject). *Newly admitted:* nothing. *Newly warned:* nothing; the §16.5 warning is retired with its shape. *Unchanged:* a computed index, a runtime `const` index, a heap array's index; a non-constant divisor; a range reached through a variable or a call; binary-float division. *Ramp:* none — the warning was never a scheduled ramp (§16.5 has stated it as the end state since the section was written; the compiler implemented it under the #4554 warning sweep), so this is a design change, not a ratchet.

**Trackers to file at landing (`spec-tracking`).**

1. *§15.1.2 (v0.346, proposal 0208): a compile-time-constant index outside an inline array's `0..<N` is a compile error at the index, read or write.* The fold exists (`foldLocalConstArraySize` resolves `N`; the in-range elision folds the index); add the out-of-range arm at the indexer typing site for `[N]T` receivers, keyed on §17.14 constancy (a literal, a `const` reference — class-level, compile-time-classified local, imported — constant arithmetic; a runtime `const` is not constant); re-pin `codegen_cg20.array_bounds_oor_read_throws`, `.array_bounds_oor_write_throws` and `codegen_uncaught_exit.index_out_of_range_out_of_main_nonzero` through a non-constant index; rows: `a[5]`, `a[3] := 9`, `a[-1]`, `a[K]`, `a[N + 1]`, the runtime-`const` and loop-variable controls, a heap-array control.
2. *§6.1 / §6.4 / §6.6 (v0.346, proposal 0208): a compile-time-constant zero right operand of `/` or `mod` is a compile error at the operator, whatever the left operand.* One predicate beside #6961's: `checkConstEvalFault` returns early on a zero divisor for the all-constant case (#6961's defect); the new arm reads the right operand alone through `constIntValueTC` (integer) and the decimal fold, for `/`, `mod`, `/=` and `mod=`, and leaves binary-float operands alone. The coordinator may fold this into #6961 (one commit closes both) or file it as the follow-on; rows: `x / 0`, `x mod Z` (local and class `const`), `d / 0m`, `d mod 0.0m`, `x /= 0`, a `var z := 0` control, `x / 0.0` control, a `double` `x / 0` control (the int constant promotes, §6.2: no fault); re-pin `divide_by_zero_and_overflow_6_4` (2883), `div_by_zero_throws_nonnullable` (2141) and the `runAot` leg of `int_div_by_zero_throws_nonnullable` (1964) through a `var` zero.
3. *§16.3 / §16.5 (v0.346, proposal 0208): an open-ended range literal as a `for` source is a compile error at the literal.* Promote `TypeResolution_Names.cpp`'s warning (`fix #4554`) to an error naming slicing and patterns, and make #7366's codegen path unreachable for the literal (a range **variable** that is open-ended still needs the `InvalidOperationException` path, which q07 shows is also missing — the separate question in `S.md`); re-cut `s16_5_0001` to `expect: reject`; rows: `10..`, `..5`, `..` as a `for` source (reject), `(10..).Where(…)` and a `Range<int>` variable (accept, run-time fault).

## 9. Self-critique (adversarial pass)

1. **Does any leg reject a program that could run?** No. The index leg reads the extent from the type; the divisor leg reads a constant zero that no dividend changes; the literal leg reads a source expression whose `GetEnumerator` faults before the first iteration. The dead-branch shapes (q02) are rejected although no execution reaches them — the same choice §6.5 and §5.2 make today (q18, q20), stated in §5 so no reader infers a reachability analysis.
2. **The runtime `const`.** `const i := Five(); a[i]` is admitted (q22): §17.14 makes a reference to a local `const` constant only when the declaration is compile-time-classified. The edit names the runtime `const` explicitly so the two spellings are not confused.
3. **Generic bodies.** An inline array's `N` is always a constant expression (§15.1) and a type parameter is never a size, so no instantiation-site re-check arises. A divisor `T.Zero` under `where T: INumber<T>` is a call, not a constant (§17.14). Nothing reaches §3.6 rule 2.
4. **`decimal` zero spellings.** `0m`, `0.0m`, `0.00m` are all the constant zero; the rule keys on the value, as §17.14's fault does for `5m / 0m` (the #4844 rows show the compiler already folds decimal constants to the fault).
5. **Does the literal rule change the stdlib's `Range<T>` contract?** No. Stdlib §9.8's `GetEnumerator` sentence states the run-time fault for every open-ended range and stands; the literal rule is a rule about one source position of one statement, stated in the spec.
6. **The §16.5 example is extracted.** `spec_0083` is `skip:elided` (it contains `...`); its verdict does not move, and the comment edit keeps the fence a single line, so EXTRACT parity holds.
7. **Other passages stating the old rule.** Swept by grep: `open-ended`, `cannot be iterated`, `constant index`, `elides the check`, `by zero`, `DivideByZero`, `IndexOutOfRange` over the twelve documents. §16.3's warning sentence and §16.5's lead are edited; §4.6's string indexer ("Out-of-range raises `IndexOutOfRangeException`") stands (option C declined); §15.6's slicing faults stand (endpoints, not constants in a type); §6.1's `mod`-on-`decimal` sentence and §6.6's `n mod= 3` comment describe the run-time fault for a non-constant divisor and stand; §17.14, Appendix C.11's `const`-initializer bullet and §31.4's hierarchy are unchanged; the code-generation reference §3.5's `step`-on-open-ended note is a different rule (§16.4) and stands.
8. **Composition with the train's other lanes.** Lane G edits §15.4/§28.11 (the fresh-store lint), lane F §28.2/§28.3/§28.7/§28.11, lane Q §28.2 (#7364), §2.7 (#7343) and stdlib §2.15 (#7360); none of this proposal's anchors is in those paragraphs. Appendix C.1 is shared with any lane adding bullets there; the two inserts anchor on their neighbouring bullets' tails.
9. **Implementation risk.** The index arm must not fire on a `const` the compiler cannot fold (an imported `const` was the #3617 defect; the fold now resolves it) — where the fold returns nothing the index is not provably constant and the run-time check stays, the direction the backstop covers. The literal arm must keep #7366's variable path: the error is for the literal only.
10. **Composition (v0.346).** Tracker 2 (the divisor) is folded into [#6961](https://github.com/s8lap/bezel-lang/issues/6961): one arm in `checkConstEvalFault` serves both, so #6961 is relabelled `spec-tracking` and carries the rows of §8's tracker 2. Trackers 1 and 3 are filed.
