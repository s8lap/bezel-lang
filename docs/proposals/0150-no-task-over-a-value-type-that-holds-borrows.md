# Proposal 0150 — No `Task<T>` over a value type that holds borrows

| | |
|---|---|
| **Status** | graduated-v0.331 — superseded — graduated v0.331 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option D, a §30.5 producer row rejecting `Task<T>` at the declaration over a value type that holds borrows, `CancellationToken` excepted; critique round 1 applied, 4 blocking and 8 non-blocking findings resolved, shared with the #5678 ruling; that round reclassified the rule from a ruling to this design change, corrected the standard-library retention survey, whose value-copy members are filed as their own question, and carved `string` and `Type` out of the #5678 plain-row rule; landed with the v0.331 changelog entry) |
| **Tracking issue** | [#5674](https://github.com/s8lap/bezel-lang/issues/5674) (borrow-flow matrix root cause B1, decided under the delegation); one `spec-tracking` compiler tracker is filed at landing (§6.4) |
| **Target module(s)** | **spec**: §30.5 (one clause on the copyable row, one new producer row, the `FromResult` paragraph and the `TaskCompletionSource` split sentence), §30.10 (the `FromResult` bullet), §30.11 (the listing comment), §9.7 (the async parenthetical), Appendix C.6 (one bullet). **stdlib**: §6.6 (the listing comment), §6.7 (the `FromResult` and `FromException` comments). **code-generation reference**: §5.6 (two index comments) and §7.6 (one *Looks right, is rejected* pair). **`DECLINED.md`**: four `declined-at-proposal` rows |
| **Written against** | spec **v0.330** at `1ec0886` (main). The stdlib companion, rationale and code-generation reference are at the same commit. |
| **Origin** | The v0.330 borrow-flow matrix audit (`scratch/matrix/`, root cause B1 = X1-A): cells R3–R6 × C8 are `missing`, and R8 × C8's standard-library leg is forwarded to them. The lane (`scratch/v0331/T3.md`) drafted the rule as a ruling. Its critic (`scratch/v0331/T3.critic.md`, finding F1) reclassified it as a design change, because it removes a stated admission; this proposal records that decision. |

*Reference convention: unprefixed §-references are language-spec references at `1ec0886` (v0.330), and K tags are §28.3's kernel ids. Quotations are verbatim. ▲ marks proposed text. The programs below use only members the stdlib companion declares.*

---

## 0. Thesis

§30.1 forbids a value type that holds borrows as an `async` method's result: "nor a result/element type that is such a value type". The ground is a frame that outlives the synchronous call. The rule, however, "keys on the **`async` modifier**". The same `Task<T>` can be completed by producers that carry no such modifier: `TaskCompletionSource<T>.SetResult`/`TrySetResult` (§30.11) and `Task.FromResult<T>` (§30.10). §3.6 rule 2 exempts their stdlib bodies from §9.7's per-instantiation reading ("a standard-library generic type's members' bodies … contribute no requirement of this kind"). No instantiation-site rule bars such a T either. The result is that a borrow-bearing record, inline array, choice or span is retained in a task's result slot, which every awaiter reads after settlement and for as long as the task lives, with no bound:

```bezel
class Item  public property Count: int => 1;  end Item;
record Pair(Item: Item);
class Svc
  private owned _tcs: TaskCompletionSource<Pair>;   // admitted at v0.330 ("copyable value type")
  public constructor() _tcs := new(); end;
  public method Arm()
    using var it := new Item();
    _tcs.SetResult(new Pair(it));                    // the task retains a borrow of `it`
  end Arm;                                           // `it` disposed
  public async method Peek(): Task<int>
    var p := await _tcs.Task;
    return p.Item.Count;                             // flag-caught for a class; a span has no flag
  end Peek;
end Svc;
```

The text states this admission three times. §30.5's first producer row reads "`Task` and `Task<T>` for **copyable** value type …: produced as today". §30.11's listing comment reads "Intrinsic restriction: T must be a copyable value type, string, or other runtime-managed reference type". Stdlib §6.6 reads "For non-class T (copyable value types, string, runtime-managed reference types)". Narrowing it changes what compiles under a stated rule, and no prior revision scheduled the change. Process §2 therefore classes it as a **design change**. The v0.274 gate refuted the closest precedent, which drew an `EventSource<T>` payload restriction from §30.1's conditional rule, as "an absolute rule drawn from a conditional" (`DECLINED.md`, the `EventSource<T>` row). The new rule therefore has to be stated on its own, not derived.

This proposal adds a §30.5 producer row beside the move-only one. A value type that holds borrows is admitted by no row, so `Task<T>` over one is a compile error at the declaration, re-read through user generics, with `CancellationToken` excepted on §30.1's A10 footing.

**Measured reach: zero sites flip** in `tests/` (gtest, lit, both conformance corpora), the doc examples and the guides (§2).

---

## 1. Ground truth

### 1.1 The rule today

- §30.1: the borrow-bearing rule rejects such a result type for "an `async` method, `async` lambda, or async iterator". For a generic async method it is "re-applied after substitution". It is scoped to the modifier: "a standard-library position whose declared signature carries none … is outside the rule's scope and states its own retention and obligation where it is declared. There is exactly one such position: `AsyncEventSource<T>.RaiseAsync`." `TaskCompletionSource<T>.SetResult` is a second such position, and it states no retention.
- §30.5's move-only row is the shape this proposal copies: "A **move-only value T** … is admitted by **no row**: declaring `Task<T>` or `OwningTask<T>` over such a T is a **compile error at the declaration** … re-applied after substitution". The row's closing words, "as for the borrow-bearing rejection ([§30.1])", already treat the two rejections as siblings.
- §9.7's instance-field bullet: "a record-carried borrow … may be consumed through access paths with **no per-access disposed-flag check** (a span's buffer window is the canonical case)". No trust residual is available for record-carried borrows.
- §3.6 rule 2: a standard-library type "is rejected at an instantiation only by its declared constraints and the instantiation-site rules stated for it". A rule therefore has to be *stated* at the type.

