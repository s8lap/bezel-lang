# Proposal 0141 — Dispatched generic methods: requirements are decided at the body, and no move-only or inline-array substitution reaches one

| | |
|---|---|
| **Status** | graduated-v0.327 — superseded — graduated v0.327 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option D, the exclusion widened to inline arrays by critique round 1, whose 4 blocking and 6 non-blocking findings, shared with the #5511 and #5554 rulings, are resolved; landed with the v0.327 changelog entry) |
| **Tracking issue** | [#5509](https://github.com/s8lap/bezel-lang/issues/5509) (the question; `needs-owner-decision`, decided under the owner's 2026-10-08 delegation) |
| **Target module(s)** | **spec** — §14.2 (a new sub-subsection *Dispatched generic methods* after *Constraints on overrides and interface implementations*), §3.6 rule 2 (one sentence), §14.6 (one clause), Appendix C.1 (one bullet); **rationale** — §10 (one paragraph); **code-generation reference** — the *What a use of `T` in a generic body demands* table (one row); `DECLINED.md` (three `rejected` rows) |
| **Written against** | spec **v0.325** at `b27728a` (main). The stdlib companion, rationale and code-generation reference are at the same commit. Written in parallel with the v0.326 guide train (`scratch/v0326/`), which touches no section named here. |
| **Origin** | v0.319's `gen2` lane ([#5504](https://github.com/s8lap/bezel-lang/issues/5504), item (e)1) ruled that a requirement reaches an outer declaration *through a substitution* and scoped itself to instantiations so formed; it filed the instantiation formed by **dispatch** as this question. Rationale §10's v0.319 paragraph ends: "An instantiation reached by dispatch rather than formed by substitution, a generic method's override reached through its base's signature, is not decided here." |

*Reference convention: unprefixed §-references are language-spec references at `b27728a` (v0.325); K tags are §28.3's kernel ids; rationale, code-generation-reference and proposal references are marked. Quotations are verbatim. ▲ marks proposed text, landed nowhere. The programs below use only members the stdlib companion declares; none invents an API.*

---

## 0. Thesis

§3.6 rule 2 decides a generic method's interior-use requirements "at each instantiation of the method, a call at explicit or inferred type arguments among them". For a method reached by **dispatch** — an interface method, a `virtual` or `abstract` method, and their overrides and implementations — that site does not exist: the call `i.Find<R>(…)` on `i: IFinder` instantiates `IFinder.Find<R>` and dispatch reaches `F.Find<R>`, a body the caller cannot see and, across modules, need not even have in its compilation closure (§2.8.3). The spec today says nothing about where `F.Find<R>`'s requirements are decided, and the two readings the issue names pull apart: declaration-site (§14.2's "dispatch flows through the base or interface signature, so its constraints are the only ones call sites are ever checked against") and call-site (§14.6's attribution of an override's edges to the overridden member's node).

This proposal decides **declaration-site**, uniformly for every body dispatch can reach, and closes the gap that makes the declaration-site reading expensive on its own: with no admitted constraint stating copyability (§7.10), an override that reads a `T`-typed parameter twice, boxes it, or binds it could not be checked without an instantiation. It therefore adds one instantiation-site rule stated in the signature's own terms: **a dispatched generic method's type parameters admit no move-only and no inline-array substitution**, checked at every call (and carried through substitution by §3.6 rule 2 as every instantiation-site rule is). Inside every body dispatch can reach, the parameter is then copyable and not an inline array, and the move-only-keyed requirements (§7.10's copy demand, §8.6's boxing rule, §19.3's binding arm) and the inline-array-keyed ones (§15.1's element rules for a `[N]T`, and — after this train's #5511 ruling — the `T?` a body forms, which §7.10 itself recommends) need no instantiation to decide them. The remaining substitution-read requirements (equality, `as` at a unit substitution, the class-only owning-element rules, a wrapper carrying one) are errors at the body unless the inherited constraints establish them, with the remedy §14.2 already names: the constraint goes on the declaring interface or base method (for `as`, §17.4's `is` test is the other remedy).

Measured reach: **zero** flips in `tests/` (26 files, 146 dispatched generic method declarations surveyed), the docs and the rule corpus. The `copyable` constraint the issue floats is declined with a reopening bar (§5.6).

---

## 1. Ground truth

### 1.1 The rule today

- **§3.6 rule 2** (line 541): "a use of a generic **type's** parameter, in any member body … is therefore a requirement of the constructed type, checked at every site that names it whether or not the program reaches that member; a use of a generic **method's** own parameter is checked at each instantiation of the method, a call at explicit or inferred type arguments among them." And its closing sentences: "A member neither form can carry — among them an override, an interface implementation, … — makes its use a requirement of the type, stated in the signature by a constraint on the type's parameter." That closing sentence is about the *type's* parameter; it says nothing of an override's **own** method type parameter, whose constraint set the override cannot change.
- **§14.2 *Constraints on overrides and interface implementations*** (line 2489): an override or implementation "**inherits the overridden or implemented declaration's constraints**"; "A **differing** clause — added, removed, narrowed, or widened relative to the inherited set — is a compile error naming the inherited constraints: dispatch flows through the base or interface signature, so its constraints are the only ones call sites are ever checked against — an *added* constraint on the override would go unenforced at every virtual call (unsound)". So the implementer has no spelling for a requirement its body carries beyond the inherited set.
- **§14.6** (line 2710): "An override's body contributes its edges to the overridden or implemented member's node, since dispatch reaches it ([§14.2])." This attributes the override's *instantiation requests* to the base node for the bounded-closure check. It decides no verdict about requirements, but reads as if the override's body were checked where the base's is.
- **§3.6 rule 2's deferred set** — the requirements a generic body carries on a bare `T` that are read at the substitution — is, by the spec's own list: §7.10's copy demand (a second read of `T`-typed storage; move-only substitutions turn copies into moves and reject what needs a copy), §9.2's equality use (a non-hashable record or choice), §17.4's `as` target (an inline-array or unit substitution), §19.3's type-parameter binding arm (a move-only substitution), §15.1's element rules for `[N]T` (an inline-array or move-only substitution), §8.6's boxing rule (a move-only substitution), §14.4's / §15.1.1's / §28.5's class-only rules (where the body forms `OwningArray<T>` or `[N owned]T` — admitted only under a class or type bound, then rejected at a runtime-managed substitution), and every instantiation the body forms at `T` that carries one of these (`Bag<T>`, `Dictionary<T, int>`, `Eq<T>(…)`; v0.319's chain). Rendering (§35.11, v0.319 #5503) and the §28.3 flow rules (v0.319 #5507: checked once under the weakest assumption) are **not** deferred.

### 1.2 The compiler today (`src/` at `b27728a`; no build was available to this lane — every statement below is read from source and recorded test names, not probed)

- There is **no interior-use per-instantiation check at all** (v0.317's #5481 item; restated by v0.319's #5504 tracker). Equality on an unconstrained `T` at a non-hashable record is not rejected anywhere.
- The `as`-target check has two joins: `Resolver::checkAsTargetInstantiations` for a **type** parameter and `ExprTC`'s deferred join for a **method** parameter (`src/sema/TypeResolution.hpp:247–262`, `degenerateAsTargetReason`). The method-parameter join fires at the *direct* call of the method whose body contains the `as`; a call through an interface or base reaches a different declaration's join, so an override's `o as T` is unchecked through dispatch. This is the hole in miniature.
- `where T: value` admits move-only inline arrays and owning choices at every generic method (§7.10's design), with no distinction for a dispatched one. `tests/codegen/generic_virtual_method_erased_layout_2896_test.cpp` records (by its name and its program) that a generic virtual method is realized over an **erased layout** — one body for every type argument — which is why a move-only argument reaching an override's copy today would be a bitwise duplicate with no check (HYPOTHESIS; not probed).
- Generic virtual dispatch itself is implemented and exercised cross-module (`tests/module/xmod_virtual_generic_dispatch_5124_test.cpp`, `xmod_library_internal_virtual_generic_dispatch_5214_test.cpp`, `tests/codegen/virtual_generic_override_580_test.cpp`, `virtual_generic_inherited_override_5256_test.cpp`, `virtual_generic_generic_subclass_override_5247_test.cpp`, `overloaded_virtual_generic_dispatch_5113_test.cpp`, `virtual_generic_new_constraint_cross_module_5033_test.cpp`).

---

## 2. Measurement: what the existing sites do with their own `T`

A survey of every `virtual`, `abstract` or `override` method declaring its own type parameters in `tests/` and `docs/` (`grep -rE "(virtual|abstract|override) method [A-Za-z_]+<"`: 146 matches in 26 test files, 2 in the docs — both prose in §12.1 / §14.1 and the changelog), plus every interface member declaring its own type parameters. *(Critic correction: the standard library declares **no** interface method and no `virtual`/`abstract` method with type parameters of its own — measured by walking every `interface … end` block in the stdlib companion and the spec, and grepping `(virtual|abstract) method X<` in `docs/`; the LINQ-style operators (`Select<U>`, `ThenBy<TKey>`, `ToDictionary<TKey>`) are extension members, §26, not dispatched. In `tests/`, interface methods with their own type parameters appear in five files — `virtual_generic_override_580` (`Show<T>`), `xmod_library_internal_virtual_generic_dispatch_5214` (`VI<T>`), `xmod_override_omitted_where_generic_constraints_5076` (`M<T>`), `iface_method_where_clause_5391` (`M<T> where T: class`), `interface_impl_type_param_correspondence_5392` (`Show<U>`) — whose implementations return a constant, render, or are the bodies listed below.)* What the bodies do with the parameter:

| Body shape | Files | Deferred requirement under §3.6 rule 2 today? | Verdict under this proposal |
|---|---|---|---|
| `return 0;` / `return 1;` / `=> 77` (parameter unread) | 2391, 5114 ×2, 5076, 5009 (`Tag<T>`), 2050, 1579 lit ×8, 5264, 5392, 5113 | none | unchanged |
| `$"Base({x})"`, `x.ToString()` | 580, 5392, 2896 | none (rendering is not a requirement, v0.319 #5503; `ToString` binds `Object`'s member) | unchanged |
| `b.Get().Area()` under `where T: IV…` | 5256, 5247, 5124, 5214 | none (established by the bound) | unchanged |
| `var t := new T();` under `where T: …, new()` | 5033, 5009 | none (a declared constraint) | unchanged |
| `F := x;` where `F: Object?` (a box of the parameter) | 5129, 5216 | **yes** — §8.6 rejects boxing a move-only `U`, read at the substitution | unchanged: the parameter is never move-only (§3's rule 2), so the box needs no instantiation |

No body compares its own `T`, uses it as an `as` target, forms `[N]T` or `T?` over it, or names a user generic at it. No call of a dispatched generic method in `tests/` or `docs/` supplies a move-only or an inline-array type argument (none of the 31 files above contains an inline-array type or a `choice` declaration) (`grep -rE "<\[[0-9]+ owned\]"`: two hits, both stdlib type instantiations — `OwningBuffer<[5 owned]R>`, `Channel<[2 owned]Node>` — and both already rejected on other grounds). The rule corpus (`tests/conformance/rules/manifest.tsv`, 2,196 cases) has no case keyed on an override's interior use or on a dispatched call's type argument kind; `s14_2_0009`–`0020` are the constraint-satisfaction cases and none names an override body.

**Reach of the two rules: zero sites.** The declaration-site rule rejects a body that today compiles and defers; the move-only/inline-array exclusion rejects a call that today compiles. Neither shape occurs.

---

## 3. Options

### 3.1 Option A — status quo (silent)

Leave the text as v0.319 left it. The override's requirements are then decided nowhere: §3.6 rule 2 names a site (the call) that cannot see the body, and the body is checked once under the inherited set with its deferred uses admitted. At a dispatch to `F.Find<R>` with `R` a non-hashable record, `F.Find`'s `xs[i] = x` reaches `Object.Equals` on a box — the #5481 reflexivity hazard — and at a move-only `R`, a copy inside the override duplicates an owning value. **Unsound**; also the process's standing-hazard class (an unenforced rule the compiler mirrors).

### 3.2 Option B — call-site (the issue's reading 2)

Carry the implementation's requirement to every call of the base or interface member, as §14.6 attributes its edges. **Unenforceable under separate compilation**: the implementing module need not be in the caller's compilation closure (§2.8.3 rule 1 supplies the closure of the *caller's* imports), and even inside one closure the set of implementations is open until link. It also contradicts §14.2's own ground ("its constraints are the only ones call sites are ever checked against"). A link-time variant — checking the program's whole instantiation set (§2.8.2's instantiation obligation) — has no source site for its diagnostic, against §3.6 rule 2's "the diagnostic landing at the first site whose argument decides the requirement". Declined (§5.6, row 1).

### 3.3 Option C — declaration-site only (the issue's reading 1, bare)

Check every override and implementation under the inherited set with no deferral: every substitution-read requirement the set does not establish is an error at the body. Sound. But the deferred set includes the three **move-only-keyed** requirements, and no admitted constraint excludes a move-only substitution short of `class` or a class bound: `where T: value`, `notnull`, `hashable` (an owning inline array of a hashable class element is hashable by §3.6's clause (c)) and `sendable` all admit one. So an override over a value-kind `T` could not read its parameter twice, box it (`F := x`, two test files today), or bind it in a `case`. The copy gap the issue names, exactly. Option C alone would flip `method_own_type_param_named_local_release_5129` and `…_array_add_release_5216` and make generic interface methods over values nearly unimplementable. It composes worse still with this train's #5511 ruling, which makes a `T?` at an inline-array substitution a compile error at the instantiation site: under option C every dispatched body declaring a `T?` local — §7.10's own advice for an unconstrained `T` — over a parameter not class-bounded would be rejected at the body, and no admitted constraint short of a class bound excludes an inline array.

### 3.4 Option D — declaration-site, plus no move-only substitution through dispatch (**recommended**)

Option C's rule for every body dispatch can reach, plus one instantiation-site rule stated in the signature's terms: the type parameters of a dispatched generic method admit no move-only value type and no inline array as a type argument. The call is rejected where it is written, through any receiver type; where the argument is or contains a type parameter in scope, §3.6 rule 2 carries it to the enclosing declaration's instantiations (the #5504 chain, the shape every instantiation-site rule already has). Inside every reachable body the parameter is copyable and not an inline array, so §7.10, §8.6, §19.3 and §15.1's element and nullable-form rules need no instantiation; the other deferred requirements have admitted constraints to establish them (`hashable`; a class bound), stated on the declaring member, which the implementation inherits, and `as T` at a unit substitution has §17.4's `is` test besides.

*Why inline arrays too (critic round).* The lane's first draft excluded move-only types only. That left §15.1's inline-array-keyed requirements — `[N]T`'s nested rule and #5511's no-nullable-form rule — to be decided at the body, where no admitted constraint short of a class bound can establish them, so a `T?` local in a dispatched body over a `value`, `notnull`, `hashable` or unconstrained parameter would have been a compile error at the body. That is the copy gap again, in its most common shape. Excluding inline arrays at the call is the same move the move-only exclusion makes, costs no measured program (no dispatched call in `tests/` or `docs/` passes an inline array), and matches the cost #5511 already records for a generic storing `T?`.

Why uniform over the declaring `virtual` body too: a call on a base-typed receiver reaches the base body only when no override exists; checking the base body per instantiation at its calls while checking overrides at their declarations would be one method with two postures, and the direct-call check protects nothing dispatch does. Measured cost of uniformity: zero (no `virtual` body in the corpus defers a requirement).

### 3.5 Option E — a `copyable` admitted constraint instead of the exclusion

Add **copyable** (= not move-only, §15.3) to §3.6's derived-property table and to §14.2's admitted set, so an interface author writes `method Find<T>(xs: Array<T>, x: T): int where T: copyable;` and the implementation inherits it. Sound and explicit, and it would also give non-dispatched generics a signature for a requirement v0.319 noted has none. Against it: every generic interface or virtual method whose implementations copy, box or bind their parameter must carry the constraint **in the declaring interface** — a module the implementer may not own, so a library interface written without it closes its implementations to the most ordinary body shape; and the copy demand §7.10 decides implicitly everywhere else would be explicit at one kind of site. Under option D the same programs compile unchanged, and a move-only value through a dispatched generic method — for which no program exists (§2) — is the only thing given up. The owner's standing direction (prefer simplicity; no new surface without strong need) decides it: declined with a reopening bar (§5.6, row 2).

### 3.6 Option F — reject move-only substitutions at *every* generic method

Simpler still, but it retires §7.10's designed behaviour ("for move-only inline arrays, copy operations are replaced by moves") for the non-dispatched case, where the per-instantiation check works and `Swap<T>(ref a: T, ref b: T)` over an owning inline array is a plausible program. Not proposed.

---

## 4. The options side by side

| | Sound | Enforceable per module | Programs rejected today (measured) | New surface | Remedy for the copy gap |
|---|---|---|---|---|---|
| A status quo | no | — | 0 | none | — |
| B call-site | yes in principle | **no** | 0 | none | per-call |
| C declaration-site | yes | yes | 2 test files; every value-kind override reading `T` twice | none | none |
| **D** C + no move-only or inline array through dispatch | yes | yes | **0** | one instantiation-site rule | implicit |
| E C + `copyable` constraint | yes | yes | 2 test files until interfaces are edited | a §3.6 row + admitted-set entry | explicit, at the interface |
| F move-only excluded everywhere | yes | yes | 0 | retires a §7.10 behaviour | implicit |

---

## 5. Decision (delegated ruling, 2026-10-08)

### 5.1 The rule

**Option D.** The text is §6's.

1. A **dispatched generic method** is an interface method, instance or static (§11.8), or a `virtual` or `abstract` method, declaring type parameters of its own, together with every `override` of one and every method implementing one (an inherited witness included).
2. **Requirements are decided at the body.** Every body of a dispatched generic method is checked under its constraint set as any generic body is, and a requirement of its own type parameter that the set does not establish is a compile error at that body, never a requirement carried to the method's instantiations. The diagnostic names the requirement and the remedies: a §14.2 constraint on the **declaring** interface or base method (inherited by every implementation), or a named type in the parameter's place. For an inherited witness, the check lands at the conforming class.
3. **No move-only or inline-array substitution.** A call of a dispatched generic method at a move-only type argument (§15.3) or an inline array of any form (§15.1), explicit or inferred, through any receiver type, is a compile error at the call; at an argument that is or contains a type parameter in scope, §3.6 rule 2 carries the requirement to the enclosing declaration's instantiations. In every reachable body the parameter is copyable and not an inline array.
4. §14.6's edge attribution is unchanged and decides no requirement.

### 5.2 Kernel classification (spec-change-process §9)

**No contact.** Neither rule touches an ownership axiom: rule 2 moves the site of a check §3.6 rule 2 already makes; rule 3 is an admissibility restriction on type arguments of the §14.4 / §15.1.1 kind. No consume position, no borrow source, no flag schedule changes.

### 5.3 Body-locality (proposal 0105 §1)

- Rule 2 at a declared override, implementation or `virtual` body: **local** — the body's own check, under its own constraint set, with the deferral removed.
- Rule 2 at an inherited witness: **fact-discharged** — the per-body fact is *the set of its own type parameters' requirements the body defers to instantiation*, yielded at the inherited body's single check; the conforming class consumes it and rejects where it is non-empty. No body is re-checked.
- Rule 3: **local** at the call; **carve-out, confinement (i)** for the chain through a type parameter in scope — the #5504 shape, composed by substitution over §14.6's request graph.

### 5.4 Implementation sketch (`b27728a`)

- The interior-use check does not exist (#5481); when it lands, a dispatched generic method's body yields its deferred set as an error instead of a fact, and the inherited-witness consumer runs at interface-conformance checking (`resolveMember`'s method arm, the site #5393 extended for positional correspondence).
- `ExprTC`'s deferred `as` join for a method type parameter (`TypeResolution.hpp:247`) already computes "this body defers an `as` requirement on its own parameter"; for a dispatched method it reports at the body.
- Rule 3: at every call resolving to a dispatched generic method (direct, interface or base receiver), after inference, test each type argument with `common::isMoveOnlyValueType` (`src/common/OwnershipFacts.cpp:2327`) and for the inline-array kind; where the argument contains a type parameter in scope, record the requirement on the enclosing declaration for the chained passes (`checkOwningElementInstantiations`'s worklist shape, `TypeResolution_Checks.cpp`), which v0.319's #5504 tracker already asks to track method type parameters.

### 5.5 Tests that flip

**None.** §2's survey: no dispatched body defers a requirement the inherited set does not establish other than the move-only-keyed box in 5129/5216, which rule 3 discharges; no call supplies a move-only argument. Tests to add at implementation (the corpus gaps §6.4 records): the `IFinder` equality implementation (reject at the body, diagnostic naming `where T: hashable` on `IFinder.Find`); the same under `where T: hashable` on the interface (accept); an override copying its parameter twice at an unconstrained `T` (accept, run); `i.Put<[2 owned]Node>(arr)` and `i.Put<[3]int>(v)` (reject at the call); `G<U>(i: ISink, a: U) => i.Put<U>(a)` at `G<[2 owned]Node>` and at `G<[3]int>` (reject at `G`'s instantiation, naming the chain); an override declaring `var r: T? := null;` over an unconstrained `T` (accept).

### 5.6 `DECLINED.md` rows (all `rejected`; entered at graduation)

1. Carrying an override's or implementation's interior-use requirement to the calls of the overridden or implemented member, or deciding it at link time over the program's instantiation set (§3.2).
2. A `copyable` derived-property constraint as the spelling of a dispatched generic method's copy requirement, in place of the move-only and inline-array exclusion (§3.5). Reopening bar: a program that needs a move-only value or an inline array through a dispatched generic method, or a non-dispatched generic that must state copyability in its signature; the constraint would then be one §3.6 row and one admitted-set decision.
3. Checking overrides and implementations at the body while leaving a `virtual` method's own body per instantiation at its direct calls (§3.4's uniformity).

---

## 6. Edits at graduation (each `old` unique at `b27728a`; the exact JSON is `scratch/v0327/dI.json`)

1. **§3.6 rule 2**, after "a use of a generic **method's** own parameter is checked at each instantiation of the method, a call at explicit or inferred type arguments among them." ▲ "A **dispatched generic method** — an interface method, or a `virtual` or `abstract` method, declaring type parameters of its own, with every override of one and every method implementing one — has no such site for the bodies dispatch reaches: a use of its own parameter that its constraint set does not establish is a compile error at the declaring body, and its parameters admit no move-only and no inline-array substitution ([§14.2](#sec-dispatched-generic-methods) *Dispatched generic methods*)."
2. **§14.2**, a new sub-subsection `#### Dispatched generic methods` (anchor `sec-dispatched-generic-methods`) after *Constraints on overrides and interface implementations*, with the two rules of §5.1 in full (text in the JSON).
3. **§14.6**, the attribution sentence gains: ▲ "; the requirements its body carries are not attributed that way — they are decided at its declaration ([§14.2](#sec-dispatched-generic-methods))".
4. **Appendix C.1**, one bullet after the member-`where` bullet.
5. **Rationale §10**, one paragraph after the v0.322 #5556 paragraph, recording the decision and the three declines.
6. **Code-generation reference**, one row in the *What a use of `T` in a generic body demands* table after the generic-**type** row.
7. **`DECLINED.md`**, three `rejected` rows after the v0.319 #5504 rows; the index-size line is recomputed by the coordinator over the revision.
8. **README**, this proposal's live-queue row (status `accepted — …`; flipped at landing).

### 6.1 Changelog (drafted in `scratch/v0327/dI.md`)

### 6.2 Migration

*Newly rejected (spec-ahead-of-impl):* a dispatched generic method body whose use of its own parameter is a requirement the inherited set does not establish (`xs[i] = x` in an implementation of `method Find<T>(xs: Array<T>, x: T): int;` with no `hashable` on the interface; `o as T`, `[2]T`, a `Dictionary<T, int>` local in an override); a call of a dispatched generic method at a move-only or inline-array type argument, directly or through a type parameter in scope. Measured reach: zero sites. *Newly admitted:* nothing — copies, boxes and bindings of the parameter in such bodies were admitted and stay admitted. *Doc-only:* the §14.6 clause, the Appendix C.1 bullet, the rationale paragraph, the code-generation row.

### 6.3 Corpus

No case states the old rule (none exists to state). Gaps recorded: the five programs of §5.5.

### 6.4 Trackers to file at landing

- Spec-ahead-of-impl: the body-site check for dispatched generic methods (folds into the #5481 interior-use work; names the inherited-witness consumer and the `as` join's reporting site).
- Spec-ahead-of-impl: the move-only and inline-array exclusion at calls of dispatched generic methods and its chain through a type parameter in scope (`where T: value` admits both today; `isMoveOnlyValueType` plus the inline-array kind test is the predicate).

---

## 7. Self-critique (adversarial pass)

- *Does rule 2 reject `$"{x}"` or `x.ToString()` in `Show<T>(x: T)`?* No: rendering is not a requirement (v0.319 #5503 made §35.11's list explicit), and `ToString` binds `Object`'s member for an unconstrained `T`. The 580 and 2896 tests stand.
- *Does rule 2 reject `F := x` (a box of the parameter into an `Object?` field)?* Without rule 3 it would, at a move-only substitution's §8.6 reading. With rule 3 the parameter is never move-only, so the box is the ordinary §8.6 conversion. Checked against 5129 and 5216.
- *Is "the inherited set" enough to establish equality?* `where T: hashable` establishes §9.2's first kind and `GetHashCode` (§14.2 *Derived-property constraints*: "Inside the body, `T` counts as having the property wherever a rule of this specification demands it"); a class bound establishes `as`, `[N]T`'s element rule and §14.4's class-only rule. The one requirement no constraint establishes short of a class bound is §14.4's / §28.5's runtime-managed exclusion under `where T: class` (`OwningArray<T>` in an override admitting `string`); its remedy is the class bound, as §14.4 already says for a user type.
- *Can rule 3 be laundered?* Through a type parameter in scope: `G<U>` calling `i.Put<U>(…)` — caught at `G`'s instantiations by the chain. Through a bound `where T: U` with `U` the class's parameter at a move-only class argument: `class C<U>: ISink` whose `Put<T>(x: T) where T: U`… the interface cannot name `C`'s `U`, so the override's constraint set would differ from the inherited one (§14.2) — rejected already. Through a function value: a dispatched generic method is instantiated at type arguments wherever it is named as a value, which is a call-shaped site under §14.1 ("a call to a generic member at explicit or inferred type arguments"); the rule reads the same site.
- *Does rule 3 contradict `where T: value`'s text ("inline arrays (all forms, including `[N owned]…` owning forms)")?* It narrows what the constraint admits **at a dispatched method only**, as an instantiation-site rule, which §14.2 *A type parameter as the type argument* keeps separate from constraint satisfaction. §7.10's sentence stays true of the constraint; the rule is stated in §14.2 and cross-referenced from §3.6. A reader of §7.10 alone would not learn it; the code-generation row carries it, and the rationale paragraph names the seam.
- *Why not state rule 3 as a change to §14.2's inheritance rule (an implicit `class`-like constraint)?* Because it is not a constraint: it must not be "decided at the naming by `P`'s own constraints" (§14.2's rule for a type-parameter argument), or `G<U>(i: ISink, a: U) => i.Put<U>(a)` with an unconstrained `U` would be rejected at `G` itself rather than at `G<[2 owned]Node>`. The instantiation-site shape is the one the #5504 chain exists for.
- *Composition with #5511 (same train, critic round):* #5511 makes `T?` at an inline-array substitution a compile error at the instantiation site, a body's own `T?` local included. Under the move-only-only exclusion every dispatched body declaring a `T?` over a non-class-bounded parameter would have been rejected at the body. The exclusion now covers inline arrays, so that body is admitted (corpus gap recorded in §5.5).
- *Does rule 3 over-reject the standard library?* No: the stdlib declares no dispatched generic method (§2's correction). `Select<U>` over `IEnumerable<T>` is an extension member and is not reached; a class `U` (`Select<OwnedThing>`) is not move-only in any case.
- *Composition with v0.326's §28.3 bare-type-parameter clauses:* none of the edited sections is §28.3; the §3.6 rule 2 sentence is appended after v0.319's text, before the "A requirement reaches a declaration through a substitution" sentence, and quotes nothing v0.326 moves. *(Critic round, against HEAD `c69ff18` with v0.326 applied.)* Textually disjoint, but v0.326 added two per-instantiation requirements on a method's own type parameter, keyed on a borrow-bearing substitution (§3.6 rule 2): #5616 (an `owned T` result carrying a borrow §9.7 does not admit, which bites only under `where T: value`, the class-or-interface reading already rejecting it otherwise) and #5620 (§9.7's return bullet at a `where T: value` parameter, `Id<T>(a: T): T where T: value => a` rejected at `Id<Pair>`). In a dispatched body rule 2 decides both at the body, and no admitted constraint short of a class bound excludes a borrow-bearing value type, so `method Id<T>(a: T): T where T: value;` implemented as `=> a` is a compile error at the implementation. The remedies exist and are the ones v0.326 names: the declaration without `value` (#5620's own decline: "the unconstrained declaration is the remedy", checked once by §28.3's *Bare-type-parameter returns*), or a fresh return. Measured: zero sites (no dispatched body in `tests/` or `docs/` returns its parameter under `where T: value` or declares an `owned T` result). Not excluded at the call like move-only types: a borrow-bearing record is an ordinary, common type argument, and the requirement exists only for a body that returns a parameter-sourced borrow.
- *Separate compilation of the inherited-witness fact:* the fact is per body and the artifact interface already carries per-declaration requirement summaries (§2.8.6's rows); a witness in another module is read through its interface. §2.8.6 may want a row for the dispatched-method case (a body edit that newly defers a requirement is now a *rejection of the body*, not a client-visible summary change); flagged for the coordinator, not edited here.
