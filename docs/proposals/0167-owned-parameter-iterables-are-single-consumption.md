# Proposal 0167 — An iterable minted over an `owned` parameter is single-consumption

| | |
|---|---|
| **Status** | graduated-v0.334 — superseded — graduated v0.334 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues" and the 2026-10-09 direction "resolve all findings autonomously": option (a) (SIMPLIFY C4), against the root's recommended (b) — an iterator method that declares an `owned` or move-only parameter returns a single-consumption iterable under §29.11's affine rule, the first enumerator taking the frame's parameters and an iterable from which no enumerator was produced disposing them itself; critique round 1 applied, which closed the never-enumerated transfer route, corrected the remedy and kept K6.1's lazy-query bound; landed with the v0.334 changelog entry) |
| **Tracking issue** | [#5766](https://github.com/s8lap/bezel-lang/issues/5766) (`spec-defect`: an `owned` parameter of a re-iterable iterator is disposed by the first enumerator, and a second `GetEnumerator()` re-runs the body over it); the parameter-moment half of [#5765](https://github.com/s8lap/bezel-lang/issues/5765); one `spec-tracking` compiler tracker is filed at landing (§7); [#4828](https://github.com/s8lap/bezel-lang/issues/4828) and [#4827](https://github.com/s8lap/bezel-lang/issues/4827) become implementation of this rule |
| **Target module(s)** | **spec**: §29.3 (one sentence after the return-type list), §29.8 (the first paragraph; the not-moved-out paragraph; the move-only paragraph's disposal point; the Echo example's comment), §29.11 (the opening sentence; the streaming-reader parenthetical), §28.3 K6.1, Appendix C.3 (one bullet). **stdlib**: none. **rationale**: §5.12 (one dated paragraph). **code-generation reference**: §7.5 (one *Looks right, is rejected* pair). **Guide 05**: one bullet (the pin moves at the train's guide re-trace). **`DECLINED.md`**: two `declined-at-proposal` rows (§9) |
| **Written against** | spec/stdlib **v0.333** at `429fd72` (main). Section numbers are v0.333's. |
| **Origin** | The disposal matrix's first run (pilot cell R8/C11, re-verified by lanes L1, L3, L4 and L5 — cells R8/C1, R8/C5, route R8/C9·reiterable, R6/C11), consolidated as root RC28 and filed as #5766; the simplification review's candidate C4. It needs a proposal because the decision narrows a stated admission: §29.3 makes every `IEnumerable<T>` iterator "re-iterable: each call to `GetEnumerator()` runs the body again from the start", and this rule removes that for one class of iterator. |

*Reference convention: unprefixed §-references are language-spec references at v0.333; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

§29.8 says an iterator's `owned` parameter is taken by "the state machine" at construction and disposed "when the iterator is disposed"; §29.3 says every `IEnumerable<T>` (and `IOwningEnumerable<T>`, `IAsyncEnumerable<T>`, `IOwningAsyncEnumerable<T>`) result is re-iterable. The text never says whether the parameter belongs to the enumerable or to each enumerator, and nothing bars a second enumeration. Read with §29.8's own Echo comment ("disposed by the iterator state machine when the consumer disposes the enumerator") and §29.11 ("Each `GetEnumerator()` call produces an independent enumerator"), the first enumerator disposes the parameter and the second runs the body over a disposed object — or, in §29.8's `using var r := move reader;` shape, moves it twice. That is a soundness hole with no K4.3 clause recording it.

The decision: **an iterator method that declares an `owned` or move-only parameter returns a single-consumption iterable**, under exactly §29.11's existing affine rule for a lazy query. The body runs once. The first `GetEnumerator()`/`GetAsyncEnumerator()` takes the frame's parameters into the one enumerator, which disposes any the body did not move out of when it completes (a body fault being a completion, #5765) or is disposed; an iterable from which no enumerator was produced disposes them itself (its right never spent, or spent only by a transfer to a composed query or a callee that never enumerated it). A second consumption is a compile error where the flow analysis sees it and `InvalidOperationException` at run time; the remedies are §29.11's — materialize at a value element type, or lend the resource as a plain parameter, which keeps the iterable re-iterable.

Measured reach: 87 owned-parameter iterators keep compiling; five gtest programs consume one such bound result twice and flip to rejected; zero lit, rule-corpus, doc-corpus, guide and reference sites.

## 1. The problem

### 1.1 The two frames and the one parameter (R8/C11, R8/C1, R8/C5, R8/C9·reiterable)

§29.8 (v0.333): "An iterator method may take `owned` parameters. The state machine takes ownership at construction time. When the iterator is disposed, the state machine disposes any `owned` parameter that has not been moved out of by the body." §29.3: the return type `IEnumerable<T>` is "re-iterable: each call to `GetEnumerator()` runs the body again from the start". §29.11: "Each `GetEnumerator()` call produces an independent enumerator." §29.8's Echo example: "source is disposed by the iterator state machine when the consumer disposes the enumerator."

```bezel
class Res
  public property N: int;
  destructor
    Console.WriteLine("closed");
  end destructor;
end Res;
class Demo
  public static method Echo(owned r: Res): IEnumerable<int>
    yield return r.N;            // §29.8's Echo shape
  end Echo;
  public static method Run()
    using var xs := Echo(new Res());               // r taken by the frame at the call
    for var a in xs do Console.WriteLine(a); end;  // enumerator 1 disposed: r disposed
    for var b in xs do Console.WriteLine(b); end;  // §29.3: body runs again: r.N on a disposed r
  end Run;
end Demo;
// Twin (ReadAndClose): body `using var k := move r; yield return k.N;` moves r twice.
```

The escapes the lanes recorded: a second `for` over one enumerable reads a disposed parameter (or moves it twice); nested loops over one enumerable dispose it under the outer enumerator; a move-only parameter's slots are emptied by the first enumerator and re-read by the second (R6/C11). Under the enumerator reading a never-enumerated enumerable never discharges the parameter (R8/C1); explicit kills on the enumerable versus its enumerators (R8/C9) and the transfer side (R8/C5) have no stated holder.

### 1.2 What the text already says that the rule reuses

§29.11: a lazy query "is **single-consumption**. It may be enumerated **at most once** … A second enumeration, or any other use after the first, is a compile error where the flow analysis can see it statically …; where a re-enumeration is reachable only at runtime, it raises `InvalidOperationException`", with the normative consumption-point list (enumeration or a terminal operator; a further deferred operator; passing as an `IEnumerable<T>` argument) and the remedies; and, already: "(A lazy or single-pass source whose backing is itself external, such as a streaming reader, follows the same single-consumption rule.)" K6.1: "the **single consumption point** for a lazy query, which is thereby affine". The streaming-reader sentence is exactly §29.8's `ReadAndClose` and `Echo`; the rule below makes it true by rule rather than by description.

### 1.3 The compiler (`429fd72`)

The compiler is already partly affine here. `tests/codegen/iterator_owned_param_conditional_973a_test.cpp:226` (`sync_second_enumeration_raises_and_disposes_once_spec_29_11`) and `tests/codegen/iter_named_getenumerator_reenum_2376_test.cpp:269` (`named_local_owned_param_second_getenumerator_raises_ioe_spec_29_8`) pin that a second enumeration of a named owned-parameter source raises `InvalidOperationException` — but, as 2376's note states, the claim-and-null mechanism (#973a/#2154) "only ARMS for the narrower CONDITIONALLY-discharged owned-param shape"; a plain owned parameter the body merely reads is cloned into a second enumerator (`tests/codegen/iterator_owned_param_named_local_1001_test.cpp:153–207` pins "disposed once over two enumerations"). The async twin (#2356) claims at `GetAsyncEnumerator` for owned-parameter async iterators. No static check exists.

## 2. Measurement

Recipe: `scratch/matrix2/simplify_work/reach_out/inspect_c4_reiterable_owned_param.txt` (87 owned-parameter iterators returning an `I*Enumerable`: 73 gtest in 25 files, 3 lit, 2 conformance-doc — §29.8's two examples —, 7 rule-corpus cases, 2 spec examples; 4 move the parameter), each `bound_enumerated_twice > 0` file then read by hand at `429fd72`:

| File | Programs that consume one bound owned-parameter result twice | Verdict today → under 0167 |
|---|---|---|
| `tests/codegen/iterator_owned_param_named_local_1001_test.cpp` | `double_enum_named_local_disposed_once` (:153), `nested_loops_named_local_disposed_once` (:172), `early_break_then_reenum_disposed_once` (:192) | run, disposed once over two enumerations → **rejected** (statically visible: one binding, straight-line) |
| `tests/codegen/iterator_owned_param_conditional_973a_test.cpp` | `sync_second_enumeration_raises_and_disposes_once_spec_29_11` (:226) | run, second `for` raises → **rejected** (two straight-line `for` loops over one binding are statically visible; the run-time face stays for shapes the analysis cannot see) |
| `tests/codegen/iter_named_getenumerator_reenum_2376_test.cpp` | `named_local_owned_param_second_getenumerator_raises_ioe_spec_29_8` (:269) | run, second `GetEnumerator()` raises → **rejected** (same ground) |
| `tests/ownership/state_machine_param_conditional_discharge_2310_test.cpp` | none — every pair of loops iterates two **separate** calls (`Use(new Cell(), true)` then `Use(new Cell(), false)`); SIMPLIFY's column over-counted | unchanged |
| `tests/codegen/async_iter_reiterate_bound_source_1268_test.cpp` | none — `owned_param_iterator_self_enumerator` (:186) loops once; the re-iteration tests use the parameterless `Gen()` | unchanged |
| `tests/codegen/async_iter_early_abandonment_dispose_1525_test.cpp` | none — `reiteration_after_abandonment_independent_disposal` re-iterates the parameterless `C.Gen()`; `D.Gen(new Cell())` loops once | unchanged |

Lit: 0 (the three owned-parameter iterators are declared, never enumerated twice). Rule corpus: `s29_8_0001`–`0004`, `s29_8_0021`, `s29_8_0022`, `s29_4_0004` each enumerate a fresh call once (0 flips; three `rule:` headers quote the old sentence and are refreshed, §8). Doc corpus: `spec_0242`, `spec_0243` declare and never enumerate (0 flips; `spec_0243` mirrors the Echo comment). Guides, code-generation reference, stdlib, rationale: 0 re-enumerations. The 4 iterators that move the parameter (`ReadAndClose`) keep compiling: the body runs once.

Newly rejected: **5 gtest programs**, all the mechanism's own tests. Newly admitted: nothing.

## 3. Options

- **A — status quo.** Re-iterable; the parameter's holder unstated. Unsound (§1.1).
- **B — single consumption (the root's (a); SIMPLIFY C4).** The result of an iterator declaring an `owned` or move-only parameter is affine under §29.11; the body runs once; the parameter rides with the one enumerator.
- **C — the enumerable owns the parameter (the root's (b), its recommendation).** Each enumerator borrows it; disposed at the enumerable's disposal; a `move` of such a parameter in a re-iterable body is rejected (`ReadAndClose` becomes `IEnumerator<string>` or loses the move).
- **D — `owned` parameters only at the single-use enumerator types (the root's (c)).**

## 4. Evaluation

| | Soundness | Simplicity | Consistency | Migration reach |
|---|---|---|---|---|
| A | hole (five escapes) | — | — | — |
| **B** | closed: one body run, one holder at every instant (frame → enumerator → disposal) | one rule the language already has, cited; ~8 lines of §29.8 shrink to a cross-reference | matches §29.11's own streaming-reader sentence, K6.1's affine bound, and the compiler's existing claim-once for part of the shape | 5 test programs; 0 everywhere else |
| C | closed for re-enumeration; nested enumerators sound | a new ownership shape (enumerable owns, enumerators borrow) and a new rejection (no `move` in a re-iterable body) | rejects §29.8's own warning-free idiom in every re-iterable iterator; changes the must-use story | 4 moving iterators re-declared or rewritten; the `using var … := move …` adoption idiom lost at `IEnumerable<T>` |
| D | closed | one declaration rule | loses `for` over the call's result at the common shape | 87 declarations |

B is the simplest sound option and the owner's stated preference (narrow a feature whose measured reach is negligible rather than add a rule). The deviation from the root's recommendation (C) is on the idiom cost: C rejects the one spelling §29.8 recommends.

## 5. Decision (delegated ruling, 2026-10-08)

**Option B.** Normatively:

1. ▲ §29.3: at every re-iterable return type, an iterator method that declares an `owned` or move-only parameter returns a **single-consumption** iterable: its body runs once.
2. ▲ §29.8: the call transfers the argument into the frame; the iterable is single-consumption under exactly §29.11's rule — consumption points (`for`/`await for`, a terminal operator, `GetEnumerator()`/`GetAsyncEnumerator()` or the owning form, a further deferred operator, passing as an `IEnumerable<T>`/`IAsyncEnumerable<T>` argument), the static error where visible, `InvalidOperationException` at run time, the remedies (materialize at a value element type; lend a class-typed resource as a plain parameter). The first `GetEnumerator()` transfers the frame's `owned` and move-only parameters to the one enumerator, which disposes any not moved out of when it completes (a body fault included, #5765) or is disposed; an iterable from which no enumerator was produced disposes them itself. Single-use return types: unchanged.
3. ▲ §29.11: the opening "works as expected" sentence names the exception; the streaming-reader parenthetical names the owned-parameter iterable as that case by rule, a `GetEnumerator()` call counting as the enumeration it begins.
4. ▲ K6.1 states the owned-parameter iterable's affinity beside the lazy query's — its carried sources keep the iterator frame's bound (completion or disposal), only the enumeration right is affine — and Appendix C.3 lists the rule.

Kernel classification: **derivation** — K6.1's affine bound applied to a second frame kind; K3 (the call-site transfer) and K1 (one holder) unchanged; no new clause. Body-locality: the static check is §29.11's existing provenance tracking, **local** to the consuming body. It keys on the binding's initializer being a call statically resolved to an iterator method whose declaration carries an `owned` or move-only parameter — a declaration fact of the callee (its `owned` parameter is in the signature; that it is an *iterator* is a fact of its body, which a direct same-unit call resolves). Through virtual or interface dispatch, a function value, or a callee whose iterator-ness the artifact interface does not carry, the analysis cannot see the iterable as affine and the run-time `InvalidOperationException` guard is the protection — §29.11's own "where the flow analysis can see it" limit, as for a lazy query passed through an `IEnumerable<T>` parameter. *(Critic amendment: the lane's draft called the key "a signature fact".)*

## 6. Edits at graduation

The exact edits are `scratch/v0334/W7.json` ids `W7-0167-1` … `W7-0167-12` (spec §29.3, §29.8 ×4, §29.11 ×2, K6.1, Appendix C.3; rationale §5.12; code-generation reference §7.5; Guide 05). Composition: `W7-0167-3` carries #5765's parameter-moment sentence, so the two rulings share one anchor by design.

## 7. Compiler facts and the tracker

- `src/codegen/LlvmBackend_Iterators.cpp` (the #2154 capture claim; `GetEnumerator` at vtable slot 4 returning a clone) and the 2376 test's note: the claim arms only for the conditionally-discharged shape; a plain owned parameter is cloned into every enumerator. The async twin (#2356) claims at `GetAsyncEnumerator`.
- No static single-consumption check exists for an iterator result (§29.11's exists for lazy queries).
- **Tracker (spec-tracking):** arm the claim for every iterator with an `owned` or move-only parameter, sync and async, so a second `GetEnumerator()`/`GetAsyncEnumerator()` raises `InvalidOperationException`; extend §29.11's static consumption tracking to such a result; move the parameter's disposal to the one enumerator (and to the enumerable where no enumerator was produced). Flips: the five programs in §2. #4828/#4827 close under it.

## 8. Corpus cases to write (after landing)

- `s29_8_*`: a second `for` over one bound owned-parameter result (reject, statically visible); the same through an `IEnumerable<T>` parameter boundary (run: `InvalidOperationException`); a never-consumed bound result disposes the parameter at scope end (run: `closed` after the block); `GetEnumerator()` twice (reject). Headers to refresh: `s29_8_0002`, `s29_8_0003`, `s29_8_0022` (they quote "When the iterator is disposed, the state machine disposes…" / "at iterator disposal"; verdicts unchanged).
- Doc corpus: `spec_0243` mirrors the Echo comment (#1380, by hand).

## 9. Declined alternatives (one `declined-at-proposal` row each)

- **C** — the enumerable owns the parameter, no `move` in a re-iterable body. Reopening bar: a measured program that must re-enumerate an iterable over a resource it transferred in and cannot lend the resource instead.
- **D** — `owned` parameters only at the single-use enumerator types. Reopening bar: —(none stated).

## 10. Self-critique (adversarial pass)

1. **Does "consumption" cover the manual protocol?** §29.11's list names enumeration, operators and argument passing; a bare `GetEnumerator()` is the protocol face of enumeration. The rule says so explicitly (a `GetEnumerator()` call counts as the enumeration it begins), which is what 2376 already pins at run time.
2. **The never-consumed iterable.** Its parameter is disposed at the iterable's disposal (scope end, K8 boundary, or an explicit kill) — R8/C1's gap closed; under a plain `var` the must-use lint fires on the ordinary terms, as for a lazy query.
3. **Nested loops over one binding.** The inner `for` is a second consumption on the same path: a static error. A consumer that needs nesting lends the resource.
4. **The iterable disposed while its one enumerator is live.** The parameters already moved to the enumerator; the enumerable's disposal disposes nothing of them (R8/C9's route: which kill discharges — the enumerator's, once the right is spent; the enumerable's, before). A `GetEnumerator()` after the enumerable's disposal is §28.2's member access on a disposed instance.
5. **Owning return types.** `IOwningEnumerable<T>` with an `owned` parameter is the same rule; the drain protocol already consumes once in practice.
6. **Async.** `IAsyncEnumerable<T>`/`IOwningAsyncEnumerable<T>` with an `owned` parameter: `await for` or `GetAsyncEnumerator()` spends the right; the compiler already claims there (#2356).
7. **Pass-through to a callee.** Passing the iterable as an `IEnumerable<T>` argument transfers the right (§29.11 point 3); inside the callee the parameter is statically untracked and the run-time guard holds — the same limit as for lazy queries, stated there.
8. **Is this a narrowing of §29.3 or a clarification?** A narrowing: §29.3 said "re-iterable" for every `IEnumerable<T>` iterator. Hence the proposal and the §10-shaped reach measurement.
9. **Could the compiler's existing run-time behaviour be the whole rule (no static error)?** §29.11 already gives lazy queries both faces; giving this iterable the run-time face only would be a second rule. Taking §29.11 whole is the smaller text and the critic's "fix the rule" preference is not engaged (the feature is narrowed, not re-ruled).
10. **(Critic) A right spent by transfer.** A further deferred operator or an `IEnumerable<T>` argument spends the right without producing an enumerator; if the composed query or the callee never enumerates, no enumerator ever takes the parameters. The holder rule is therefore keyed on whether an enumerator was produced, not on whether the right was spent: the iterable disposes every parameter no enumerator took.
11. **(Critic) Remedies at a class element type and for move-only arguments.** §29.11's remedies paragraph forbids materialization over a minting source at a class element type (dead borrows), and a move-only argument cannot be lent by a plain parameter (plain passing moves it, K3.12). The §29.8 remedy sentence says "materialize first at a value element type" and "take a class-typed resource as a plain parameter"; a move-only argument has no re-iterable spelling and is consumed once.
12. **(Critic) K6.1's bound.** The draft placed the owned-parameter iterable under the lazy query's *bound* (its single consumption point); its carried borrows (a plain parameter beside the `owned` one) must outlive the enumeration, not its start, so it keeps the iterator frame's bound and only shares the affinity.