### 1.2 The compiler today (`src/` at `1ec0886`)

- `src/ownership/Ownership.cpp:545–551`: the §30.1 signature check `checkAsyncSignatureBorrowBearing` runs only `if (isAsync_)`. `Ownership_Checks.cpp:18936–18963` reads the generic argument of `Task<T>` there. Nothing keys on the `Task<T>` *type*.
- `src/sema/TypeResolution_Names.cpp:256–263` registers the `TaskCompletionSource` arities and runs no instantiation check. The stated class-T rejection is itself unenforced: rule case `l6_6_0001` is banked `fail divergence` in `tests/conformance/rules/baseline.tsv`.
- `Task.FromResult` at a class T is rejected (`tests/codegen/task_fromresult_class_reject_1107_test.cpp`).
- The rule is therefore **spec-ahead-of-impl** in full.

---

## 2. Measurement

The lane ran a sweep over `tests/` (gtest, lit, `tests/conformance/cases`, `tests/conformance/rules/cases`) and `docs/` for `Task<X>`, `TaskCompletionSource<X>`, `FromResult`, `WhenAll<X>`, `WhenEach<X>`, `FromException<X>` and `FromCancelled<X>` at a non-primitive X. It classified each X by its declaration and found 32 distinct X. The critic re-ran the `TaskCompletionSource<X>` tally: `T` 102, `int` 35, `MyClass` 6, `Res` 5, `Resource` 4, `P` 4 (a class), `Widget` 3, `string`/`long`/`int?` 2 each, `Cell` 1 (a class), and `BoxKD4305j` 1 (`record BoxKD4305j(N: int)`, borrow-free). Every X that holds borrows sits in an `async` signature that §30.1 already rejects: `async_borrow_soundness_1518_1519`, `record_span_field_escape_2127__async_result_rejected`, `s30_1_0015`, and `choice_borrow_field_owning_4708_test.cpp:320`. In that last test, `FromResult` gains a second rejection at the call and the expected diagnostic is unchanged.

