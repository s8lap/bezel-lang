# Proposal 0189 — A type position names a type: §2.7's enclosing-member stop is a rule of name positions

| | |
|---|---|
| **Status** | graduated-v0.339 — superseded — graduated v0.339 (accepted 2026-10-20 as a delegated ruling under the owner's 2026-10-08 direction ("Please resolve and decide all known spec related issues. Do proposals for issues that need them.") and the 2026-10-19 "Proceed with drafting", [#6390](https://github.com/s8lap/bezel-lang/issues/6390), [#6400](https://github.com/s8lap/bezel-lang/issues/6400) and [#6432](https://github.com/s8lap/bezel-lang/issues/6432) ruled together, option C — a type position names a type: at a bare type-name position (a name the grammar reads only as a `Type`; a single-identifier pattern is decided by A.7) only nested types take part in §2.7's enclosing-type lookup, ranked between local scope and the module's declarations, innermost first, the first type with a nested type of the name, declared or inherited, whose arity the reference fixes ending the search; a same-named member of any other kind, on the type's own body or on a type enclosing it, declared or inherited, neither binds nor ends the search; at the positions rule 1's table ranks members the *Through nesting* stop is unchanged and reads "declared or inherited", and §11.6's access grant reaches every enclosing depth, `private` and `protected`, the instance clause conditioning use, not access; critique round 1 applied (the row's position list limited to positions read only as a `Type`, the single-identifier pattern excluded; the stop keyed on arity per §2.2; §11.6's bare-name clause scoped to §2.7's generic-encloser rule; the measured reach recounted — 25 gtest rows, 20 flipping to accept and 5 staying rejected on another ground); landed with the v0.339 changelog entry) |
| **Tracking issue** | [#6390](https://github.com/s8lap/bezel-lang/issues/6390) (a top-level type's own body), [#6400](https://github.com/s8lap/bezel-lang/issues/6400) (a member inherited by a link), [#6432](https://github.com/s8lap/bezel-lang/issues/6432) (a grandparent's `private` member); compiler tracker to be filed at the v0.339 landing (§8) |
| **Target module(s)** | **spec**: §2.7 (*Enclosing-type members*: a new bare-type-name row in rule 1's table; *Through nesting*: the stop sentence, the harm sentence, the accessibility sentence), §11.6 (the access sentence). **rationale**: §9.10 (one paragraph). **proposals**: `README.md` (this row), `DECLINED.md` (three `declined-at-proposal` rows). No stdlib, guide or code-generation-reference text changes: nothing is newly rejected. |
| **Written against** | spec **v0.338** at `d09319a91` (main). Section numbers are v0.338's. |
| **Origin** | The 2026-10-19 ruling sweep (`scratch/rule1019/L4.json`, entries 6390/6400/6432, and `L4.critic.md`) found the text silent at a bare type-name position: rule 1's table — "the normative statement" — has no row for one, and the *Through nesting* stop is scoped to "a nested type's bodies". Two compiler lanes read the same sentence opposite ways (`r31-p-6383` reverted its strict reading; `r31-p-27nest` shipped and pinned the nested-only one, `1a657994e`), and three issues then asked, each from a different link of the chain, what the stop does at a type position. Drafted by lane A2 of the v0.339 train. |

*Reference convention: unprefixed §-references are language-spec references at v0.338. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the standard library companion declares; none invents an API.*

---

## 0. Summary

§2.7 rule 1 looks a bare name up in local scope, then among "the enclosing type's accessible members **in the position given below**", then the module's declarations, then imports. The position is fixed by a table whose rows are a chain head, a bare value read, an assignment target, a bare callee, a head continued by `?.`/`!`, and a warning row. Every row is a position at which a *name* is read and a member could be what is meant. A **bare type name** — the `Box<int>` of `private M: Box<int>? := null;`, a `new`, a generic argument, a base list, a pattern head — has no row. The v0.314 *Through nesting* paragraph then says that in a nested type's bodies the lookup walks the type and its enclosing types, innermost first, "at the rank the table gives its members", and that "the first type that has an accessible member of that name ends the search".

Read as a rule of type positions too, that sentence makes a method, property, field or constant named `Box` end the search for the *type* `Box`, and that is what the v0.338 compiler does inside nested and local types — with a carve-out borrowed from the chain-head warning row so that `property Color: Color` can still be declared, a node-identity repair of that carve-out for a member's own declared type (#6410, #6424), a guard that switches the whole stop off in a top-level type's body (#6390), no walk of base types (#6400), and the §13.1 filter at the second link out (#6432). Each repair was needed because the stop is doing something at a type position that no row of the table asks for.

This proposal decides that **a type position names a type**. At a bare type-name position the enclosing-type lookup finds nested types, innermost first, and nothing else: a same-named member of any other kind, on the type's own body or on a type enclosing it, declared or inherited, neither binds nor ends the search. The table gains that row. At the positions the table ranks members — the name positions — the *Through nesting* stop is unchanged: it now says "declared or inherited" (#6400), and the accessibility it tests is §11.6's, which reaches every enclosing depth (#6432). `property Status: string` and `method Kind(): Status` coexist in one class at any nesting depth; `property Color: Color` needs no carve-out at a type position; and wrapping a class in another class changes nothing about what the type names in its body mean.

**Reach, measured (§5):** no rule-corpus, doc-corpus, specification, guide or standard-library program changes verdict; the banked `s2_7_0003` keeps its `warn`. Twenty-five gtest rows that pin the compiler's type-position stop change expectation — twenty flip from reject to accept (the only verdict flips, all newly admitted) and five stay rejected on another ground (an unresolved name, or §12.9) with the stop's clause gone.

---

## 1. The text, and what it does not say

> 1. **Resolve the leftmost segment `A` as a bare name**: local scope, then the enclosing type's accessible members **in the position given below**, then the current module's declarations, then unambiguous imported bare names (per [§2.2](#sec-imports)'s collision rule), then import **aliases**.
>
> — §2.7 rule 1, v0.338

> **Enclosing-type members.** Inside a type's bodies, the enclosing type's accessible members (inherited included; instance and static, […]) participate in rule 1's bare-name lookup at a position fixed by the name's syntactic shape, decided before any lookup. The decision, tabulated (**the table is the normative statement**; the prose that follows carries its tie-breaks, remedies, and scope):
>
> — §2.7, v0.338

> **Through nesting.** In a nested type's bodies ([§11.6](#sec-nested-types)), the enclosing-type lookup visits the nested type and then each type enclosing it, innermost first, at the rank the table gives its members (a nested type keeping the position the preceding paragraph leaves it); the first type that has an accessible member of that name ends the search. […] Ending the search at the first type that has the name, even when the member cannot be used there, keeps a bare name from passing over an enclosing member and silently resolving to a same-named module declaration or import.
>
> — §2.7, v0.338

> A bare name there in a position [§2.7](#sec-name-resolution-order) rule 1 resolves (a value read, an assignment target, a callee or a chain head) […] A type position, a named-argument label and a member name after `.` are not such lookups.
>
> — §21.10, v0.338

> A nested class has access to all members (including `private`) of its enclosing class when it holds an instance of the enclosing class.
>
> — §11.6, v0.338

Three things are decided: the chain (the type, then each type enclosing it, innermost first; a local type's declaring type standing first among its enclosing types), the stop at name positions ("the first type that has an accessible member of that name"), and the harm the stop prevents (a bare name passing over an enclosing member to a same-named module declaration or import). Three things are not:

1. **Whether the stop runs at a type position at all.** The stop runs "at the rank the table gives its members", and the table gives a type position no rank. §21.10 already says in so many words that a type position is not one of rule 1's lookups. Yet §11.6 says a type nested in a generic type "is named only where §2.7's enclosing-type lookup reaches it", so the lookup does run at type positions for nested types. The text never says which members are candidates there. (#6390 asks this for a top-level type's own body; the nested-only compiler reading is one answer, the uniform reading another.)
2. **Whether a member a link inherits counts for the stop.** The ranking paragraph says "inherited included"; the stop paragraph enumerates enclosing links only (#6400).
3. **What "accessible" means two links out.** §13.1's `private` row says "the declaring class only"; §11.6 grants a nested class access to its enclosing class's `private` members, conditioned on "when it holds an instance" — a condition with no meaning for a lookup (#6432).

---

## 2. Why the stop exists, and where that reason holds

The v0.314 ruling (changelog v0.314, #5372) wrote the stop for name positions: a nested type's body reads `s` or calls `Helper()`, and a bare name that passed over an enclosing type's `s` to a module-level `s` would silently re-mean. "Even when the member cannot be used there" names the instance member a nested body cannot reach without an instance, and the static member of a generic enclosing type that has no current instantiation: both are *found* and *rejected* rather than passed over, so that the program is told what it named. That is the harm sentence's whole content, and at a name position it is right.

At a type position the premise fails. `private M: Box<int>? := null;` can mean nothing but a type. A method `Box()` one link up is not a candidate the author might have meant, so passing it is not re-meaning; it is the only resolution. The one kind of member a type position *can* mean is a nested type, and nested types are already found by the chain, innermost first, before the module's declarations (the 6383 variance rows pin exactly that precedence). A stop that fires on a non-type member at a type position therefore protects nothing and costs three things:

- **The carve-out.** A class that declares `property Color: Color` names the type `Color` in the property's own declaration. Under a type-position stop the property blocks its own type, so the chain-head warning row's carve-out ("unless the member's declared type is the resolved type") had to be imported into the stop (`1a657994e`), and then repaired for the member being resolved, whose resolved type does not yet exist (#6410, a node-identity test). None of this machinery exists at a name position, where the warning row's carve-out lives and belongs.
- **The own-body question.** `property Status: string` beside `method Kind(): Status` is the ordinary shape of a class that stores a status string and classifies it by an enum. Under a uniform type-position stop (option B) it cannot be written without `Conf.Status`; under the nested-only stop the compiler ships (option A) it can be written at top level and not one level in — wrapping the class in another changes what `Status` means in its body, which is the action at a distance the stop's own rationale names.
- **The inherited and accessibility questions.** Each is a real question at a name position (§3.4, §3.5 answer them there). At a type position both are artefacts of running the stop where nothing can be meant.

---

## 3. Options

### 3.1 Status quo

Leave the table without a type-name row and the stop's scope to the reader. Two lanes have already read it opposite ways and pinned both readings; a third issue reads the accessibility sentence a third way. The status quo is what produced the three issues.

### 3.2 Option A — the nested-only stop (what `r31-p-27nest` ships; #6390's recommendation)

Codify the compiler: in a nested or local type's bodies, every member of the name on the nested type itself or on a type enclosing it ends a bare type-name search (the `property X: X` carve-out excepted); in a top-level type's own body no member does. Base types are not walked; `private`/`protected` members of a link two or more out are filtered by §13.1.

### 3.3 Option B — the uniform stop (the 2026-10-19 lanes' recommendation)

A type's own body is the first link of its chain whatever its nesting; every link's members, inherited included, end a bare type-name search; the carve-out is generalised to every link, keyed on the member's own declared type. One rule, one chain.

### 3.4 Option C — a type position names a type (decided)

Rule 1's table gains a bare-type-name row: at a type position the enclosing-type lookup's candidates are nested types only, ranked between local scope and the module's declarations, innermost first; a member of any other kind, declared or inherited, on the type's own body or on a type enclosing it, neither binds nor ends the search. The *Through nesting* stop is a rule of the positions the table ranks members at, and there it is unchanged. Two companion clauses settle #6400 and #6432 at those positions: the stop sentence reads "an accessible member of that name, **declared or inherited**", and the accessibility it tests is §11.6's — the `private` and `protected` members of every type enclosing the body, at any depth, with §11.6's instance clause stated as a condition on *using* an instance member, not on access.

### 3.5 Option D — no stop anywhere

Delete the stop at name positions too. Not considered further: it reopens the v0.314 ruling against its recorded reason (a bare `s` in a nested body silently re-meaning), and no issue asks for it.

---

## 4. Evaluation

| | A (nested-only) | B (uniform) | **C (type position names a type)** |
|---|---|---|---|
| **Soundness** | sound (rejects more) | sound (rejects more) | sound: at a type position the only candidates are types; the stop at name positions is untouched; no borrow, ownership or dispatch rule reads a type position's resolution differently |
| **Re-meaning hazard closed** | none exists at a type position; the stop there closes nothing | same | same — and the harm sentence is scoped to say so |
| **Nesting-independence** | **fails**: wrapping a class in another class changes what a bare type name in its body means | holds | holds |
| **Carve-out** | needed at type positions, including the member's own declared type (#6410, #6424) | needed at every link | **none at a type position**; the warning row's carve-out stays a chain-head rule |
| **Text** | a scope clause ("in a nested type's bodies, and not in a top-level type's own") plus the carve-out restated for type positions | a table row for the type position, "declared or inherited", "the type's own body first", the carve-out restated | a table row for the type position, "declared or inherited", §11.6's access; the harm sentence scoped |
| **Measured reach** (§5) | nothing flips (it is the shipped behaviour); the questions it leaves (#6400, #6432) each need a further rule | `s2_7_0003` (banked `warn`) → `reject`; gtest row 11 → reject; base-type walks at two surfaces | **no corpus, doc or guide verdict moves**; 25 gtest rows pinning the type-position stop change expectation (20 → accept); compiler deletes the stop's type-position plumbing |
| **Simplicity for a writer** | "a type name means the type unless the class is nested and some member shares the name" | "a type name means the type unless some member of the class or its enclosers shares the name; qualify" | **"a type name means a type"** |

Against the owner's standing directions — soundness first, then simplicity, narrowing a feature with negligible measured reach over adding rules — C narrows the stop to the positions its rationale covers at zero measured cost to any banked program, removes a carve-out rather than generalising one, and leaves the fewest words. B is the uniform slogan; it is uniform about a position that has no member to protect. A is the shipped behaviour and the least principled: its nesting-dependence is the very hazard the stop was written against. The recorded recommendations (#6390's author: A; the 2026-10-19 lanes: B) are departed from for these reasons, and each is recorded as a decline (§7).

### 4.1 The two companion rulings at name positions

**#6400 — inherited members end the search.** Rule 1's *Enclosing-type members* paragraph admits "inherited included" members to the lookup, and the stop is that lookup's stop. A stop that tested declared members only would let a bare `s` in a nested body pass over the `s` its enclosing type inherits and bind a module-level `s`: the harm sentence's case. The stop sentence reads "declared or inherited". Under option C this is observable at name positions only; the compiler's two type-position surfaces (`enclosingTypeMemberBlocksBareName`, the §12.9 variance stop) no longer need a base walk, because they no longer stop.

**#6432 — §11.6's access reaches every depth.** §11.6 grants a nested class "access to all members (including `private`) of its enclosing class"; the trailing "when it holds an instance" is the condition for *using* an instance member (a nested body has no instance of the enclosing type — exactly the *Through nesting* instance error), not for the access, since a `private static` member or a `private const` needs no instance. Lexical containment, not derivation, is the ground, so it reaches a grandparent as it reaches a parent, and a local type's bodies, which §2.7 makes "that of the body declaring the local type", inherit it through the declaring type's chain. For the lookup at a name position this means a `private` member two links out is accessible, is found, and ends the search — a `private static` of a non-generic type is then admitted by bare name, a `private` instance member is the instance error. The alternative (#6432 reading (b): §13.1 from the second link out) would pass over `Outer`'s `private Box` to a module-level `Box`, the harm sentence's case again, and would make `private` mean different things one and two links out. §11.6's sentence is rewritten to say what it grants and how each member kind is reached.

---

## 5. Reach, measured

**Method.** `scratch/v0339/A2work/measure.py` (approximate, regex-based; hits inspected by hand) scanned 40,190 programs: every `.bzl` under `tests/` (the rule corpus, the doc corpus, lit), every fenced ```` ```bezel ```` block in `docs/*.md` (spec, stdlib, rationale, guides, the code-generation reference), and every gtest program (the string literals of each `BEZEL_TEST` body). For each class, record or interface body it collected the body's own members (and, by base list, the members of same-program bases) and the bare type names used in type positions in that body, and reported each type position whose name is also a member of the own type (own link), of an enclosing type (enclosing link) or of a declared base (inherited link), excluding a nested type of the name and the `property X: X` carve-out. Names declared as types in the same program and names of standard-library types were then separated from false positives (value reads and callees after `=>` or `,`).

**Result.**

- **Own link, non-carve-out, a declared type:** one program outside the test files that exist to pin the stop — the banked rule case `s2_7_0003` (`public property Status: string` beside `public method Kind(): Status => Status.Active;`, `expect: warn` for the chain head). Under A and C it keeps its verdict; under B it flips to `reject` at the return type.
- **Enclosing link:** only rows of `tests/sema/enclosing_member_bare_type_name_test.cpp`, `enclosing_member_bare_name_variance_stop_6383_test.cpp` and `local_type_nested_type_6411_test.cpp` (the stop's own pins) and the `property X: X` shapes the carve-out admits.
- **Inherited link:** zero (every inherited hit was a value read).
- **Standard-library type names:** zero type-position collisions (seven value-read false positives).
- **Doc corpus, specification, guides, stdlib:** zero.
- **lit:** zero programs assert the stop's diagnostic text ("is a member of the enclosing type") at a type position.

**What flips under C** (listed for the compiler tracker; critic re-count — 25 rows change expectation, 20 of them reject → accept):

- `tests/sema/enclosing_member_bare_type_name_test.cpp`, every `expectStoppedAtEnclosingMember` row (22; the 23rd grep hit is the helper's definition at `:146`): `spec_2_7_nested_method_of_the_name_stops_a_bare_box_t`, `spec_2_7_member_without_any_nested_type_names_the_stop_reason`, `spec_2_7_nested_property_of_the_name_stops_a_bare_box_t`, `spec_2_7_nested_field_of_the_name_stops_a_bare_box_t`, `spec_2_7_nested_static_method_of_the_name_stops_a_bare_box_t`, `spec_2_7_three_levels_the_innermost_member_ends_the_search`, `spec_2_7_three_levels_holder_alone_still_ends_the_search`, `spec_2_7_public_outer_member_is_accessible_so_it_does_stop`, `spec_2_7_private_member_of_the_nested_type_itself_does_stop`, `spec_2_7_agreement_with_6383_the_resolver_stops_at_the_member`, `spec_2_7_local_types_own_member_stops_a_bare_type_search`, `spec_2_7_local_type_property_of_the_name_stops_the_search`, `spec_2_7_local_types_own_private_field_stops_the_search`, `spec_2_7_local_type_static_member_of_the_name_stops_the_search`, `spec_2_7_local_type_const_of_the_name_stops_the_search`, `spec_2_7_local_type_positional_field_of_the_name_stops_the_search`, `spec_2_7_enclosing_toplevel_member_stops_a_local_types_search`, `spec_2_7_enclosing_private_member_stops_inside_a_local_type`, `spec_2_7_enclosing_protected_member_stops_inside_a_local_type`, `spec_2_7_enclosing_public_member_still_stops_inside_a_local_type`, `spec_2_7_a_local_type_inside_a_local_type_still_sees_the_outer_link`, `spec_2_7_member_of_the_name_declaring_another_type_still_stops`. Of these, 20 become accepted; two stay rejected without the stop's clause: `spec_2_7_member_without_any_nested_type_names_the_stop_reason` (no type `Box` exists anywhere, so it is the plain `unresolved type name`) and `spec_2_7_agreement_with_6383_the_resolver_stops_at_the_member` (`Box<T>` binds `Outer`'s nested `Box<in X>`, and `T` declared `out` then fails §12.9). The file's `expectAccepted` rows stay accepted; only the reason changes, from "the guard" or "the carve-out" to "a type position names a type".
- `tests/sema/enclosing_member_bare_name_variance_stop_6383_test.cpp`, the three `expectStopEndsOnlyTheStoppedChain` rows: `spec_2_7_through_nesting_method_of_the_name_ends_the_composition`, `spec_2_7_the_stop_is_not_a_polarity_or_marker_effect`, `spec_2_7_a_member_typed_by_something_else_still_ends_the_search` — each becomes `expectBothCompose` — still rejected, now with a §12.9 line for both interfaces: the bare head in `Stop` passes `Stop`'s member and binds `Outer`'s nested `Bad<in X>`, as its three `expectBothCompose` siblings already do.
- `spec_2_7_carve_out_admits_the_member_and_leaves_the_name_unresolved` keeps its verdict (reject: no type of the name exists) and its current text.
- `s2_7_0003`, the five `spec_2_7_*` rows of `tests/codegen/choice_body_record_ctor_store_2097_test.cpp`, the two of `local_type_nested_type_6411_test.cpp` and every `expectAccepted` row keep their verdicts.

**What flips at name positions** (#6400, #6432): nothing measured. The compiler's bare-expression lookup from a nested body finds only the innermost type's members today (changelog v0.314, #5372's compiler note), so both clauses land inside that open delta.

---

## 6. Soundness and the neighbouring cells

- **A nested type and a module type of one name.** Innermost first: the nested type is the binding (unchanged; the 6383 precedence rows pin it).
- **A nested type on an enclosing type and a method of the name on the nested type itself** (`Outer.Box`, `Holder.Box()`): the type position in `Holder` names `Outer.Box`. Nothing else could be meant; the method stays reachable as a callee.
- **A member and a type of one name, chain head** (`Status.Active` beside `property Status: string`): the warning row, unchanged — the member ranks last, the type wins, the compiler warns unless the carve-out holds.
- **A member and a type of one name, value read or callee**: the member outranks the type, unchanged.
- **A single-identifier pattern** (`case x of Limit => …`) is not a type position: A.7 makes it a type test or a constant pattern by what the name resolves to, so its shape cannot be decided from tokens. Were it a bare-type-name position, an enclosing `const Limit` would stop binding and a module-level type `Limit` would silently turn a constant pattern into a type test; the row excludes it, and it keeps the resolution it has today (the compiler resolves it by ordinary name lookup, `ExprTypecheck.cpp` `checkTopLevelNamedPattern`). A destructuring head `Limit(…)` is a type position (or a case name through §19.8/§35.5's prefix omission, which this row does not reorder).
- **Arity and inheritance.** A nested type is selected by arity as §2.2 selects among every type rule 1 finds, and an inherited nested type is a candidate as a declared one is (§11.6: `Node<int>` in the body of a type deriving from `Tree`).
- **Generic enclosing types.** §11.6's rule that a nested type does not see its enclosing type's type parameters is untouched; a type position in a nested body still cannot name a type nested in a generic type except where the lookup reaches it (rationale §9.10, v0.316).
- **Local types.** §21.10's "A type position […] [is] not such [a] lookup[]" is now also what §2.7 says; the local-type shadowing rule (a bare name spelled like an enclosing local) is a name-position rule and is unchanged.
- **The §12.9 variance composition** (#6383): a bare nested head still composes; a head that resolves to a module-level or imported type composes at that type. The "stop ends the composition" arm goes with the stop.
- **Adding a declaration or import never silently re-means a type position:** before the addition the name was unresolved (an error) or bound to a nearer type (innermost first, unchanged); after it, the same. Rule 1 outranks rule 2 as before.
- **Accessibility at name positions (#6432).** A `private` member found two links out is subject to the same kind rule as one found one link out: a static member of a non-generic type is admitted, an instance member is the named compile error. Nothing becomes reachable that §11.6 did not already grant; what changes is that the lookup no longer passes such a member to reach a module declaration.

---

## 7. Declined alternatives (rows for `DECLINED.md`, `declined-at-proposal`)

1. **Option A, the nested-only type-position stop.** Reason: a type position can mean nothing but a type, so the stop protects nothing there; and scoping it to nested bodies makes the meaning of a bare type name in a class's body depend on whether the class is nested — the action at a distance the stop's rationale names. Reopening bar: a measured re-meaning hazard at a type position.
2. **Option B, the uniform type-position stop with the carve-out generalised.** Reason: the same absence of a hazard, plus a measured cost (`s2_7_0003` flips, a class with `property Status: string` loses the bare `: Status` in its own body) and a carve-out keyed on a member's own declared type, which the chain-head warning row does not need. Reopening bar: a measured re-meaning hazard at a type position.
3. **Excluding inherited members from the stop at name positions (#6400 reading A).** Reason: rule 1's paragraph admits them to the lookup; a stop that ignored them would pass over an inherited member to a module declaration. Reopening bar: a program where an inherited member's stop is unsound.

(#6432's reading (b) — §13.1 from the second link out — is a ruling-class decline recorded as a `rejected` row by the v0.339 landing, not a proposal decline; see lane A2's notes.)

---

## 8. The normative edits (`scratch/v0339/A2.json`, ids `A2-0189-*`), changelog, migration, trackers

**Spec §2.7.** (i) A new table row after the `?.`/`!` row: *Bare type name* — positions the grammar reads only as a `Type` (a single-identifier pattern excluded: A.7 resolves it by name, so it stays a name position); rank "between local scope and module declarations, nested types only"; effect: a type position names a type, a same-named member of any other kind, declared or inherited, on the type's own body or an enclosing one, neither binds nor ends the search; a nested type of the name is the innermost answer. (ii) The stop sentence gains "declared or inherited" and a second sentence confining the stop at a type position to nested types, declared or inherited, selected by arity as §2.2 selects among the types rule 1 finds. (iii) The harm sentence gains the scope sentence ("a rule of the positions at which the table ranks members; at a type position nothing but a nested type could be meant"). (iv) The accessibility sentence is replaced by §11.6's access at every depth, for nested and local types alike. **Spec §11.6.** The access sentence states the grant (every enclosing class, any depth) and how each member kind is reached. **Rationale §9.10.** One paragraph (v0.339) recording the decision and the three declines. **Proposals.** This row in `README.md` (*Accepted*); three `declined-at-proposal` rows.

**Changelog (draft, lane A2's notes carry the house-style bullet).** Design change, proposal 0189; #6390, #6400, #6432. Kernel: no contact. Body-locality: local.

**Migration.** *Newly rejected:* nothing. *Newly admitted:* a bare type name in a nested or local type's body that a non-type member of the type or of a type enclosing it shares (the 20 newly accepted gtest rows of §5; 5 more change their rejection's diagnostics), and the §12.9 composition through such a head. *Doc-only:* the table row, the harm sentence's scope, the rationale paragraph. *Spec-ahead-of-impl:* at name positions, the stop's "declared or inherited" and §11.6's access at every depth, both inside the open v0.314 delta (#5372's compiler note: the bare-expression lookup consults only the innermost type).

**Compiler tracker (filed at landing).** `src/sema/TypeResolution_Names.cpp`: delete the type-position stop — `lookupBareTypeName`'s `stoppedAt` walk (`:2726–2751`), `enclosingTypeMemberBlocksBareName` (`:2517–2629`) with its top-level guard and `privateVisibleViaDeclaringBody`, and the carve-out predicate `memberDeclaresCollidingName` (`:2402` ff., the `ty == resolving` arm of #6410) — keeping `typeBodyScopes_` only if the §12.9 walk still needs it; the §12.9 variance stop's two halves (`src/sema/TypeResolution_Checks.cpp:14547` `enclosingMemberBareName`, applied at `:14881`), which read `owning->members` to end the composition, compose instead. The 25 test flips of §5. At name positions, the v0.314 delta's eventual implementation tests `declared or inherited` and §11.6's access at every depth.

---

## 9. Self-critique (adversarial pass)

- **"You are reversing a fix the compiler just shipped, with a 36-row test file pinning it."** The rows pin a reading the text never stated; the lane that shipped it said so in its own test file ("a pin of current behaviour, not of a ruling" is how the 2026-10-19 sweep classed it, and #6390's author asked for exactly this ruling). The reading needed four repairs in two weeks (the guard, the carve-out at type positions, #6410, #6424) and left two residuals filed as spec questions; a rule that fights the language this hard at a position where nothing can be meant is the wrong rule, not an under-implemented one.
- **"Option B is the simplest statement: one chain, first holder wins."** It is the simplest *slogan*. Written out it needs the carve-out at every link, a node-identity test for a member's own declared type, and `Conf.Status` in a class that stores a status string. C's statement is one table row and a scope sentence, and it deletes code.
- **"Is `s2_7_0003` really the spec's shape, or a corpus author's?"** A corpus author's — but it is the ordinary shape, and the warning row exists precisely because such classes exist. Under B the warning row's own idiom (a member and a type of one name) would be writable at a chain head and not at a type position in the same body.
- **"Does C let a nested type's method `Box` be shadowed silently?"** No: methods are not type-position candidates under any reading, so nothing is shadowed; `Box()` as a callee still resolves to the method (callee row, member outranks type).
- **"Does the §21.10 sentence already decide this, making it a clarification?"** It says a type position is not one of rule 1's lookups, which is C's premise; but §11.6 runs the enclosing-type lookup at type positions for nested types, and the compiler reads the stop as covering them. A reading the compiler ships and pins is retired here, so the proposal class is the honest one.
- **"#6432 under C changes nothing observable."** At type positions, yes (the rows flip for 0189's reason). At name positions it decides whether `Inner`'s body may call `Outer`'s `private static Box()` by bare name (admitted) and what a bare `private` instance `Box` is (the instance error, not a module `Box`). Both are inside the open v0.314 delta and are stated so the implementation does not have to guess, which is what the issue asked.
- **Measurement limits.** The scan is regex-based; it cannot see a type position inside a C++ string that is assembled by helpers (`moduleLevel(...)`), so the gtest count comes from reading the two stop test files directly, not from the scan. Standard-library type-name collisions were checked against the stdlib document's declared names (189). An inherited collision across files of a multi-file corpus case is not seen; every multi-file case was also read by the own-link and enclosing-link passes, which found nothing.
- **Shared paragraphs.** Lane E edits §2.7 rule 1 (#6068, type parameters) and §19.3's named-slot bullets (#6047); the anchors here are the table's `?.` row, three *Through nesting* sentences and §11.6's access sentence, none of which E quotes.
