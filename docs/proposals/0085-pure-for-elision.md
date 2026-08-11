# Proposal 0085 — Pure-body `for` over concretely-typed arrays and strings: make §29.2's elision normative

| | |
|---|---|
| **Status** | under-review — drafted 2026-08-11 (owner-directed, from the elegance review) |
| **Tracking issue** | [#3051](https://github.com/s8lap/bezel-lang/issues/3051) |
| **Target module(s)** | **spec** — §22.15 body rule 5 (the amendment), §29.2 (one cross-reference sentence noting the pure-body normativity of the already-admitted elision). **stdlib** — none. **rationale** — §9.7 area (`pure` is declared, not derived) gains a one-paragraph record. **guides** — Guide 06 §6's "the rule that surprises people" passage rewritten at graduation. |
| **Written against** | spec/stdlib v0.241 |
| **Origin** | Elegance review 2026-08-11, finding 5: inside a `pure` body, `for var c in text do … end;` is rejected and the remedy is a hand-written index loop — the cost landing on the single most common loop shape in the language. The widening reuses an elision the spec already blesses (§29.2: direct indexed loops for `Array<T>`, `OwningArray<T>`, and `string` "when the compiler can see the type concretely") and a move the pure fragment already made once (§22.15 rule 5: the `Range<T>` counted loop is "the normative semantics inside a pure body"). The rationale is silent on this specific widening — the pure-iteration question is recorded as deliberately open, but the cheap reuse was never weighed. |

*Reference convention: unprefixed §-references are language-spec references.*

---

## 0. Thesis

§22.15 body rule 5 admits a `for` loop in a pure body exactly when its desugaring constructs no enumerator, and it already contains the template this proposal instantiates: for `Range<T>` of primitive steppable type, the direct counted loop — *"elsewhere an admitted elision"* — is *"**the normative semantics inside a pure body**."* Meanwhile §29.2's closing paragraph already expects the same elision for the concrete sequence types: *"For heap arrays (`Array<T>`, `OwningArray<T>`) and strings, the compiler is expected to emit direct indexed loops as an optimization (these types do implement `IEnumerable<T>`, but the protocol is elided when the compiler can see the type concretely)."*

The proposal: extend rule 5's normative-in-pure-bodies status to exactly those concretely-typed sources. `for var c in text do … end;` becomes legal in a pure body with the direct-indexed semantics; interface-typed sources stay rejected; nothing outside pure bodies changes.

## 1. The rule-text amendment (§22.15 rule 5, drop-in)

Current rule 5 sentence (v0.241, verified):

> A `for` loop is admitted when its desugaring does not construct an enumerator: over `Range<T>` of primitive steppable type, the direct counted-loop form (§16.5, §29.2) — elsewhere an admitted elision — is **the normative semantics inside a pure body**; over inline arrays, spans, `ReadOnlyStringSpan`, and value-typed `OwningBuffer<T>` the desugaring is already the direct indexed loop. A `for` over an `IEnumerable<T>`-typed source (including `Array<T>` and `string`) is **rejected**: the desugaring constructs an enumerator class instance; the remedy is an index loop (`for var i in 0..<xs.Count do … xs[i] … end;`).

Proposed replacement:

> A `for` loop is admitted when its desugaring does not construct an enumerator: over `Range<T>` of primitive steppable type, the direct counted-loop form (§16.5, §29.2) — elsewhere an admitted elision — is **the normative semantics inside a pure body**; over inline arrays, spans, `ReadOnlyStringSpan`, and value-typed `OwningBuffer<T>` the desugaring is already the direct indexed loop; and over a source whose **static type at this body's own check site** is concretely **`Array<T>`, `OwningArray<T>`, class-typed `OwningBuffer<T>`, or `string`**, [§29.2](…)'s admitted protocol elision is likewise **the normative semantics inside a pure body**: the compiler emits the direct indexed loop — a bounds read and per-element reads, rule-1 material — and constructs no enumerator. The iteration variable's type and state are [§29.2](…)'s for the elided form (a value copy for value elements and `string`; a borrow of the element for class-typed elements — default iteration over `OwningArray<T>` remains borrowing). A source whose static type is an **interface** (`IEnumerable<T>` included) or a **generic type parameter** — even one instantiated at a concrete sequence type — remains **rejected**: the remedy is an index loop (`for var i in 0..<xs.Count do … xs[i] … end;`). The structural-modification fault of [§29.12](…) is preserved by the elision as everywhere; inside a pure region no admitted operation can structurally modify a collection (stores through class-typed paths and non-pure calls are both prohibited), so the fault path is statically unreachable there and an implementation may omit its check in pure bodies with no observable difference.

§29.2's closing paragraph gains one sentence: *"Inside a `pure` body this elision is normative for the concretely-typed sources, per §22.15 rule 5."*

## 2. The concrete-type visibility condition, argued

**"Concretely typed" means the declared static type at the pure body's own check site — generic type parameters do not qualify, even when every instantiation substitutes a concrete sequence type.** Argued, against the live counter-position:

The counter-position is real: §3.6 rule 2's per-instantiation pattern is used throughout §22.15 itself — the move-only parameter and return checks, and owning-choice construction, are all "re-applied after substitution, at the instantiation site." Why not admit `for var x in xs` for `xs: T` per-instantiation when `T = Array<int>`?

Three reasons to reject it, the first decisive:

1. **§22.15's checking discipline is single-pass over bodies, by design.** The dispatch subsection states it as a load-bearing property: *"A generic pure member is checked once, at its declaration, under its constraints … Nothing purity-relevant depends on the substitution."* The existing per-instantiation checks in §22.15 are **signature-level** facts (a parameter or return type turning move-only under substitution) — re-checked without re-entering the body. Admitting loops per-instantiation would make *body statement admissibility* substitution-dependent — a per-instantiation body re-check pass that falsifies the quoted sentence and changes the checker's architecture for one loop form.
2. **The elision itself is keyed on concrete visibility.** §29.2's condition is *"when the compiler can see the type concretely"*; at a bare `T` the emitted code would have to dispatch through the protocol — precisely the enumerator construction rule 5 exists to exclude — or specialize per instantiation, which is reason 1 again.
3. **The status quo at generic sources is not worsened.** A generic pure body looping a `T`-typed source is rejected today and remains rejected; no constraint vocabulary exists that would make its index-loop remedy writable either (no indexer constraint) — so the generic case is not a regression surface, merely untouched. If a future revision adds sequence constraints, the question reopens there with its own machinery.

The condition is applied *after* the surrounding member's own generic substitution only in the trivial sense that a **non-generic** pure body inside a generic *type* sees the enclosing type's parameters as bare parameters too — same rule, no special case.

## 3. Span types — checked and decided

The critic asked whether `Span<T>`/`ReadOnlySpan<T>` sources should join. Checked against v0.241: **they are already admitted** — rule 5's existing text names "spans" alongside inline arrays, `ReadOnlyStringSpan`, and value-typed `OwningBuffer<T>` as sources whose desugaring *"is already the direct indexed loop"* (these types do not implement the enumeration protocol at all, §29.2), and `ArraySpan<T>`/`ReadOnlyArraySpan<T>` share that status (§29.2). Nothing to do.

Two neighbors decided here:

- **Class-typed `OwningBuffer<T>`** — **included** in the amendment (it is the one §4.14 storage type on the *protocol* path: §29.2 says it "implements `IEnumerable<T?>` and the standard enumerator path applies, with the optimization-elision rule above; the observable result is the same as a direct-indexed loop"). Excluding it would leave a one-type hole in the §4.14 family for no stated reason. Its iteration variable is `T?` — a possibly-null borrow — all rule-1 reads.
- **`IReadOnlySpan<TElement>`** — **excluded** (stays rejected). Its §29.2 lowering is a direct indexed loop, but through **interface-dispatched** `Length`/indexer accessors — rule-6 *calls*, admissible only if the interface members are `pure`-declared, which stdlib §4.16 does not currently state. Admitting it is a stdlib-contract question (mark the interface's read surface `pure`), severable and recorded as Q3, not smuggled in here.

## 4. Purity coherence — each operation of the elided loop, admitted

The elided loop's operations, checked against the pure-body list:

| Operation | §22.15 admission |
|---|---|
| bounds read (`Count` of `Array<T>`/`OwningArray<T>`/`OwningBuffer<T>`, `Length` of `string`) | the loop is compiler-emitted: the bound is read as an intrinsic length fact of the concrete layout, exactly as the `Range<T>` counted loop's endpoints are — rule-1 material, not a rule-6 member call. (Where an implementation lowers it as the accessor call, the accessors in question are the stdlib's intrinsic reads — `string.Length`/the indexer are `pure` per stdlib §9.6 — so both lowerings satisfy the fragment; the spec text says "a bounds read," committing to neither.) |
| index increment, compare | rule-2 frame-local mutation of a compiler temp |
| element load (`xs[i]`, `text[i]`) | a read of reachable storage — rule 1 (value copy or element borrow; a borrow is "readable, never consumed" per the signature rules) |
| the loop variable binding, per iteration | rule-2 locals |
| body | unchanged — checked under the ordinary pure-body rules |
| enumerator construction | **none exists** — that is the amendment's entire content |

Mutation-during-iteration (§29.12): the elision must ordinarily preserve the structural-modification fault. Inside a pure region, structural modification is unreachable — `Add`/`Remove`/`Clear`/`Sort` are non-pure members (rule 6 excludes them) and element stores through a class-typed path are rule-2-prohibited — so the fault-preservation obligation is vacuously met; the amendment says so and licenses omitting the check there (observable-identical). Element mutation `xs[i] := v` is likewise already prohibited (store through a class-typed dereference), so the borrow-invalidation corner cannot arise.

Ownership coherence: the loop introduces no owned values (borrow/copy iteration variables only), so the pure fragment's no-discharge-machinery invariant is untouched; `OwningArray<T>` default iteration stays borrowing (§29.2's rule — the *owning* path requires `Drain()`, whose call is non-pure and stays excluded by rule 6, and whose consuming iteration would mint owned locals the fragment cannot discharge — the exclusion is two-deep, stated for the record).

