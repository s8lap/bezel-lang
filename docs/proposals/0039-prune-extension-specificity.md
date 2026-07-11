# Proposal 0039 (RFC) — One specificity rule for extensions: delete §26.5.1 rules 2 and 3

| | |
|---|---|
| **Status** | draft (RFC) — authored + critique-verified 2026-07-11; owner-gated (nothing lands this run)
| **Tracking issue** | [#1650](https://github.com/s8lap/bezel-lang/issues/1650)
| **Target module(s)** | Spec [§26.5.1](../Bezel_Language_Spec.md#sec-specificity-ordering) (rules 2 and 3 deleted; the v0.183 "What each relation compares" paragraph reduced to its rule-1 half; the worked example replaced), [§26.5](../Bezel_Language_Spec.md#sec-resolution) (step 3's pointer text; step 4's remedies gain the distinct-name remedy), Appendix A (none — no grammar is involved), Appendix C (**none — verified**, §7). Rationale: one decision record (the decline of auto-ranked generic specialization). **No stdlib change** (§5 verifies none is needed). No conformance-level change. |
| **Written against** | spec/stdlib **v0.183**. *(Carried to v0.184: the cited sections are unchanged by the 2026-07-11 ownership-coherence revision; re-verify at landing.)* All citations re-verified against the v0.183 text; the stdlib extension-block inventory in §5 was re-derived from the current document, not inherited from the finding — and independently re-derived a second time by the adversarial critique pass (2026-07-11), which confirmed it complete and ran the killer-counterexample hunt to exhaustion: **zero** stdlib call sites resolve through rules 2–3 today. |
| **Design character** | **RFC** — deletes normative resolution rules, so it is a design change even though no implementation and no program has ever run under the deleted rules. The recommendation is argued; the owner decides. |

## 0. Summary

Extension-member resolution ([§26.5](../Bezel_Language_Spec.md#sec-resolution)) ranks applicable candidates by a three-rule specificity ordering ([§26.5.1](../Bezel_Language_Spec.md#sec-specificity-ordering)) and errors on ties (rule 4). This RFC recommends keeping exactly **one** specificity relation — rule 1, subtype ordering on receiver-substituted heads (`extension Array<T>` beats `extension IEnumerable<T>` for an `Array<int>` receiver) — and **deleting rules 2 and 3**:

> 2. E1 and E2 are both generic instantiations of the same generic type, and E1's type arguments are *more concrete* than E2's. A type argument is more concrete than another when it is a non-parameter type and the other is a generic type parameter, or when both are non-parameter and the rule applies recursively to their type arguments.
> 3. E1 has more constraints in its `where` clause than E2, and every constraint of E2 is implied by E1's constraints.

Every collision the deleted rules would have ordered becomes the **existing** rule-4 ambiguity error, whose remedies are already normative (import surgery, module-level helper — §26.5 rule 4) and gain one more (distinct method names — the posture [§22.6] already canonizes for the analogous owned-overload problem).

Why these two rules specifically: rule 3 requires deciding **constraint implication** — "every constraint of E2 is *implied* by E1's constraints" — an entailment judgment with unstated semantics (is `where T: Dog` implied by `where T: Animal`? does implication chain through `where T: U`? through interface inheritance, so `where T: IEnumerator<X>` implies `where T: IDisposable`?) that appears **nowhere else in the language**; rule 2 requires a recursive concreteness ordering *plus* the v0.183 clarification that rules 1 and 2 compare **different heads** (receiver-substituted vs declared) — a subtlety that exists only to keep the two rules from contradicting each other. And both rules are pure spec debt in the strict sense: the reference compiler has **never implemented either** (#1307, open: "extension resolution is first-declared" — the spec's own worked example returns the wrong member today, silently, by declaration order), and the standard library **never exercises them** (§5: every same-name overlap across stdlib extension blocks is disambiguated by applicability alone). Deleting them converts #1307 from "implement a recursive ordering plus an entailment judgment" into "emit the rule-4 diagnostic" — strictly less compiler work for a strictly simpler language.

What is genuinely lost — the auto-ranked specialization pattern (`extension IEnumerable<int>` refining `extension IEnumerable<T>`, §26.5.1's own example) and rule 3's constrained-fast-path sibling — is priced in §4, with the reversibility asymmetry stated: re-adding rule 2 later is a compatible widening (programs that were errors start resolving); shipping the rules and deleting them later would break programs. Deletion is the cheap-to-reverse direction.

## 1. The mechanism today (v0.183)

§26.5 resolves `x.Foo(args)` in four steps: (1) instance members win by name (v0.183 name-blocking); (2) extension blocks on the receiver's type, bases, and interfaces are searched, block-level type parameters inferred from the receiver; (3) "Extensions on the most specific type are preferred per the ordering below"; (4) "If two equally-specific extensions match, the call is a compile error" with the remedies quoted in §3 below. §26.5.1 then defines *more specific* by the three rules — rule 1 (subtype), rules 2–3 (quoted in §0) — plus the v0.183 paragraph:

> Rule 1's subtype test is evaluated on the **receiver-substituted heads** … Rule 2's more-concrete comparison is evaluated on the **declared, pre-substitution heads** — it exists precisely to order `extension IEnumerable<int>` above `extension IEnumerable<T>`, which are indistinguishable after substitution.

and a worked example: `IEnumerable<T>.First()` vs `IEnumerable<int>.First()`, where an `Array<int>` receiver "selects the IEnumerable<int> overload" by rule 2.

### 1.1 What rules 2–3 cost (the ledger)

1. **Rule 3's entailment judgment.** "Implied by" is undefined. A conforming implementation must decide: syntactic constraint identity? subtype entailment between type bounds? closure over `where T: U` parameter chains? interface-inheritance closure? Each answer is defensible; the spec picks none. This is the **only constraint-entailment reasoning in the language** — constraint *satisfaction* (§14.2) is checked against concrete arguments at instantiation, a different and decidable question. One under-specified judgment, reachable from exactly one rule, used by zero programs.
2. **Rule 2's recursive concreteness ordering** — a second ordering relation (after subtyping) that resolution must implement, with its own recursion into type arguments.
3. **The two-heads split.** Because rule 1 must see substituted heads (else `extension Array<T>` — substituted `Array<int>` — could not be compared to `extension IEnumerable<int>`) while rule 2 must see declared heads (else `IEnumerable<int>` and `IEnumerable<T>` are indistinguishable), v0.183 had to add a clarification paragraph whose entire second half exists for rule 2. A resolution algorithm that evaluates one relation on two different views of its inputs is a standing invitation to the next inconsistency.
4. **The example-maintenance trail.** The §26.5.1 example has already needed repair twice (rationale changelogs: v0.85 fixed its invalid `=> ...;` elision syntax; v0.94 replaced `Sum` with `First` because the shown `Sum(): T` was unimplementable without static interface members). A rule whose only observable life is its example, and whose example keeps needing maintenance, is living on the wrong side of the spec-debt ledger.
5. **#1307, open.** The implementation applies first-declared-wins. The spec's worked example — normative text — returns 100 or 200 *depending on declaration order* today, with no diagnostic. Whatever is decided, the current state (a normative ordering, unimplemented, with silent order-dependence in its place) is the worst of the options.

## 2. Recorded decisions engaged

- **The v0.183 heads clarification** (rationale changelog: "Specificity: rule 1 compares receiver-substituted heads, rule 2 the declared heads (worked example added…)") is the freshest text this RFC deletes half of. It was a **mechanical consistency clarification** made while defining resolution end-to-end — not a considered keep-with-reasons ruling on whether rules 2–3 earn their place. No rationale entry, in §11 or any changelog through v0.183, defends rules 2–3 on the merits. Like 0035/0036/0037's subjects, their presence is a default, not a decision.
- **Rationale §18 (Things deferred), the extension-disambiguation entry (v0.183)**: a first-class call-site disambiguation spelling is deferred because "the collision is rare, loud, and totally remediable today." This recorded posture is *load-bearing for this RFC*: it is the project's own judgment that rule-4 ambiguity errors are an acceptable terminal state for extension collisions. Deleting rules 2–3 routes more (still rare) collisions to that already-blessed terminal state.
- **§22.6's distinct-names posture** (v0.181, withdrawing the v0.77 out-owned carve-out): when two same-name declarations cannot be soundly auto-ranked, Bezel's recorded answer is *distinct names* — "the convention the standard library uses everywhere (`TryGetValue` vs `TryPopValue` …)". §4 applies the same posture to the specialization pattern.
- **#1307** is an implementation-gap issue, not a design record; it asks for rule 2 to be implemented because the spec says so. This RFC proposes resolving it the other way (fix the spec, shrink the bug). Not a re-opened decline — no decline exists.
- **DECLINED.md / rationale decline records**: nothing adjacent.

## 3. The replacement

### 3.1 New §26.5.1 (sketch)

> For receiver type R, specificity ranks the **applicable** candidates — extension members of the called name that match the call's arity and argument types, from blocks whose head matches R (step 2), with block-level `where` clauses satisfied by the inferred arguments. An extension on E1 is **more specific** than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1, the test being evaluated on the **receiver-substituted heads**: each candidate's declared head has its block-level type parameters replaced by the arguments inferred from the receiver before the subtype test runs. (Derived class beats base class; class beats interface implemented by that class; derived interface beats extended interface — for a receiver of type `Array<int>`, `extension Array<T>` — substituted head `Array<int>` — is more specific than `extension IEnumerable<T>` — substituted head `IEnumerable<int>`.)
>
> If neither candidate's substituted head is a proper subtype of the other's, the candidates are equally specific and rule 4 of §26.5 produces an ambiguity error.

Three properties of this text, called out:

(a) it keeps the load-bearing half of the v0.183 paragraph (rule 1 on substituted heads — needed independently of rule 2, per the `Array<T>`-vs-`IEnumerable<int>` walkthrough, which still resolves correctly);

(b) it makes explicit what the current text leaves implicit, that specificity ranks **applicable** candidates only — and this reading is not a free choice, it is the only reading the stdlib has ever been consistent with. The decisive evidence is §5's own inventory: under the alternative rank-blocks-then-resolve reading, `xs.Sum(selector)` on an `Array<int>` would commit to the per-type `extension IEnumerable<int>` block (most specific under today's rule 2), find no `Sum(selector)` member there, and **fail** — the stdlib's pervasive arity-split pattern (no-arg members in the specific/constrained blocks, selector members in the generic block) is unresolvable under blocks-first ranking. Two consequences are stated so they are visibly chosen rather than smuggled: the new text changes the *unit* of ranking from **block** to **applicable member**; and for cross-head, different-signature same-name pairs this is a **semantic choice** — a less-specific block's applicable member now beats a more-specific block's inapplicable one, where a blocks-first reading would arguably error. The choice is the one that keeps every current stdlib call resolving, and it matches the overload-resolution reading the v0.183 end-to-end definition already implies;

(c) it deletes the declared-heads view entirely — one relation, one view.

### 3.2 The worked example, retargeted

The current example (the `First()` pair) is a rule-2 pair and becomes the *ambiguity* illustration. Proposed replacement example: `extension Array<T>` vs `extension IEnumerable<T>` declaring the same member for an `Array<int>` receiver — resolves by rule 1 to the array block. Followed by the negative example: `extension IEnumerable<T>` vs `extension IEnumerable<int>` declaring `First()` — equally specific (identical substituted heads for an `Array<int>` receiver), ambiguity error when both are visible — with the remedies inline (below).

### 3.3 The ambiguity diagnostic (new normative requirements on the rule-4 error)

Because extension blocks are nameless, the error must identify candidates by what they *have*: for each, the **defining module**, the **declared head including its `where` clause**, and the **declaration site**. The message lists the remedies in the recorded order: (1) remove or narrow a conflicting import (§26.5 rule 4, existing); (2) hoist into a module-level helper (existing, v0.183); (3) **if both blocks are yours: give the members distinct names** (new text, §22.6's posture applied here — the specialized member becomes `FirstFast()` or the block merges into one member with an internal dispatch). Sketch:

```
error: call `xs.First()` is ambiguous between equally-specific extensions
  candidate: extension IEnumerable<T> (module Seq.Core, seq/core.bz:12)
  candidate: extension IEnumerable<int> (module Seq.Fast, seq/fast.bz:8)
  note: extensions are ranked only by receiver-subtype specificity (§26.5.1);
        same-head blocks do not auto-rank
  help: narrow one import, call a module-level helper, or (if you control a
        block) rename one member
```

### 3.4 #1307 re-routed

The issue's fix becomes: implement rule 1 over applicable candidates + the tie ambiguity error (replacing today's silent first-declared-wins). That is strictly smaller than what #1307 currently asks (rule 2's recursive ordering, and eventually rule 3's entailment), and it removes a silent wrong-answer state rather than refining it: under first-declared-wins, the winning member **changes when imports or declaration order change** with no diagnostic — exactly the class of action-at-a-distance §26.5's v0.183 name-blocking rule was written to prevent on the instance side. And because §5's enumeration establishes that **zero** stdlib calls reach a same-head specificity comparison today, rule-1-plus-ambiguity reproduces the current green suite's resolution *exactly* — the re-routed fix carries **zero behavioral risk for any existing passing test**, converting "strictly less compiler work" from plausible to proven.

## 4. What is lost, and the reversibility asymmetry

**Lost (rule 2): auto-ranked specialization.** A library shipping `extension IEnumerable<T>.First()` and an optimized `extension IEnumerable<int>.First()` — §26.5.1's own example — today (per spec, not per implementation) has the int form selected automatically. After deletion, a client importing both gets the ambiguity error. Remedies, in the order a library author would take them: keep **one** block and dispatch internally (a type test on the receiver — ordinary code); or export the specialization under a **distinct name** (§22.6's recorded posture); or split the blocks across modules so clients import one. The pattern is speculative — no stdlib block, no proposal, and no test exercises it (§5) — and the project's recorded judgment on extension collisions is that they are "rare, loud, and totally remediable" (rationale §18).

**Lost (rule 3): the constrained fast path**, priced with the same honesty: a general block plus a *constrained same-head* block declaring the **same signature** — `extension IEnumerable<T>` and `extension IEnumerable<T> where T: IComparable<T>`, both declaring `Foo()` — today (per spec) resolves to the constrained block when the constraint is satisfied. After deletion, when the constraint holds, both candidates are applicable with identical substituted heads and the call is the ambiguity error. The remedies are exactly rule 2's three. This shape exists in the stdlib's block structure (§5's L2717-vs-L2823 pair) but **never with a shared member name and arity** — the stdlib's discipline already avoids it.

**The asymmetry.** If evidence ever demands auto-ranked specialization, **re-adding rule 2 alone** (without rule 3) is a *compatible widening*: rules apply in order, so rule 2 can only order pairs that rule 1 left tied — every program that resolves under rule-1-only still resolves to the same member; rule 2 only converts (some) former ambiguity errors into resolutions. The declared-heads machinery would return with it, but as a considered decision paying for demonstrated demand. The reverse path — keeping the rules now, implementing them (#1307), letting programs depend on the ordering, then discovering rule 3's entailment semantics need to be pinned down — ends somewhere expensive: any later change to entailment semantics silently re-routes calls. One direction is cheap to reverse; the other compounds. This RFC takes the cheap direction. (The re-addition condition — Q4 — deliberately covers **rule 2 only**: rule 3 would return with its unspecified entailment judgment, which is the single worst item on the ledger, and no re-addition condition is offered for it.)

**Not lost:** constrained extension blocks themselves (`extension IEnumerable<T> where T: IComparable<T>` stays fully legal — constraints remain an *applicability* filter per §3.1; only their use as a *ranking* signal goes); rule 1's entire behavior; every §26.5 step.

## 5. Migration — the honest count

Who resolves through rules 2–3 today, verified against the v0.183 documents and the current test corpus (and independently re-derived by the critique pass — the inventory below is **complete** over the stdlib document, not sampled):

- **Stdlib: zero call shapes.** The complete inventory of stdlib extension blocks whose heads can match one receiver: §5's `public extension IEnumerable<T>` (L2717) vs §5.1's `public extension IEnumerable<T> where T: IComparable<T>` (L2823) vs §5.1's per-type `public extension IEnumerable<int/long/double/decimal>` (L2834–2849; uint/ulong/float documented as analogous); §4.4's `public extension Array<T> where T: IComparable<T>` (L1285); §4.5's `public extension OwningArray<T> where T: class, IComparable<T>` (L1339); the byte-accessor blocks on distinct concrete heads — `ReadOnlySpan<byte>`, `Span<byte>` (§4.14, L2359/2403), with the v0.165 #1358 mirror onto `ArraySpan<byte>`/`ReadOnlyArraySpan<byte>` (§4.15, by prose). The stdlib document does repeat the `ReadOnlySpan<byte>`/`Span<byte>` heads once each (the normative surface at L2359/2403 and an illustrative implementation snippet at L2453/2465) — same-module, same-head repetition, which is [§26.6] **merge** territory and never reaches a specificity comparison. §5's `IOrderedEnumerable<T>` block (L2803) shares no member names with overlapping heads. Every same-name pair across overlapping blocks differs in **arity**: `Sum()`/`Average()` (per-type blocks) vs `Sum(selector)`/`Average(selector)` (generic block); `Min()`/`Max()` (IComparable block) vs `Min<U>(selector)`/`Max<U>(selector)` (generic block). Under §3.1's applicable-candidates rule, at most one candidate survives per call — no stdlib call ever reaches the specificity comparison for a same-head pair. (This also holds for the `Sum`/`Average`→`INumber<T>` promotion that stdlib §5's own prose anticipates — the per-type overloads "predate the `INumber<T>` abstraction (§2.19) that now provides them". **No issue currently tracks that promotion** — the §11.8 recorded-follow-ups family (#1506 `FromCollectionLiteral`, #1507 `IDeserializable.Deserialize`, #1508 `Range<T>`/`ISteppable<T>`) covers three *other* static-abstraction promotions and not this one; the landing agent may file it alongside this RFC's tracking issue if wanted. If it is ever taken up, replacing the per-type no-arg blocks with one `where T: INumber<T>` block preserves the no-arg/selector arity split, so the promotion needs no rules 2–3 either.)
- **Accepted proposals: zero.** 0026's additions (indexed Select/Where, MinBy family, Chunk) go into the *same-module, same-head* `IEnumerable<T>` block — §26.6 merge territory, never a specificity comparison (indexed vs non-indexed Select is intra-set overload resolution by lambda arity, as 0026 itself says); 0028 puts Shuffle on `Random` (instance); 0032/0023/0027 declare no overlapping blocks. Guides: zero extension mentions.
- **Spec: one worked example** — retargeted per §3.2.
- **Conformance suite: one case**, `spec_0167` (manifest row 169, `2651-Specificity-ordering`) — already `skip` in the manifest (wrapping limitation, #683); it is replaced by cases for the new text rather than migrated (a hand edit, per #1380).
- **Tests: zero.** No test in `tests/` exercises rule 2 or rule 3 ordering (verified; `extension_resolution_test.cpp` covers steps 1–2 and name-blocking; within the #1346 family — #1346 being the §26.3 extension-underspecification issue, whose tests are mostly visibility/property-form cases — the one concrete-arg case, `extension_resolution_test.cpp:319`, tests *registration* of `extension IEnumerable<int>`, not its ranking against a generic block; `generic_extension_698_test.cpp`'s spec_0167-derived case only checks that the generic block's `T` resolves; `extension_nested_generic_target_1599_test.cpp` tests head matching, not ranking). New spec-derived tests: the rule-1 pair resolves; the same-head pair errors with both candidates named; a constrained-vs-unconstrained same-head pair errors (the shape rule 3 would have ordered).
- **User code: none exists** (pre-users project). And programs *hypothetically* relying on rules 2–3 were never served by the implementation anyway — they got declaration-order roulette (#1307). There is no behavior anyone has ever observed that this deletion takes away.

## 6. Edit list

- **Spec §26.5.1**: delete rules 2 and 3; replace the rules paragraph and the v0.183 comparison paragraph with §3.1's text; replace the example per §3.2.
- **Spec §26.5**: step 3's sentence retargets ("per the subtype ordering below"); rule 4 gains the distinct-names remedy sentence and the candidate-identification requirements of §3.3.
- **Rationale**: one decision record — auto-ranked generic specialization (rule 2) and constraint-implication ordering (rule 3) **declined**, with: the never-implemented/never-exercised evidence, the §22.6 distinct-names posture as the remedy, the §18 rare-loud-remediable posture as the terminal-state justification, and the §4 re-addition condition (rule 2 alone, on demonstrated demand, as a compatible widening; none for rule 3).
- **Appendix C**: no edit (§7).
- **#1307**: retitle/re-scope to "implement §26.5.1 rule-1 ranking + rule-4 ambiguity diagnostic (replacing first-declared-wins)"; note this RFC's tracking issue as the authority.
- **Conformance**: replace `spec_0167` with cases for the new text (hand edit, per #1380).

## 7. Appendix C — verified, no edit

Appendix C (Compiler-Enforced Invariants, C.1–C.12) was checked line-by-line for this RFC (and re-checked by the critique pass): **it contains no extension-resolution or specificity invariant** — nothing to delete or reword. Deliberately, no new row is proposed either: C's rows are program-shape invariants ("X appears only in Y"), not resolution-algorithm restatements, and the ambiguity error is already normative in §26.5 rule 4. Recorded here so a landing editor does not go hunting.

## 8. Sizing

| Piece | Size |
|---|---|
| Spec edit | Small — one subsection rewritten (shorter), one rule's remedies extended, one example replaced |
| Rationale edit | Small — one decision record |
| Stdlib edit | None (verified — §5) |
| Compiler | Net reduction of *obligation*: #1307's task shrinks from two ordering relations + an entailment judgment to subtype ranking + a tie diagnostic. New code is still owed (today's first-declared-wins matches neither the old nor the new spec) — but with zero behavioral risk to the existing green suite (§3.4) |
| Tests | New spec-derived cases (§5); nothing migrates |
| Migration risk | None identified — no document, test, or implementation behavior depends on the deleted rules |

## 9. Owner questions

1. **Q1 — the ruling**: delete rules 2 and 3? Recommended: **yes**. The alternative — keep-and-specify — requires designing rule 3's entailment semantics (a real piece of type-system design) and implementing rule 2's ordering, all in service of a pattern with zero recorded demand.
2. **Q2 — asymmetric alternative**: delete rule 3 only, keep rule 2? Priced: this keeps the declared-heads split and the recursive concreteness ordering (ledger items 2–3) and keeps #1307's current scope, to preserve a pattern nothing uses. Not recommended — but it is the natural fallback if the owner weighs the `IEnumerable<int>`-specialization story more heavily than this RFC does, and it still deletes the entailment judgment (the worst item).
3. **Q3 — diagnostic requirements** (§3.3): make the candidate-identification content normative, or leave it as quality-of-implementation? Recommended: normative — nameless blocks make an under-specified ambiguity error genuinely hard to act on.
4. **Q4 — record the re-addition condition** (§4) in the rationale record? Recommended: yes; it is what makes this a cheap-to-reverse decision rather than a door slammed. The condition covers rule 2 only, deliberately (§4).

## Recommendation

**Adopt** — delete §26.5.1 rules 2 and 3, keep rule 1 on receiver-substituted heads with the applicable-candidates clarification, extend rule 4's remedies and diagnostic requirements, re-route #1307, and record the decline of auto-ranked specialization with its re-addition condition.

**Confidence: high** on the deletion being safe (never implemented, never exercised, zero-migration — all mechanically verified, and the critique pass's exhaustive enumeration of stdlib call sites found **zero** that resolve via rules 2–3) and on rule 3 specifically (an unspecified entailment judgment used nowhere is debt by any measure). **Medium-high** on deleting rule 2 rather than keeping it: rule 2 is at least well-defined, and the specialization pattern it enables is a real (if unexercised) library technique — Q2 exists because reasonable owners could keep it. The RFC recommends deleting both because rule 2's real cost is the two-heads split it drags into rule 1's semantics, and because re-adding it later is compatible while un-shipping it later is not.

---

## Finalization record (2026-07-11, phase 3)

Adversarial critique verdict: **REVISE (minor)** (1 blocking, 5 advisory). Disposition:

- **B1 (blocking, fixed)** — the §5 parenthetical claimed a "tracked `Sum`/`Average`→`INumber<T>` promotion, the #1506–#1508 family's fourth member". **No such tracked item exists**: spec §11.8's recorded follow-ups name exactly three promotions (#1506/#1507/#1508), none of them this one. The parenthetical now cites what actually exists — stdlib §5's prose noting the per-type overloads "predate the `INumber<T>` abstraction" — states plainly that no issue tracks the promotion, and suggests the landing agent may file one. The substantive point (the promotion, if taken, needs no rules 2–3) was verified correct and stands.
- **A1 (fixed)** — §3.1(b) now *shows* the applicability-first evidence (the `Sum(selector)`-on-`Array<int>` failure under blocks-first ranking) instead of asserting it, and explicitly states the two consequences as chosen: the ranking unit changes from block to applicable member, and cross-head different-signature pairs resolve rather than arguably erroring.
- **A2 (fixed)** — §5's byte-accessor sentence no longer claims "never two on one receiver"; it now states the heads are distinct concrete heads and that the document's same-head repetition (normative surface + illustrative snippet) is §26.6 merge territory, never specificity.
- **A3 (fixed)** — §4 now prices rule 3's lost pattern (constrained same-head fast path) explicitly, with the same remedies, and states that Q4's re-addition condition deliberately covers rule 2 only.
- **A4 (fixed)** — the #1346 description is tightened: #1346 is the §26.3 underspecification issue; the concrete-arg registration case is one case within its family.
- **A5 (fixed)** — §3.4 and §8 now carry the enumeration result as proof: rule-1-plus-ambiguity reproduces today's stdlib resolution exactly, so the re-routed #1307 fix has zero behavioral risk for the existing green suite.

No substantive position changed; the recommendation and both confidence levels are as drafted.