- **Tests that flip:** none.
- **Doc examples that flip:** none. Guide 04 has no `TaskCompletionSource` or `FromResult` at a borrow-bearing payload.
- **Rule corpus:** no case states the old admission. `s30_5_0013` and `s30_10_0011` quote "is restricted to non-class T", which is kept verbatim. `corpus.py lint --strict` reports 0 problems on a scratch copy with the edits applied.

---

## 3. Options

### 3.1 Option A: status quo
Keep the admission. Unsound for a span (no flag) and flag-only for a record over a class, while §9.7 refuses that regime to record-carried borrows everywhere else. **Declined.**

### 3.2 Option B: a K4.3 trust clause for the retention
Record "every carried source outlives the task's last result read" as a residual. K4.3 requires a named backstop, and a span window has none. **Declined.**

### 3.3 Option C: reject at the retaining members only
Reject `TaskCompletionSource<T>` at such a T, plus `FromResult<T>`'s call, and leave `Task<T>` declarable. This is sound. It needs two statements where one suffices. It also leaves a `Task<Pair>` type that only `FromException<Pair>` and `FromCancelled<Pair>` can produce, so the type never carries a value, and `async` producers already cannot form it (§30.1). **Declined** for simplicity and consistency.

### 3.4 Option D: a §30.5 producer row at the type (**recommended**)
The type is ill-formed over a value type that holds borrows. That one sentence covers every producer (`SetResult`, `FromResult`, `WhenAll`, `WhenEach`, `FromException`, `FromCancelled`) and every written `Task<Pair>`, in the move-only row's form. The row also states the instantiation-site rule §3.6 rule 2 needs: `Fwd<Pair>` is rejected wherever named when `class Fwd<T>` forms `Task<T>`. `CancellationToken` is excepted, as in §30.1.