## 5. Corpus evidence

- **The workaround is live in the corpus:** 26 `for var i in 0..<` index loops in `tests/`, 19 in `docs/` (grep, v0.241 HEAD) — among them Guide 06 §6's own `CountOf` example, written in the index form solely because of rule 5, with the rejected `for var c in text` shown beside it as a comment.
- **The flip-polarity test:** `tests/sema/pure_body_22_15_test.cpp`, case `pure_enumerable_for_rejected_spec_22_15` (an `Array<int>`-sourced `for` in a pure body, currently `EXPECT hasMsg("for loop source")`) — becomes an **acceptance** test under this proposal; the rejection test is rewritten over an interface-typed source, which the amendment keeps rejecting. The neighboring `Range` acceptance case (`0..<10`) is unaffected.
- **No accepted program changes:** the amendment admits previously-rejected programs only (a pure widening); non-pure bodies are untouched (the elision there remains the §29.2 optimization it always was).

## 6. Alternatives considered

1. **Status quo.** The surprise stays on the most common loop shape, taught as a trap (Guide 06 §6), with a mechanical workaround the compiler could write itself. Declined: the `Range<T>` precedent shows the fragment already crossed this exact bridge — "elsewhere an admitted elision, normative here" — and stopping at ranges was never argued anywhere findable.
2. **Per-instantiation admission for generic sources.** Declined with reasons in §2 (the checking-discipline argument decisive).
3. **A pure-iterator design** (admit the protocol by making enumerators pure-constructible). Declined here: §22.15 records the pure-iterator form as "deliberately left open, not designed" — a genuine design (frame allocation, disposal timing, laziness vs. the effect fence) that this proposal's cheap widening neither needs nor prejudices.
4. **Admitting `IEnumerable<T>`-typed sources with a runtime-elided fast path.** Declined: at an interface type the elision condition ("see the type concretely") is false by construction; admission would put enumerator construction — class construction, prohibited — behind a dispatch the checker cannot see through. The interface rejection is the rule's honest boundary, not a gap.
5. **Also admitting `IReadOnlySpan<TElement>` now.** Deferred to Q3 (§3) — severable stdlib-contract work.

