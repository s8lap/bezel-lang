# Proposal 0156 — A retyping of `IReadOnlySpan<TElement>` is a borrow-erasing conversion

| | |
|---|---|
| **Status** | graduated-v0.333 — superseded — graduated v0.333 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": options C and G2 — every identity-preserving retyping that would carry an `IReadOnlySpan<TElement>` value to `Object` or another interface type (§12.9's variance at a type argument, §27.1.1's return and parameter slots, a method reference's formation) is spec §9.7's borrow-erasing conversion, a compile error where it is written, and in a generic body such a retyping over a type parameter is checked under the class-or-interface assumption with its conditions read at each instantiation; critique round 1 applied, 3 findings resolved, one of them a soundness face, shared with the #5727 editorial fix; that round made a run-time type test fail through such a variance (`o as IReadOnlyList<Object>` over an `Object` holding a list of spans is `null`), read a variant conversion's source at its instantiation of the target's interface (an `Array<IReadOnlySpan<int>>` source, an implementing class), and listed a dispatched generic method's variance over its own type parameter among the newly rejected; landed with the v0.333 changelog entry, beside the composition check's pointer from §9.7's interface paragraph to the retyping rule) |
| **Tracking issue** | [#5726](https://github.com/s8lap/bezel-lang/issues/5726) (`spec-defect`, `soundness`; the borrow-flow matrix's third run, cell R6/C4, with the R8/C4 observation); one `spec-tracking` compiler tracker is filed at landing (§6) |
| **Target module(s)** | **spec**: §9.7 (the borrow-erasing bullet's conversion list), §8.6 (the *Boxing of borrow-bearing records* conversion list), §12.9 (two new paragraphs), §27.1.1 (one sentence in each of the parameter-slot and return-slot bullets), §27.6 (one clause in the formation parenthetical), Appendix C.1 (two rows). **stdlib**: §4.16 (one clause). **rationale**: §4.2 (one dated record). **code-generation reference**: §7.3 (one *Looks right, is rejected* pair). **`DECLINED.md`**: five `declined-at-proposal` rows |
| **Written against** | spec/stdlib **v0.332** at `6a3f45f` (main). Line numbers below are at that commit. |
| **Origin** | The borrow-flow matrix's third run against v0.332 (`scratch/matrix/r3/`, lane M2): cell R6/C4 moved from `decided` to `conflict` on a route that predates v0.332 and was not seen before; cell R8/C4 recorded the generic-body face as an observation. This proposal is needed because it removes a stated admission: §27.1.1's "The **return slot**, when of class or interface type, is **covariant**" and §12.9's "Reference-typed T admits the full covariant chain" both admit an `IReadOnlySpan<TElement>` at those positions today. |

*Reference convention: unprefixed §-references are language-spec references at `6a3f45f` (v0.332); "stdlib §" references are to the standard library companion; K tags are §28.3's kernel ids. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares (`IReadOnlySpan<TElement>`, stdlib §4.16; `IReadOnlyList<out T>`, stdlib §4.1; `Array<T>.AsSpan()`, stdlib §4.15); none invents an API.*

---

## 0. Thesis

Spec §9.7 says a value that holds borrows "reaches no other reference-typed position" than `IReadOnlySpan<TElement>` and a member-call receiver, and §8.6 says it is converted to `Object` or an interface "at exactly **two positions**". Both rules are stated as rules about a **conversion**: the borrow-erasing bullet fires "at the conversion site". `IReadOnlySpan<TElement>` is an interface, though, and the language has three ways to retype a value of an interface type **without converting any value**: §27.1.1's variance of function-type slots, §12.9's variance of interface type arguments, and a method reference's formation adapter (§27.6). Each carries an `IReadOnlySpan<TElement>` value to `Object` or another interface while no conversion of a span value is written, so the borrow-erasing bullet has no site.

This proposal treats every such retyping as the conversion it performs: a compile error where the retyping is written, under the same bullet, with the same diagnostic and remedy. It adds no mechanism and changes no admitted program in `tests/`, the documents or the rule corpus (§2). It also decides the generic-body face the matrix recorded at R8/C4: a variant retyping over a type parameter is checked under the class-or-interface assumption at the declaration, and its conditions are read at each instantiation (§3.6 rule 2), so an instantiation at `IReadOnlySpan<…>` or at a value type is rejected.

---

## 1. Ground truth

### 1.1 The routes

1. **Return-slot covariance (§27.1.1, line 5928).** "The **return slot**, when of class or interface type, is **covariant**: the value's return type must implicitly reference-convert to the target's". `IReadOnlySpan<int>` reference-converts to `Object` (an interface type to `Object`). §27.1.1 excludes the function-typed twin in the next sentence ("A return slot whose type in the value is a function type is not such a slot, whatever the target's") but not this one.

   ```bezel
   class Keeper
     private _o: Object?;

     public method Keep(s: IReadOnlySpan<int>)
       var id: (IReadOnlySpan<int>) -> IReadOnlySpan<int> := lambda(x) => x;
       var up: (IReadOnlySpan<int>) -> Object := id;   // §27.1.1: covariant interface return slot
       _o := up(s);   // announcement table row 4: an Object borrow sourced at s; the field-store residual admits it
     end Keep;
   end Keeper;

   var xs: Array<int> := [1, 2, 3];
   var k := new Keeper();
   k.Keep(xs.AsSpan());
   xs.Dispose();   // k._o still boxes a span over xs
   ```

   The lambda's own `return x` is not a conversion: its return type is the target's `IReadOnlySpan<int>`. The assignment `up := id` converts a function value, not a span. `up(s)` passes `s` at a position declared `IReadOnlySpan<int>`, which is admitted. The invocation's result is the announcement table's row 4 ("sourced from the invoked value and from the invocation's plain arguments"), an `Object` borrow sourced at `s`, a plain parameter, which the field-store bullet's residual admits.

2. **Parameter-slot contravariance (§27.1.1, line 5927).** "the target's parameter type must implicitly *reference*-convert to the value's". `IReadOnlySpan<int>` converts to `Object`, so a `(Object) -> ()` value is usable where `(IReadOnlySpan<int>) -> ()` is expected. The value's body receives the span at a parameter declared `Object` and may store it. The matrix did not name this face. It is the same gap from the other side, and §27.1.1 closes its function-typed twin in the same bullet ("A slot whose type in the target is a function type is not such a slot").

   ```bezel
   class Sink
     private _o: Object?;
     public method Put(o: Object)
       _o := o;
     end Put;
     public method Each(s: IReadOnlySpan<int>, f: (IReadOnlySpan<int>) -> ())
       f(s);
     end Each;
   end Sink;

   // var g: (Object) -> () := sink.Put;   // a formed value at (Object) -> ()
   // var h: (IReadOnlySpan<int>) -> () := g;   // §27.1.1: contravariant parameter slot
   // h(xs.AsSpan());   // the span reaches Put's Object parameter and is stored
   ```

3. **Interface variance (§12.9, line 2210).** "Variance applies only when the type argument is a reference type." `IReadOnlySpan<int>` is one, so `IReadOnlyList<IReadOnlySpan<int>>` converts to `IReadOnlyList<Object>` (stdlib §4.1 declares `IReadOnlyList<out T>`), and an element read from it is an `Object`: `var v: IReadOnlyList<Object> := xs; _o := v[0];`. The contravariant face exists at a user interface (`interface ISink<in T> method Put(x: T); end ISink;`): `ISink<Object>` converts to `ISink<IReadOnlySpan<int>>`, and `sink.Put(span)` passes the span to an implementation whose parameter is declared `Object`. The standard library declares no contravariant interface. `IEvent<out T>` (stdlib §6.5) is a covariant one whose `T` sits inside a handler's parameter, so `IEvent<IReadOnlySpan<int>>` to `IEvent<Object>` admits an `(Object) -> ()` handler for span payloads.

4. **Method-reference formation (§27.6, line 6069).** "Formation may bridge representation-changing conversions by synthesizing an adapter at the reference site". `var keep: (IReadOnlySpan<int>) -> () := k.Keep;` over `Keep(o: Object)` bridges the span-to-`Object` reference conversion inside a synthesized adapter. No written expression converts a span. The function-typed twin has its own rule (§28.14's *method-reference formation bar*). The span has none.

5. **A run-time type test from `Object` (§12.7).** Converting the constructed type as a whole stays admitted (`var o: Object := xs;` over `xs: IReadOnlyList<IReadOnlySpan<int>>` converts the list reference), and §12.7's `is` answers by conformance "with §12.9 variance". So `var v := o as IReadOnlyList<Object>;` succeeds at run time, and `_o := v[0]` stores a span at `Object` with no static retyping written: the operand's static type is `Object`, which carries no argument a conversion check could judge. The same holds for a downcast from any interface the runtime type reaches (`e as IReadOnlyList<Object>` over `e: IEnumerable<IReadOnlySpan<int>>`, a cross-cast, not a §12.9 conversion). A conversion error cannot close this face; the run-time relation must not succeed through the erasing variance.

### 1.2 What the text already says

- §9.7, the `IReadOnlySpan<TElement>` paragraph (line 1450): "It is the one reference type so read …: a value that holds borrows reaches no other reference-typed position (the borrow-erasing bullet below; [§8.6])."
- §9.7, the borrow-erasing bullet (line 1455): "The conversion — §8.6's box, or the reference conversion of an `IReadOnlySpan<TElement>` value to `Object` or another interface type, implicit or by an `as` … or a typed-pattern binding … at such a type — is a **compile error at the conversion site**".
- §8.6 (line 1203): "a value that holds borrows … is converted to `Object` or to an interface type at exactly **two positions**".
- §9.7, the instance-field bullet: "A value type that holds borrows is **stored past its bound at no position**", and stdlib §4.16: a span is "never storable in a static field or in a class instance field".

The routes contradict these statements, so the documents disagree. §27.1.1 and §12.9 state an admission and §9.7/§8.6 state a prohibition, and nothing says which wins. Since 0153 (v0.332) the escape is backstopped at run time: a read through the boxed span is a member access on its backing object and faults in checked mode (§28.2). The static posture is still contradicted. That posture is what keeps a span out of every field.

### 1.3 The generic-body face (R8/C4)

§12.9 does not say whether a generic body may variant-convert `IReadOnlyList<T>` to `IReadOnlyList<Object>` over a type parameter. Two readings are defensible. Under §3.6 rule 2's first sentence ("A generic body type-checks under the weakest assumption its constraints establish"), an unconstrained `T` is not known to be a reference type, so the conversion would be rejected at the declaration. Under §28.1 clause 3 ("The generic body type-checks under the class-or-interface assumption"), which §9.7's generic paragraph and §28.3's *Set-erasing positions* both cite for a bare `T`'s body uses, it is admitted at the declaration. Neither reading says what happens at an instantiation substituting `IReadOnlySpan<int>`, which is the span route of §1.1 item 3. The same instantiation at a value type that holds borrows (`T = Pair`) reaches the box with no conversion written, which reopens R3–R5/C4 through R8. The `where T: class` case is not ambiguous: `class` admits interfaces (§14.2), so `T = IReadOnlySpan<int>` satisfies it and the conversion over `T` is admitted at the declaration under either reading.

### 1.4 The compiler (`6a3f45f`)

- **Function-type slots.** `functionSlotConvertible` (`src/sema/ExprTypecheck.cpp:4409–4424`) applies variance only to a **non-generic** class or interface slot whose two sides are both `Named` (`genericArgs.empty()`). Every other slot is compared structurally. `IReadOnlySpan<int>` is generic and `Object` is a primitive `TypeRef`, so routes 1 and 2 are already rejected by the compiler, through an under-implementation of §27.1.1 that also rejects `() -> Dog` to `() -> Object`. The diagnostic is the generic "not implicitly convertible", not the borrow-erasing one.
- **Interface variance.** `variantGenericAssignable` / `typeArgCovariantAssignable` (`ExprTypecheck.cpp:9371–9420`, `:9590–9690`) treat `Object` as the universal root and `isValueTypeArg` (`:9179–9196`) does not classify `IReadOnlySpan<…>` as a value type. So `IReadOnlyList<IReadOnlySpan<int>>` to `IReadOnlyList<Object>` is **admitted** (unprobed: no build in this environment, reading only).
- **Generic bodies.** A free type variable "accept[s] any binding without variance restriction" (`ExprTypecheck.cpp:9593–9604`), so `IReadOnlyList<T>` to `IReadOnlyList<Object>` is admitted in the body. No per-instantiation check exists.
- **Formation.** Method-reference formation applies §5.1 per slot (`ExprTypecheck.cpp:6003–6010`, fix #4081). The span-to-`Object` reference conversion is a §5.1 conversion, so route 4 is presumably admitted (unprobed).

---

## 2. Measurement

Searched with `grep` over `tests/` (gtest `.cpp` raw strings, lit `.bzl`, the rule corpus `tests/conformance/rules/cases/` and the doc-example corpus), the ten normative documents, the code-generation reference and the guides:

- `IReadOnlySpan<…>` as a type argument of any generic type (`<IReadOnlySpan<`): **zero** sites outside the changelog. Its only uses as a type argument are the v0.328/v0.331/v0.332 changelog entries for `Keep<IReadOnlySpan<int>>`, `Task<IReadOnlySpan<int>>` and `AsyncEventSource<IReadOnlySpan<int>>`, each already rejected.
- `IReadOnlySpan<…>` in a function type's slot (`(IReadOnlySpan<…>) ->`, `-> IReadOnlySpan<…>`): **zero** sites.
- A method reference formed at a span-typed slot: **zero** sites.
- A variance conversion to `IEnumerable<Object>`, `IReadOnlyList<Object>`, `IReadOnlyCollection<Object>`, `IEnumerator<Object>` or `IEvent<Object>` anywhere: two lit tests (`sema_variance_assign__spec_12_9_ienumerable_object_from_array_{int_rejected,dog_accepted}_1180.bzl`) and rule-corpus case `s12_9_0004`, all at concrete non-span arguments (`Array<int>`, `Array<Dog>`). None is in a generic body. None moves.
- Function types returning or taking `Object` (`) -> Object`, `(Object) ->`): five gtest sites (`formation_fence_function_value_1820_test.cpp:177,219`, `expr_getter_object_boxing_2761_test.cpp:309`, `method_ref_conversion_adapter_5208_test.cpp:274`, `function_value_call_arg_conversion_5221_test.cpp:69`), none at a span slot. None moves.

**Newly rejected: zero measured sites.** Newly admitted: nothing.

---

## 3. Options

- **A — status quo.** The retypings stay admitted, and the escape is backstopped at run time by 0153's flag (a read through the boxed span faults in checked mode). Declined. It leaves §9.7's "reaches no other reference-typed position" and §8.6's "exactly two positions" false, and makes §9.7's instance-field ban (re-grounded at v0.332 on "stored past its bound at no position") avoidable with one assignment. In an unchecked build the stale read is undefined, on a route that every other rejection of a span storage position exists to rule out. The function-typed twin is closed statically in the same bullet.
- **B — a type-relation exclusion (the issue's recommendation).** §27.1.1: a slot whose type is `IReadOnlySpan<TElement>` "is not such a slot". §12.9: an `IReadOnlySpan<TElement>` type argument "does not vary". This mirrors the function-typed exclusion's wording. It reaches the same verdicts as C, but:
  1. It makes the retyping a **missing conversion** while the direct conversion of the same value is a **present conversion that is rejected**. At an overloaded callee, a candidate needing the retyping becomes inapplicable and another candidate may be chosen silently. A candidate needing the direct conversion is chosen and rejected with §9.7's remedy (`Console.WriteLine(span)` is the recorded instance).
  2. A type relation cannot depend on a substitution. In a generic body (`where T: class`, `T = IReadOnlySpan<int>`) the exclusion still needs a per-instantiation requirement, which is §9.7's generic paragraph read over a conversion. So B needs C's reading anyway.
  3. Route 4 (formation) is not a variance question. B needs a third clause at §27.6.

  Declined in favour of C, which states the same verdicts once.
- **C — the retyping is the borrow-erasing conversion (recommended).** §9.7's bullet lists the three retypings beside the box and the reference conversion. §12.9, §27.1.1, §27.6, §8.6 and Appendix C.1 say so where a reader of each section looks. One rule, one diagnostic, one remedy. §9.7's generic paragraph and §14.2's dispatched-method rule apply unchanged because the retyping is now one of the bullet's conversions.
- **D — track the span's sources through the retyped `Object`.** Read the result of invoking a `(…) -> Object` value, or an element read through an `IReadOnlyList<Object>`, as carrying the span's borrows when the value was retyped from a span type. Declined. The declared type at the use site is `(…) -> Object`, which announces nothing (K2.3), and the retyping can happen in another body. This is the flow-tracked `Object` that rationale §4.2 (v0.329) declined for a local ("`Object` locals with tracked sources", `DECLINED.md`) and that the v0.331 `as` record declined for a function value.
- **E — ban `IReadOnlySpan<TElement>` as a type argument.** Declined: it over-rejects. A method-local `Array<IReadOnlySpan<int>>` is admitted by §9.7's collection bullet (cell R6/C3 decided at v0.332), and passing it at `IReadOnlyList<IReadOnlySpan<int>>` is the ordinary way to read it in a callee. The problem is the variance away from the span type, not the type argument.
- **F — retire `IReadOnlySpan<TElement>`'s reference-type status.** Read it as a value type for §12.9 and §27.1.1, so that it is invariant everywhere. Declined. It would contradict stdlib §4.16 ("An interface"), §3.6 rule 1 ("The interface is not a value type") and the boxing model of §8.6. It would also change what `as`, `is` and `=` mean on the type, all to obtain C's verdicts.

**The generic-body face (R8/C4):**

- **G1 — the weakest assumption.** Reject at the declaration any variant retyping over a type parameter whose constraints do not establish a reference type. This is §3.6 rule 2's first sentence read literally. Declined. It contradicts the class-or-interface assumption that §28.1 clause 3, §9.7's generic paragraph and §28.3's *Set-erasing positions* all apply to a bare `T`'s body uses. It would also not close the span face: under `where T: class` the conversion is admitted at the declaration and `T = IReadOnlySpan<int>` still reaches it, so a per-instantiation reading is needed anyway.
- **G2 — the class-or-interface assumption, conditions read per instantiation (recommended).** The body checks the retyping under §28.1's class-or-interface assumption. §12.9's reference-type condition and the span condition are requirements of the body's use of the parameter (§3.6 rule 2: "an interior use whose admissibility is read at the substitution"), checked at each instantiation, and so are §27.1.1's value-typed-slot invariance and its span arms at a slot over a type parameter. A dispatched generic method decides them at its body (§14.2). At a function-type substitution the body is not re-checked: every value of the parameter's type the body holds is capture-free there (§28.3's *Set-erasing positions*, the induction behind provenance clause 5). One reading for a span and a value type, in the pattern every substitution-dependent rule of §9.7 already follows.

---

## 4. Evaluation

| | A (status quo) | B (type relation) | **C (conversion)** | D (tracked `Object`) | E (no span type arg) | F (span as value type) |
|---|---|---|---|---|---|---|
| Soundness (static posture) | span reaches a field, `Object` param, collection of `Object` | closed | closed | closed where tracking reaches; open across bodies | closed | closed |
| Simplicity | — | three clauses (§27.1.1 ×2, §12.9) + a formation clause + a generic requirement | one list in §9.7, cross-references elsewhere | a new provenance class | one ban | a reclassification |
| Consistency | contradicts §9.7, §8.6 | retyping ≠ conversion at overloads | same rule, diagnostic and remedy as the direct conversion; the function twin's exclusions in span form | contradicts rationale §4.2's declines | contradicts R6/C3's admission | contradicts stdlib §4.16, §3.6 rule 1 |
| Newly rejected (measured) | 0 | 0 | **0** | 0 | 0 known, but every collection of spans read at an interface | unmeasured, large |
| Newly admitted | 0 | 0 | 0 | 0 | 0 | 0 |

For the generic face: G1 newly rejects, at the declaration, every variance over an unconstrained type parameter (measured: zero sites in `tests/` and the documents, but the compiler admits the shape today). G2 newly rejects only the instantiations at a value type or `IReadOnlySpan<…>` (zero sites).

---

## 5. Decision (delegated ruling, 2026-10-08)

**C and G2.**

1. **The retyping is the conversion.** §9.7's borrow-erasing bullet lists, beside §8.6's box and the reference conversion: ▲ "a **retyping** that carries an `IReadOnlySpan<TElement>` value there with no value converted — a variant conversion at a type argument ([§12.9]: `IReadOnlyList<IReadOnlySpan<int>>` to `IReadOnlyList<Object>`), a function-type conversion at a return or parameter slot ([§27.1.1], a lambda literal's written parameter type and return-type ascription included …), or a method reference whose formation bridges the conversion at such a slot ([§27.6] …), nullable or not, composed positions included". The directions are exact: a covariant type argument or return slot that is the span type in the source and not in the target, and a contravariant type argument or parameter slot that is the span type in the target and not in the source. Every other combination is either not a conversion at all (`Object` to `IReadOnlySpan<int>` covariantly) or is not erasing (a class implementing `IReadOnlySpan<int>` varying *to* the span type, which only adds tracking). Converting the constructed type as a whole is unaffected: `IReadOnlyList<IReadOnlySpan<int>>` to `Object` converts a reference to the list, which keeps its own source (cell R2/C4).
2. **Cross-references.** §27.1.1 gains one sentence in each slot bullet, beside the function-typed exclusion it mirrors. §12.9 gains a paragraph stating the span condition, and §27.6's formation parenthetical gains one clause. §8.6's list and the two Appendix C.1 rows name the retyping. Stdlib §4.16 names it beside the direct conversion. A reader of any one of these sections meets the rule there.
2a. **The run-time type test.** §12.9's span paragraph and §9.7's bullet state that a run-time type test (`is`, `as`, a typed pattern) does not succeed through such a retyping: a value is not an instance of a type it reaches only by varying an `IReadOnlySpan<TElement>` argument in the erasing direction, so `o as IReadOnlyList<Object>` over an `Object` holding an `Array<IReadOnlySpan<int>>` is `null`. This is the one place where C, like B, needs the relation itself (at run time) to exclude the variance; the static face stays a rejected conversion for the overload reason of §3 B.1. The §12.9 paragraph also says the source is read at its instantiation of the target's interface, so `Array<IReadOnlySpan<int>>` to `IReadOnlyList<Object>` and a class implementing `ISink<Object>` to `ISink<IReadOnlySpan<int>>` are caught as written.
3. **The generic body (G2).** §12.9 gains a paragraph: a variant conversion at a type argument that is or is formed over a type parameter is checked at the declaration under §28.1's class-or-interface assumption, and its conditions are requirements of the body's use of the parameter read at each instantiation. At an instantiation substituting a value type or `IReadOnlySpan<TElement>` there, it is rejected. It stands at every other substitution, and a function-type substitution is not re-checked. §27.1.1's slot conditions are read the same way, and a dispatched generic method decides the requirement at its body. This decides the value-type case §12.9 left unstated, in the reading the compiler's free-variable admission already takes at the declaration.
4. **Why a design change and not a ruling.** C narrows two stated admissions (§27.1.1's covariant interface return slot and contravariant interface parameter slot, §12.9's "full covariant chain" for a reference-typed argument) for one type. Under spec-change-process §2 that is a design change, and the delegation accepts it. The texts it narrows disagreed with §9.7 and §8.6 at the same revision, so the change makes the documents agree rather than moving a settled verdict.

**Kernel classification (process §9).** A derivation, not a new axiom: K2.3 (a declared type announces the borrows its values may carry, and `Object` announces none) and K2.4, applied at three more conversion sites. These are the same derivations the borrow-erasing bullet and its set-erasing twin already cite. **Body-locality (proposal 0105 §1):** *local* at a concrete retyping; *carve-out* (generic instantiation, the §3.6 rule 2 clause) for the generic face, the classification §9.7's generic paragraph already has.

**Declined and deferred (rows for `DECLINED.md`, in `V1.json`):**

| Topic | Verdict | Reason | Reopening bar |
|---|---|---|---|
| The status quo: the retypings of an `IReadOnlySpan<TElement>` value admitted, the escape backstopped at run time by 0153's flag (option A) | `declined-at-proposal` | contradicts §9.7 and §8.6, which the instance-field ban rests on; undefined in an unchecked build; the function-typed twin is closed statically | —(none stated) |
| A type-relation exclusion, the span slot "not such a slot" and the span type argument "does not vary" (option B, #5726's recommendation) | `declined-at-proposal` | the same verdicts with a missing conversion where the direct one is a rejected conversion (overload resolution diverges), plus a separate formation clause and a per-instantiation reading C already has | —(none stated) |
| Tracking the span's sources through the `Object` a retyping produces (option D) | `declined-at-proposal` | the declared type announces nothing (K2.3); the retyping can be in another body; the flow-tracked `Object` declined at v0.329 and v0.331 | —(none stated) |
| Banning `IReadOnlySpan<TElement>` as a type argument (option E) | `declined-at-proposal` | over-rejects the admitted method-local collection of spans read through an interface; the hazard is the variance, not the argument | —(none stated) |
| Rejecting at the declaration a variant conversion over a type parameter whose constraints do not establish a reference type (option G1) | `declined-at-proposal` | contradicts the class-or-interface assumption §28.1 clause 3, §9.7 and §28.3 apply to a bare `T`'s body uses, and leaves `where T: class` at `T = IReadOnlySpan<…>` open | —(none stated) |

Option F is not recorded as a row: it is a reclassification nobody has proposed, listed for completeness.

---

## 6. Edits at graduation

The exact `old → new` pairs are `scratch/v0333/V1.json` (ids `V1-5726-*`, `V1-5727-*`). Summary:

1. **§9.7, borrow-erasing bullet:** the conversion list gains the retyping (the ▲ text of §5 item 1).
1a. **§9.7, borrow-erasing bullet:** one sentence after "a generic body is not re-checked.": a run-time type test does not succeed through such a retyping (item 2a of §5).
2. **§8.6, *Boxing of borrow-bearing records*:** the "Every other conversion" list gains "by a variance or function-type retyping, or a method reference's formation, that carries an `IReadOnlySpan<TElement>` value there with no value converted".
3. **§12.9:** two new paragraphs after "…compose without conversion.": the span condition (bold lead *`IReadOnlySpan<TElement>` does not vary to a type that is not read as holding borrows*) and the generic-body reading (bold lead *In a generic body*). The sentences rule-corpus case `s12_9_0004` quotes are not edited.
4. **§27.1.1:** one sentence after each function-typed exclusion: a parameter slot that is the span type in the target and not in the value, and a return slot that is the span type in the value and not in the target, are §9.7's borrow-erasing conversion.
5. **§27.6:** the formation parenthetical gains a clause: bridging the span's reference conversion to `Object` or another interface at a slot is the borrow-erasing conversion, a compile error at the formation.
6. **Appendix C.1:** the variance row gains the generic-body sentence. The borrow-erasing row gains the retyping sentence.
7. **Stdlib §4.16:** the *Boxing and lifetime* paragraph's conversion sentence names the retyping.
8. **Rationale §4.2:** one dated record after the v0.331 `as` record.
9. **Code-generation reference §7.3:** one *Looks right, is rejected* pair after the borrow-erasing pair.
10. **`DECLINED.md`:** five rows (above). The index-size line is the coordinator's to recompute over the train.
11. **`README.md`:** the 0156 row in the live queue, `accepted`.

The same JSON carries #5727's editorial fix to §28.3's *Set-erasing positions* (the field sentence names the `Object` substitution beside the function-type one). It is not part of this proposal.

**Spec-ahead-of-impl (one `spec-tracking` tracker):**
- Interface variance: reject the retyping when a covariant argument is `IReadOnlySpan<…>` in the source and not in the target, or a contravariant one is the span type in the target and not in the source, at any depth, with §9.7's borrow-erasing diagnostic (`ExprTypecheck.cpp:9590–9690`).
- Generic bodies: the free-type-variable skip (`:9593–9604`) needs a per-instantiation check at a value-type or `IReadOnlySpan<…>` substitution.
- Method-reference formation: reject the span-to-`Object` or span-to-interface bridge (`:6003–6010`).
- Run-time type tests: the variant-interface instance test (`typeTestInterfaceBindAmbiguous` and the runtime descriptor check, #1843/#1941) must answer `false` where success rests on varying an `IReadOnlySpan<…>` argument away from the span type.
- Function-type slots: already rejected, with the wrong diagnostic. The compiler's broader under-implementation of §27.1.1 (generic interface slots and `Object` targets are invariant) is a pre-existing impl-behind delta. It is noted for the tracker and not caused by this proposal.

---

## 7. Self-critique

- **Is this the narrowest sound rule?** It rejects exactly the conversions that move an `IReadOnlySpan<TElement>` value to a type not read as holding its borrows, in the directions in which they are conversions at all. A class implementing `IReadOnlySpan<int>` varying *to* the span type is not caught, deliberately: that direction only adds tracking.
- **Composed positions.** `() -> IReadOnlyList<IReadOnlySpan<int>>` to `() -> IReadOnlyList<Object>` is a return-slot reference conversion whose relation is §12.9's variance at a nested argument. "Composed positions included" catches it through §12.9. `IReadOnlyList<() -> IReadOnlySpan<int>>` to `IReadOnlyList<() -> Object>` is a type-argument conversion whose relation is §27.1.1's at a nested slot, caught the same way.
- **The downcast face.** The first draft closed every *static* retyping but left the run-time type test: an `Object` holding a list of spans narrowed by `as IReadOnlyList<Object>`. §5 item 2a closes it. A function-typed narrowing from `Object` (`o as (IReadOnlySpan<int>) -> Object` over a capture-free `(IReadOnlySpan<int>) -> IReadOnlySpan<int>` boxed to `Object`) is covered by the same sentence if run-time tests at function types honor §27.1.1 variance; the spec does not say whether they do (a residual question, filed separately).
- **The lambda literal's annotated parameter.** The spec states how a lambda's return-type ascription is judged against the target (§27.3: "judged as a value's return type under §27.1.1"). It does not state how a written parameter type is judged. The compiler judges it by §27.1.1 contravariance (`ExprTypecheck.cpp:5862–5960`, fix #4897). C names both forms explicitly for the span case, so the rule does not depend on that unstated question. The general question is filed as new (§8 of `V1.md`).
- **R7 parity.** The set-erasing twin needed §28.14's formation bar because a function value's conversion to `Object` is *admitted* when capture-free, so the conversion inside a formation adapter could not simply be "the" conversion. A span's conversion to `Object` is never admitted, so the adapter's conversion can simply be the rejected one. No separate bar is needed.
- **Unchecked mode.** After C, a span reaches a field only through the residual-trust surfaces proposal 0153 enumerated (a frame in a field, a slot's current value). It never reaches one in its own right.