### 3.5 Option E: a general §3.6 rule over every retaining stdlib member
"A standard-library member that retains a T past its call admits no T that holds borrows unless its section tracks them." The critic's survey (`T3.critic.md`, F2) finds a second retention family: the value-copy members of the borrowing collections and LINQ (`Array<T>.ToArray`/`Sorted`/`Reversed`/`AddRange`, `LinkedList<T>(source)`, `ToSet`, `ToDictionary`, and the span views' `Fill`/`CopyFrom`/`CopyTo`). They copy T values into storage inside an exempt stdlib body. Their holder, however, is a collection the caller holds, so the natural bound is source propagation (the copy carries the copied elements' sources), not rejection. A uniform rejection would forbid `var ys := xs.ToArray();` over a local `Array<Pair>`. **Declined**, and the second family is filed as its own question.

---

## 4. The options side by side

| | Soundness | Simplicity | Consistency | Reach |
|---|---|---|---|---|
| A status quo | unsound (span) | — | contradicts §9.7's no-residual stance | 0 |
| B trust clause | unsound (span) | one clause | contradicts §9.7 | 0 |
| C member-level | sound | two statements | partial: leaves `Task<Pair>` declarable while §30.1 forbids it for `async` | 0 |
| **D type-level** | **sound** | **one row** | **mirrors the move-only row and §30.1** | **0** |
| E general rule | sound, over-rejects | one sentence, many readings | wrong shape for collection copies | unmeasured |

---

## 5. Decision (delegated ruling, 2026-10-08)

### 5.1 The rule
Option D. A value type that holds borrows (§3.6's holds-borrows property, with `IReadOnlySpan<TElement>` read as the span it boxes) is admitted by no §30.5 producer row. Declaring `Task<T>` over such a T is a compile error at the declaration, at every site that forms the type: a written field, parameter, local or return type, and the instantiations `TaskCompletionSource<T>`, `Task.FromResult<T>`, `Task.WhenAll<T>` and so on, explicit or inferred. As an instantiation-site rule in §3.6 rule 2's sense, it is re-read through a user generic's substitution. A dispatched generic method's body that forms `Task<T>` at its own unconstrained parameter therefore meets §14.2's *Requirements are decided at the body*, exactly as §30.1's async result check and `FromResult`'s class rejection already do (remedy: `where T: sendable` or `where T: class`, or a named type). `CancellationToken` is excepted on A10's footing. Its result carries §30.8's source-outlives-token obligation, which the checked-mode flag backstops because every token access passes through the tracked source.

### 5.2 Kernel classification
K2.1 + K6.1 (§30.1's derivation: the slot outlives every synchronous frame), with no K4.3 residual for record-carried borrows (§9.7). The A10 exception is unchanged.

### 5.3 Body-locality
Decided at the type's formation site and per instantiation. No callee body is read.

### 5.4 Implementation sketch
Lift the `isAsync_`-gated leg of `checkAsyncSignatureBorrowBearing` into a type-formation check on `Task<T>` (sema, at type resolution and generic substitution), sharing the holds-borrows predicate. `TaskCompletionSource<T>` and the `Task` statics inherit it through their `Task<T>`.

### 5.5 Tests that flip
None (§2).

### 5.6 `DECLINED.md` rows
Options A, B, C and E, entered as `declined-at-proposal` (the exact rows are in `scratch/v0331/T3.json`, edit `T3-5674-declined-at-proposal-rows`).

---

## 6. Edits at graduation (each `old` unique at `1ec0886`; the exact JSON is `scratch/v0331/T3.json`, the `T3-5674-*` and `T3-codegen-7.6-*` edits)

### 6.1 Changelog
The v0.331 entry's T3 block (`scratch/v0331/T3.md`, *Changelog bullets*) names this proposal and its declines.

### 6.2 Migration
*Newly rejected (spec-ahead-of-impl):* `Task<T>` at a value type that holds borrows at every non-`async` site (`TaskCompletionSource<Pair>`, `Task.FromResult(span)`, a `Task<[2]Item>` field, `FromException<Pair>`, `Fwd<Pair>`). Measured reach: zero. *Newly admitted:* nothing. *Doc-only:* the pointer clauses.

### 6.3 Corpus
Cases to write after landing (§30.5; stdlib §6.6, §6.7):
- Reject: `TaskCompletionSource<Pair>`, `Task.FromResult(buf.AsReadOnlySpan())`, a `Task<[2]Item>` field, `TaskCompletionSource<Ref>` for a choice that holds borrows, and `Fwd<Pair>`.
- Accept: `TaskCompletionSource<CancellationToken>`, and `TaskCompletionSource<Pt>` for a borrow-free record.

### 6.4 Trackers to file at landing
`spec-tracking`: `Task<T>` at a value type that holds borrows is unchecked outside `async` signatures (§30.5, v0.331). The only check, at `Ownership.cpp:550`, is gated on `isAsync_`. The class-T rejection of `TaskCompletionSource<T>` (rule case `l6_6_0001`) is also unenforced.

---

## 7. Self-critique

- *Is the type-level reach gratuitous?* It rejects `FromException<Pair>` and `FromCancelled<Pair>`, which retain no value. That is the price of one rule. Such a task never carries a value, and the measured reach is zero.
- *Over-rejection at dispatched generics:* an interface method `GetAsync<T>(): Task<T>` with unconstrained T meets §14.2's body rule. Today every implementing body already meets it through §30.1 (an `async` body) or `FromResult`'s class rejection (a non-`async` body). No such method exists in `tests/` or `docs/`.
- *`CancellationToken`:* kept on §30.1's footing. Narrowing it at the non-`async` producers would be a one-clause change with zero measured reach. It is left as an open question (`T3.md`, *New questions* 2).
- *The second retention family (option E's evidence)* is out of #5674's narrowest fix. It is filed as a new question, not decided here.