## 7. Migration & compatibility (changelog house categories)

- **Newly accepted:** `for` in pure bodies over concretely-typed `Array<T>`, `OwningArray<T>`, class-typed `OwningBuffer<T>`, and `string` sources. Pure widening; no meaning change to any accepted program.
- **Newly rejected / newly warned:** nothing.
- **Doc-only:** §29.2's cross-reference sentence; Guide 06 §6 rewrite at graduation (the index loop demotes from "the remedy" to "the generic-source remedy"); the rationale record.

## 8. Implementation sketch

- **Compiler areas:** `src/sema` — the pure-body checker's rule-5 keying (extend the admitted-source type test); `src/codegen` — the direct-indexed lowering for these types must actually run for pure bodies. §29.2 frames the elision as expected-but-optional in general; **inside pure bodies it becomes mandatory**, so if bezelc currently lowers concrete `Array<T>`/`string` `for` loops through the enumerator path, the elision must be implemented at least for pure bodies first — the implementation-order note the tracking issue carries. Size: **S** (sema) + **S/M** (codegen, depending on the elision's current state).
- **Tests shipped with the flip** (spec-cited; fail-without/pass-with): acceptance + value-correctness of `for var c in text` and `for var x in xs` (`Array<int>`) in pure bodies (§22.15 rule 5 as amended); borrow-element iteration over `Array<C>`/`OwningArray<C>` receivers with rule-1-only bodies; continued rejection at an `IEnumerable<int>`-typed source and at a generic `T` source (§2's condition); the rewritten `pure_body_22_15` rejection case; a §29.12-shaped test documenting that no structural modification is expressible in the pure region (compile-error pin, not behavior).

## 9. Open questions

- **Q1 — the type set.** Confirm the four types (`Array<T>`, `OwningArray<T>`, class-typed `OwningBuffer<T>`, `string`); or drop `OwningBuffer<T>` to the critic's minimal three? Recommendation: include it (§3 — the family-hole argument).
- **Q2 — the generic-source condition.** Confirm declaration-site rejection of type-parameter sources (recommendation: yes, §2's argument; the alternative is a per-instantiation body-check architecture change).
- **Q3 — `IReadOnlySpan<TElement>`.** File the severable stdlib question (declare its `Length`/indexer read surface `pure`) and admit it in a follow-up, or fold into this proposal? Recommendation: file separately — it is a stdlib contract change with its own §4.16 blast radius.
- **Q4 — the bounds-read framing.** Confirm the "intrinsic length fact, both lowerings admissible" wording (§4), or pin one lowering normatively? Recommendation: the neutral wording — it matches how the `Range<T>` counted loop is specified.

## 10. Interactions

- **0086 (front-half sibling, per the coordinator's index — zero-step ranges):** both touch loop desugarings; no textual overlap (§16 vs §22.15/§29.2).
- **0081:** no contact (0081's lint B concerns terminal accessors over lazy pipelines; pure bodies cannot call the lazy operators at all — rule 6).
- **0090:** no contact (no consume positions are involved; iteration variables here are borrows/copies).
- **Contracts (§22.16):** clause expressions are checked "under the pure-body discipline … with the amendments of this section" — the widening flows into `requires`/`ensures`/`invariant` expressions automatically, letting a clause loop a concrete array without the index form; noted so the graduation entry states it deliberately rather than by accident.
- **`pure` iterator future (§22.15's open door):** untouched; this proposal narrows the *demand* for it (the dominant loop shapes stop needing enumerators in pure bodies) — recorded as context, not as prejudice.
