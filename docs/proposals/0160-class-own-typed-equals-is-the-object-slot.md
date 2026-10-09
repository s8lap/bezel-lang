# Proposal 0160 — A class's own-typed `Equals` is the `Object` slot

| | |
|---|---|
| **Status** | graduated-v0.334 — superseded — graduated v0.334 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues" and the 2026-10-09 direction "resolve all findings autonomously": option B of the retrospective's R8 — a class's own-typed `Equals` (at `C` or `C?`) is the implementation of `Object`'s `Equals` slot, two spellings of one virtual member, one per class, reached by dispatch from `=`, the comparer-less hash containers, the type-parameter path, `IEquatable<C>` and every `Object` surface; proposal 0158's clause (d) shrinks to the `Equals(other: I)` shape and its remedies go; #5755 and #5731 ride it; critique round 1 applied, shared with proposal 0161 (six fixes), which stated what the slot is to §22.6 (one candidate, exact at its nearest declaration's parameter type), corrected the enum-and-choice precedent (they reject the `Object?` spelling) and the residual (d) example, and fixed two code-generation-reference lines; landed with the v0.334 changelog entry) |
| **Tracking issue** | [#5755](https://github.com/s8lap/bezel-lang/issues/5755) (`spec-defect`: does a comparer exempt a hash container from the hashable requirement?) and [#5731](https://github.com/s8lap/bezel-lang/issues/5731) (`spec-question`: the second kind's argument typed by another type parameter) ride this proposal; the retrospective item is `scratch/matrix2/RETRO.md` R8. One `spec-tracking` compiler tracker is filed at landing (§7) |
| **Target module(s)** | **spec**: §3.6 (the **hashable** row's clause (d) shrinks to the `Equals(other: I)` shape), §8.3 (the pairing paragraph becomes the slot rule), §8.4 (one sentence), §9.2 *Through a type parameter* (the ambiguity clause loses its parenthetical; the second-kind sentence gains #5731's reading), §12.1 (one sentence), §12.2 (the `Object`-members passage), §14.2 (the equality-search register row's remedy), Appendix C.1 (the `GetHashCode` bullet). **stdlib**: §2.9, §4 *Equality search at an element type with no equality* (two clauses), §4.8 and §4.10 (#5755, one sentence each), §9.6 (`String` writes one spelling). **rationale**: §8.1 (one dated paragraph). **code-generation reference**: §7.4 (the *Looks right, is rejected* pair and its note). **Guide 07**: one sentence (pin move noted in the lane md). **`DECLINED.md`**: three `declined-at-proposal` rows; seven existing rows annotated |
| **Written against** | spec/stdlib **v0.333** at `429fd72` (main). Section numbers are v0.333's. |
| **Origin** | `scratch/matrix2/RETRO.md` item R8 (rank 2), the retrospective over v0.330–v0.333's landed equality fixes, under the owner's 2026-10-09 direction "Please also resolve all findings autonomously". It needs a proposal because it changes settled rules: it removes v0.333's stated rejection of `Set<Point3>` and of `=` at a subclass operand, changes what an `Object`-typed `Equals` answers on a class that declares an own-typed `Equals`, and adds one rejection (both spellings on one class). |

*Reference convention: unprefixed §-references are language-spec references at v0.333; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

Since v0.330 a class's `Equals(other: Point)` has been an **overload** beside the inherited `Equals(Object?)` (§12.2). Two members that answer one question is the shape every later fix has been patching: v0.332 (proposal 0155) extended the hash pairing to the own-typed member and ruled that `=` at a subclass operand is §22.6's ambiguity; v0.333 (proposal 0158) made a class at which that call is ambiguous — a subclass of `Point`, a class whose member is declared at `C?` — **not hashable**, rejected `Set<Point3>`, and amended five documents so that the remedy text stopped naming a member `Point3` cannot declare. Across §3.6, §8.3, §8.4, §9.2, §12.2, §14.2, Appendix C.1, stdlib §2.9 and §4, the two-member reading now costs about 1,100 words, and the matrix's first run found its seams still open: #5755 (does a comparer exempt the container?) and #5731 (what type does the second kind's argument carry?).

**Decision (option B).** On a class, an `Equals` whose one parameter is the declaring class's own type, `C` or `C?`, **is the `Object` slot's implementation** — the rule v0.327 gave enums and choices (§10.5, §35.2), carried to the one other kind that can declare such a member. It and `override Equals(other: Object?)` are two spellings of one virtual member; a class writes one of them; `=`, the comparer-less hash containers, the type-parameter path, `IEquatable<C>` and every `Object` surface reach it by virtual dispatch, so a deriving class that overrides the slot is compared by its override wherever its values are compared. The pairing rule is unchanged: the own-typed spelling obliges `override GetHashCode()`, discharged only by the declaring class's own.

Everything 0155 and 0158 built to keep two members agreeing has no subject once there is one member: §3.6's clause (d) shrinks to the one shape that can still be ambiguous (a class declaring `Equals(other: I)` for an interface it implements), §9.2's parenthetical, §8.3's `C?` carve, §14.2's register remedy and stdlib §4's amendments go, and stdlib §2.9 says one thing. #5755 and #5731 are decided on the way (§4). No new syntax, no new keyword, no new mechanism.

Newly admitted: `p = d` and `p.Equals(d)` at a subclass operand, `Set<Point3>` and its kin, a `C?`-declared member's class as a hash key — zero sites in `tests/`, `docs/` and the corpora (§6). Newly rejected: a class declaring both spellings — zero user sites; stdlib §9.6's `String` drops one line.

---

## 1. The problem

### 1.1 What the two-member reading costs

The texts that exist only to keep `Equals(other: Point)` and `Equals(Object?)` from disagreeing, at v0.333:

1. **§8.3**, the pairing paragraph (0155): "On a class the rule reads one more member: a non-`override` `Equals` taking the declaring class's own type, at `C` or `C?` … obliges the `GetHashCode` override as an `Equals` override does, because `=` and `<>` …, a `Set<C>` or `Dictionary<C, V>` built without a comparer, and every use through a type parameter at `C` … compare by it; at `C?` those uses are §22.6's ambiguity and `C` is not **hashable** …, and a call with a `C?`-typed argument still selects it."
2. **§8.4** (0155): "an operand at which that call is ambiguous is the same compile error at `=` and `<>`: `p = d` at `d: Point3`, a subclass of a `Point` that declares `Equals(other: Point)`, as `p.Equals(d)` is …, remedied by binding `d` at `Point` first."
3. **§3.6** clause (d) (0158): "a class or interface type at which the call `a.Equals(b)` over two operands of that type is §22.6's ambiguity … — a class deriving from one that declares `Equals(other: Point)`, or a class whose own `Equals` is declared at `C?` — which has no member for §9.2's first kind".
4. **§9.2** (0158): the ambiguity clause's parenthetical ("at a class deriving from one that declares `Equals(other: Point)`, the inherited member is not reached through a type parameter …").
5. **§12.2** (v0.330): "an argument that converts to both parameter types and matches neither exactly — one of a class deriving from `Point`, or a `Point` where the overload is declared at `Point?` — is §22.6's ambiguity … `a = b` and `a <> b` … and a use through a type parameter … are such calls."
6. **§14.2** (0158), the register row's remedy: "or, at a type §3.6's clause (d) excepts, forming the collection at the base that declares the own-typed `Equals`".
7. **Appendix C.1** (0155, 0158): two sentences.
8. **stdlib §2.9** (v0.330, 0158): a 200-word paragraph on which member each spelling reaches, ending "overrides `Equals(Object?)` where an `Object`-typed comparison must give the same answer".
9. **stdlib §4** (0158): three amendments to the equality-search paragraph.

And three questions the reading leaves open, two of them filed by the v0.333 matrix:

- **#5755.** Stdlib §2.9 and 0158 say a `Set<D>` "without a comparer is rejected … or given a comparer"; stdlib §4.8 and §4.10 make hashability a structural check at every naming (§14.1). The texts disagree on whether `new Set<Point3>(cmp)` compiles.
- **#5731.** For the second kind, `Same<T, U>(a: T, b: U) => a.Equals(b)`, §9.2 does not say whether the "same use on an S-typed operand" carries `U`'s substitution (binding `Equals(Point)` at `<Point, Point>`) or the body's `Object?` (binding `Equals(Object?)`, identity).
- **The subclass override.** A `Point3` that overrides `Equals(Object?)` to compare `Z` as well is, in a `Set<Point>`, compared by `Point`'s non-virtual `Equals(Point)` — 0158 §3's own objection to its option A, which the adopted option B does not remove: a `Set<Point>` holding `Point3` values still compares by the base's member against the subclass author's override.

### 1.2 What the compiler does today

Source-read at `429fd72`; no build exists in this checkout, so nothing was run.

- `src/sema/SymbolCollection.cpp` `isEqualityBearing`: every class is hashable (the fall-through `return true`). 0158's clause (d) was never implemented (tracker [#5736](https://github.com/s8lap/bezel-lang/issues/5736)).
- `src/codegen/LlvmBackend_Calls.cpp:1651–1677` `classEqualsOverride`: the membership lowering (`Contains`, `Remove`, the set and dictionary paths) compares by the **first single reference-parameter `Equals` found walking up the chain** — an own-typed `Equals(Point)` or an `override Equals(Object?)`, whichever is nearest. So for a `Set<Point3>` the compiler already compares by `Point`'s `Equals(Point)`: it implements neither 0158 (reject) nor identity, and is one step from this proposal (it lacks virtual dispatch to a `Point3` override of the slot and the `Object`-surface wiring).
- `src/sema/TypeResolution_Checks.cpp:4185–4214` (per 0155 §7): the pairing reads `override` only; 0155's tracker [#5724](https://github.com/s8lap/bezel-lang/issues/5724) carries the own-typed obligation.
- How `o.Equals(p)` through an `Object` receiver dispatches on a class with only an own-typed `Equals` is unprobed: the vtable's `Equals` slot is presumably `Object`'s identity member, which this proposal changes (§7).

---

## 2. Options

| | Option | `Set<Point3>`, `Point3` declaring nothing | `Set<Point>` holding `Point3` values that override the slot to compare `Z` too | `o.Equals(p)` through `o: Object` on a `Point` | Text |
|---|---|---|---|---|---|
| **A** | Status quo: two members, 0155's pairing and operator rule, 0158's clause (d) | rejected (not hashable); `Set<Point>` or a comparer | `Point.Equals(Point)`, non-virtual: the override is not reached | identity, unless `Equals(Object?)` is also overridden | the ≈1,100 words of §1.1 |
| **B** | **One slot, two spellings, virtual (adopted)** | admitted: the inherited slot (`Equals(Point)`) and hash | the `Point3` override, by virtual dispatch | `Equals(Point)`: the slot | one paragraph in §8.3; the rest deleted or shortened |
| **C** | One slot, but both spellings admitted on one class, the `Object?` one being the slot and the own-typed one its typed overload | as B when one is written; two members when both are | as B | the `Object?` one | B's paragraph plus a rule for the pair |
| **D** | B with a non-virtual slot: `p = q` at two `Point` operands is a direct call to `Equals(Point)` | as B | `Point.Equals(Point)` over the override | the slot (identity-free), but the deriving class's override is unreached through a `Point`-typed receiver | B's paragraph plus a dispatch sentence |

The status quo *is* an option here (unlike in 0158): it is internally consistent. It is declined on cost and on the subclass-override case.

---

## 3. Evaluation

**Soundness.** No option touches ownership or a borrow. The stake, as in 0155 and 0158, is a silent wrong answer — two surfaces disagreeing where it hurts (`a = b` true while a `Set<T>` keeps them apart, or a collection comparing by a member the subclass's author overrode). B removes every way to get one: the hash containers, `=`, `IEquatable<C>`, the T-path and the `Object` surface reach one member, that member is paired with `GetHashCode` by the existing rule, and the member is virtual, so an override wins everywhere or nowhere. A keeps the one wrong answer §1.1 names (the unreached override in a `Set<Point>`). C reintroduces two members and with them the ambiguity at a subclass operand. D gives the wrong answer A gives, in exchange for a direct call that a `sealed` class, or a class with no override, gets anyway.

**Simplicity.** B is one paragraph where 0155's was, and it has a precedent to cite rather than a mechanism to invent: on an enum and a choice the own-typed `Equals` already *is* the `Object` slot's implementation (§10.5, §35.2). (Those sections' "two spellings" are `override` and plain; an enum or a choice may not declare the `Object?` spelling at all. A class keeps that spelling, because 67 sites write it, so on a class the slot has two parameter spellings as well — the one difference from the precedent, stated in §8.3.) With one member nothing is ranked (§22.6 ranks candidates; there is one), so clause (d)'s subclass and `C?` shapes, §9.2's parenthetical, §8.3's `C?` carve, §14.2's register remedy and stdlib §4's three amendments have no subject and go. Clause (d) itself stays, shortened, for the one shape that remains genuinely ambiguous: a class declaring `Equals(other: I)` for an interface `I` it implements (an `RNode` argument converts to `IIdent` and to `Object?`, exact at neither), and a sub-interface of an interface declaring `Equals(other: IBase)`. That residual has one site in the tree (`xmod_imported_class_vtable_2792`'s `RNode`, never a hash key), and its remedy is now the ordinary one — declare the own-typed `Equals`, which is exact at two `RNode` operands.

Measured text (the lane's edit set, `scratch/v0334/W2.json`): the 0160 edits replace 1,157 words with 1,167 across the spec, stdlib, code-generation reference and Guide 07. The retrospective's "≈1,160 → ≈250" counted deletions only; stated in full at its home, with the residual (d) kept and §12.1 and §12.2 saying how the second spelling matches the slot, the rule is word-neutral. The gain is in rules, not words: one member instead of two overloads and a seven-site agreement protocol; no `C?` carve; no remedy that forms a collection at the base; two trackers withdrawn or shrunk (§7); and two filed questions (#5755, #5731) answered by the rule rather than by more rule.

**Consistency.** §8.4's one property — `a = b` *is* the call `a.Equals(b)` — holds at every operand under B, as 0155 wanted (its E1), and now the call has one candidate, so `p = d` at a subclass operand compiles, as `p = q` does. §9.2's T-path still "reaches the member the same use on an S-typed operand binds"; at `Point3` that is the inherited slot, virtual. §12.2's no-overload-across-the-boundary rule is untouched: a subclass's `override Equals(other: Point3)` is an override, which §12.2 admits for every virtual member, not an overload; a subclass's `Equals(a: K, b: K)` stays blocked as any member is. `IEquatable<C>` is satisfied by the own-typed spelling exactly as before (the member exists with that signature; §11.2's matching is unchanged). The enum and choice rule (§10.5, §35.2) and the class rule now read alike: "the own-typed `Equals` fills the `Object` slot; an argument that is not a `C`, `null` included, compares unequal".

**Relation to recorded declines** (`DECLINED.md`). The v0.330 `rejected` row declines *ranking* the own-typed member above `Equals(Object?)` at an argument that converts to both; 0155's E2 row declines the same ranking relocated to `=`; 0158's A and A′ rows decline reaching the inherited own-typed or nearest member through a type parameter. Each reasons from two members (one ranked over the other, one reached past the other). B has one member, so none of those reasons bind — nothing is ranked, split or reached past — and the rows are annotated as moot rather than reopened: B is not the ranking they refused, it is the removal of the second candidate. 0155's E4 row (`=` always binding the `Object` slot) and 0158's C row (the first kind reaching the `Object` slot) each state the bar "a redesign that binds the hash containers to the slot as well"; B is that redesign, and the rows are annotated as bar met. 0158's D row (a subclass's own `Equals(other: Point3)`) is annotated: under B the subclass's member is the slot's override, admitted as §12.1 admits every override, so §12.2's rule it named as its bar is untouched. 0155's E3 (a fixed member for `=`) and H4 (an inherited hash discharge) stand: `=` is still the call (at `class C: IEquatable<int>`, `c = 5` selects `Equals(int)`, an ordinary overload), and the hash obligation is still the declaring class's own. The v0.327 #5598 ruling for enums and choices is the precedent B follows.

**User-facing cost.** A class that today writes both `Equals(other: Point)` and `override Equals(other: Object?)` writes one; zero user classes do (§6). `string` in stdlib §9.6 drops the `Object?` line, its content-based behaviour unchanged (a `string?` or `Object?` argument reaches the slot; `null` and a non-string compare unequal, as the content-based override already answered). A subclass author who wants a different equality overrides the slot in either spelling and writes the paired `GetHashCode`, as for any override.

**Migration reach** (measured, §6): zero sites flip verdict in `tests/`, `docs/` or the corpora. The change is spec-ahead-of-impl in three places (§7).

---

## 4. Decision

**Option B.** Under the owner's 2026-10-08 delegation and the 2026-10-09 simplification approval this is accepted as a delegated ruling and graduates in v0.334.

1. On a class, an `Equals` whose one parameter is the declaring class's own type, `C` or `C?` (`C` with its own type parameters), is the `Object` slot's implementation: one virtual member with `override Equals(other: Object?)` as its other spelling. A class writes one spelling (the own-typed one with or without `override`); a second declaration in the other spelling is a compile error. A deriving class overrides the slot in either spelling (`override Equals(other: D)` or `override Equals(other: Object?)`), matching it under §12.1 (visibility, return type and parameter modifiers still match exactly, and §22.15/§22.16's override rules apply). To §22.6 the slot is one candidate, applicable at every argument and an exact match at the parameter type of its nearest declaration in the receiver's static type's chain (`Object?` where that declaration is `Object`'s or spelled at `Object?`) — so at a class that also declares `Equals(other: I)` its own operands are exact and a subclass's are not, and the residual (d) is computed, not listed.
2. Every path reaches that member virtually: `=`/`<>`, comparer-less `Set<C>`/`Dictionary<C, V>`, a use through a type parameter at `C` or at a class deriving from `C`, `IEquatable<C>`, and an `Object`- or interface-typed receiver. Through the slot an argument that is not a `C`, `null` included, compares unequal without the body running.
3. The pairing is unchanged in substance: the own-typed spelling obliges `override GetHashCode()` declared by the class itself. `Equals` at any other type (`Equals(other: int)`, `Equals(other: I)`) fills no slot and obliges nothing.
4. §3.6 clause (d) is kept for the residual ambiguity (`Equals(other: I)` on a class implementing `I`; a sub-interface of one declaring `Equals(other: IBase)`); the subclass and `C?` shapes leave it, and with them §9.2's parenthetical, §8.3's `C?` carve, §14.2's register remedy, stdlib §2.9's `D` clause and stdlib §4's amendments.
5. **#5755 (rides).** A hash container's **hashable** requirement is the key type's, read at every naming of the instantiation (§14.1), the comparer constructors included: a comparer chooses the equality the container consults and does not stand in for a key type that has none. Under B the question's motivating case (`Set<Point3>`) is admitted anyway; the ruling decides the residual — a non-hashable record, choice or (d)-type with a comparer stays rejected, which is what the compiler's site-based check (`checkHashContainerKeys`) already does.
6. **#5731 (rides).** The second kind's argument is read at the type the body gives it — `Object?`, or another type parameter's own surface — whatever that parameter's substitution (the issue's reading 2). Under B the observable difference at own-typed classes vanishes (the slot *is* `Equals(Point)`), so reading 2 costs nothing there and keeps a body's equality independent of how two substitutions relate.

---

## 5. The exact normative edits (▲)

The machine-actionable set is `scratch/v0334/W2.json` (ids prefixed `W2/0160`). In prose:

1. **Spec §3.6**, the **hashable** row: clause (d) becomes ▲ "(d) a class or interface type at which the call `a.Equals(b)` over two operands of that type is §22.6's ambiguity (§12.2: as at a class that declares `Equals(other: I)` for an interface `I` it implements but no `Equals` at its own type, or at an interface extending one that declares `Equals(other: IBase)`) — which has no member for §9.2's first kind". (The same edit adds proposal 0161's clause (e).)
2. **Spec §8.3**, the pairing paragraph's class sentences, replaced by the slot rule (quoted in full in `W2.json`, id `W2/0160 §8.3 slot`): the own-typed `Equals` is the slot's implementation as on an enum or a choice, and on a class it and `override Equals(other: Object?)` are two spellings of one virtual member; one spelling per class; to §22.6 one candidate, applicable at every argument, exact at its nearest declaration's parameter type; every path reaches it virtually; a deriving class overrides it in either spelling; a non-`C` argument compares unequal through the slot; the own-typed spelling obliges `override GetHashCode()`, the declaring class's own.
3. **Spec §8.4**: the Point3 sentence becomes ▲ "On a class the member invoked is the one the call `a.Equals(b)` selects (§22.6) — the `Object` slot in either of its spellings (§8.3), dispatched virtually, unless the class declares an `Equals` at another parameter type that the call selects — so an operand at which that call is ambiguous (§12.2) is the same compile error at `=` and `<>`."
4. **Spec §9.2** *Through a type parameter*: the ambiguity clause loses its parenthetical, reading ▲ "and for the first kind also where that use is §22.6's ambiguity (§12.2)"; the three-kinds sentence gains #5731's reading ▲ "`Equals` with an argument of any other type, read at the type the body gives the argument — `Object?`, or another type parameter's own surface, whatever that parameter's substitution".
5. **Spec §12.1**: after the nullability-widening sentences ▲ "The `Object` slot `Equals` has a second spelling, at the overriding class's own type, `D` or `D?`, which matches it (§8.3)."
6. **Spec §12.2**: the `Object`-members passage drops `Equals(other: Point)` from its overload examples, takes `Equals(other: I)` and the `RNode` ambiguity example, and gains ▲ "An `Equals` at the class's own type, `C` or `C?` (`Equals(other: Point)` for `IEquatable<Point>`), is not an overload but the `Object` slot's second spelling (§8.3): it declares the slot's override, `override` written or not, and a deriving class overrides the slot in either spelling."
7. **Spec §14.2**, the equality-search register row: the remedy ▲ "declaring equality on the element type, which makes it **hashable**; or testing a key …" (the forming-at-the-base clause goes).
8. **Spec Appendix C.1**, the `GetHashCode` bullet: 0155's and 0158's sentences become ▲ the slot's second spelling, one spelling per class, the own-typed pairing, and the residual (d).
9. **Stdlib §2.9**: the two-member paragraph becomes ▲ one slot, two spellings, every path, `GetHashCode` paired, "declares no `override Equals(Object?)` beside it".
10. **Stdlib §4**, *Equality search at an element type with no equality*: the ambiguity example becomes `Equals(other: I)`; the remedy's base-collection parenthetical goes.
11. **Stdlib §4.8 and §4.10** (#5755): ▲ one sentence each — the requirement is the key type's at every naming, comparer constructors included.
12. **Stdlib §9.6** (`String`): the `override Equals(other: Object?)` line goes; `Equals(other: string)`'s comment names it the slot's own-typed spelling.
13. **Rationale §8.1**: a v0.334 paragraph after the v0.333 one (text in `W2.json`).
14. **Code-generation reference §7.4**: the rejected block loses its three ambiguity lines and gains a `Both` class declaring both spellings (ERROR); the accepted block shows `p.Equals(sub)`, `p = sub` and `Set<Point3>` admitted; the note's two-member sentences are corrected (the reference's maintenance rule forbids appending a pin, not correcting a wrong sentence).
15. **Guide 07** §12.2 sentence: `Equals(other: Point)` leaves the overload examples; one clause names it the slot's own-typed spelling. The guide's revalidation pin moves with the train.
16. **`DECLINED.md`**: three new `declined-at-proposal` rows (§9); rows for v0.330's ranking, 0155's E2 and E4, 0158's A, A′, C and D annotated (§3).

Not edited: §12.2's rule against overloads across the inheritance boundary; §10.5 and §35.2 (the precedent); §25.2.

---

## 6. Measurement

Scans run by the lane at `429fd72` (fresh, not quoted): a regex over every `class … end Name;` block in `tests/**/*.cpp` (gtest string literals), `tests/**/*.bzl` (lit, conformance, rule corpus) and `docs/*.md`, plus the retrospective's `reach_r8.py` output (`scratch/matrix2/retro_work/reach_r8.out`).

| Shape | Sites | Under B |
|---|---|---|
| A class declaring both `Equals(other: C)` and `override Equals(other: Object?)` | **0** user classes; 1 stdlib declaration (`String`, stdlib §9.6) | the user set is empty; §9.6 drops one line, behaviour unchanged |
| A class deriving from one that declares an own-typed `Equals`, used as a hash key or compared at the subclass type | **0** (0158 §6's scan, re-run: `Equals(other: Point)` appears in 5 test files and 5 docs; no subclass of a declaring class is compared or keyed) | would become admitted; nothing to flip |
| An own-typed `Equals` declared at `C?` whose class is a hash key | **0** (`operator_groups_746`'s `K` is never keyed) | `K` becomes hashable again; no verdict moves |
| An `Object`-typed `Equals` call on a class with an own-typed `Equals` and no `Object?` override, with an identity expectation | **0** found (`c5_equals_object_override` lists 67 `override Equals(Object?)` sites, none beside an own-typed member) | the answer changes from identity to the declared equality at no measured site |
| A class declaring `Equals(other: I)` for an interface it implements | 1 (`xmod_imported_class_vtable_2792`'s `RNode: IIdent`), never keyed, every call exact at `IIdent` | stays (d); no verdict moves |
| Rule-corpus cases | `l2_9_0003` (own-typed `Equals`, no hash: reject) unchanged; 0158's planned `s3_6_*`/`s9_2_*` cases were never written (none names `Point3`) | no case flips |

Critic re-measure (`scratch/v0334/W2crit/scan_both.py` and greps): the both-spellings scan finds only `Object`'s own declaration (spec §8.1, its Equals parameter being its own type) and the code-generation reference's rejected-block `Point`, which is already an ERROR line; a non-`public` own-typed `Equals`, one with `requires`, or one with a dispatch modifier — each now an override-matching error — **0** sites. Newly admitted on paper but already compiled: `override Equals(other: D)` at a class whose chain declares no own-typed member (`universal_member_arity_2446`'s `user_override_correct_arity_accepted` expects it to compile; v0.333 §12.1 matched no virtual member for it).

**Newly rejected: none measured** (the both-spellings rule has zero user sites). **Newly admitted: none measured** (zero sites write the admitted shapes). No banked baseline verdict moves.

---

## 7. Compiler facts and the trackers

- `src/sema/SymbolCollection.cpp` `isEqualityBearing`: classes fall through to `true`. Under B that is right for every class except (d)'s residual, which 0158's tracker already describes; **[#5736](https://github.com/s8lap/bezel-lang/issues/5736) shrinks** to the `Equals(other: I)` and sub-interface shapes.
- `src/codegen/LlvmBackend_Calls.cpp` `classEqualsOverride`: nearest reference-parameter `Equals` up the chain. Under B the lowering must dispatch the slot virtually (so a `Point3` override wins through a `Set<Point>`), and the class's vtable `Equals` slot must be filled by the own-typed spelling (so `o.Equals(p)` through `Object` reaches it, with the slot's type test and null check in front of the body).
- `src/sema/TypeResolution_Checks.cpp` pairing (0155's [#5724](https://github.com/s8lap/bezel-lang/issues/5724)): unchanged obligation; the tracker's `=`-ambiguity probes are withdrawn.
- **Tracker to file** (spec-ahead-of-impl, §8.3 / §12.2 / §12.1, v0.334; cross-link #5724, #5736, [#5669](https://github.com/s8lap/bezel-lang/issues/5669)):
  1. The own-typed `Equals` populates the class's `Object` `Equals` vtable slot behind a type test and null check; `override Equals(other: D)` in a deriving class is accepted by the override matcher as the slot's second spelling and overrides it.
  2. A class declaring both spellings is rejected at the second declaration, the diagnostic naming the slot.
  3. Every equality path — `=`, membership, hash containers, the T-path, `IEquatable<C>` dispatch, `Object`-typed calls — reaches the slot virtually.
  4. The hash-container key check (`checkHashContainerKeys`) stays site-based at every naming, comparer constructors included (#5755); regression row: `new Set<R>(cmp)` at a non-hashable record `R` rejected.
  5. The second kind's `U`-typed argument lowers against the body's `Object?` surface (#5731); regression row: `Same<Point, Point>(p, q)` compares by `Equals(Point)` (the slot) — observably the same under both readings now, pinned so a future split is caught.
  6. Regression rows: `p = d` and `p.Equals(d)` at `d: Point3` compile and compare by the slot; `Set<Point3>` compiles; a `Set<Point>` holding `Point3` values that override the slot compares by the override; `o.Equals(p)` through `Object` on a `Point` with only `Equals(other: Point)` compares by it and returns `false` at `null` and at a non-`Point`.

---

## 8. Corpus cases to write (after landing)

- `s8_3_*` accept: a class with `Equals(other: C)` and `override GetHashCode()`; reject: the same class adding `override Equals(other: Object?)`.
- `s8_3_*` run: `Set<Point3>` over two equal-`X` values, `Count` is 1.
- `s8_3_*` run: a `Set<Point>` holding two `Point3` values equal by `X` and unequal by `Z`, with `Point3` overriding the slot: `Count` is 2.
- `s8_3_*` run: `o.Equals(p)` through `o: Object` on a `Point` with only the own-typed spelling: `True` at an equal `Point`, `False` at `null` and at a boxed `int`.
- `s8_4_*` run: `p = d` at `d: Point3`.
- `s12_1_*` accept: `override Equals(other: Point3)` in a subclass; `s12_2_*` reject: a plain `Equals(a: K, b: K)` in a subclass of a class that declares one.
- `l4_8_*` reject (#5755): `new Set<R>(cmp)` at a non-hashable record `R`.
- `s9_2_*` run (#5731): `Same<Point, Point>(p, q)` compares by the slot.

---

## 9. Declined alternatives (one `declined-at-proposal` row each)

1. **A, the status quo** — two members with 0155's and 0158's agreement machinery. ≈1,100 words across five documents for a shape with zero measured sites, and the subclass-override case stays wrong (a `Set<Point>` compares a `Point3` by the base's non-virtual member). *Reopening bar:* a program in which one class needs two equalities, an own-typed one and an `Object`-typed one answering differently, that an `IEqualityComparer<T>` does not express.
2. **C, both spellings admitted on one class**, the `Object?` one being the slot. Two members again, with the subclass-operand ambiguity and the ranking question the one-slot rule removes; the one declaration in the tree that wrote both (`string`) writes one. *Reopening bar:* none stated.
3. **D, a non-virtual slot** for the own-typed spelling, so that `p = q` at two `C` operands is a direct call. It compares a `Point3` held at `Point` by `Point`'s member against the subclass's override — the answer 0158 refused — for a dispatch cost a `sealed` class, or a class with no override, already avoids. *Reopening bar:* a measured program whose equality-dispatch cost is observable.

---

## 10. Self-critique (adversarial pass)

1. **Is this a reopening of the v0.330 decline?** No. That row refused a *ranking* between two overloads. B does not rank; it says there is one member. The row's own ground — "§22.6 ranks no implicit conversion above another" — is satisfied vacuously. The rows are annotated as moot, not reopened; a critic who reads B as a disguised ranking should say which two candidates it ranks.
2. **Does an interface-typed receiver really reach the slot?** `IEquatable<Point>.Equals(other: Point)` is implemented by the own-typed member, which is the slot; a call through the interface dispatches to the class's member as every interface call does, and the member is the slot. Through `IEquatable<int>` on `class C: IEquatable<int>` the ordinary `Equals(other: int)` is reached, as today (the 5097/5115 tests pin this and do not flip).
3. **`override` optional on a class?** On enums and choices both spellings are admitted "spelled `override` or plain" because no chain exists. On a class a chain does exist, and a reader of `Point3 { Equals(other: Point3) }` might miss that it overrides. The alternative — requiring `override` on the own-typed spelling — would flip the ≈15 fixtures that write it plain (reach_r8.out) and the code-generation reference's own `Point`. Reach decides: plain is admitted, Guide 07 may advise `override` in a later guide-only pass.
4. **Where does `Point3` get its hash?** From the inherited `GetHashCode` override, as every class inherits its hash. A `Point3` that overrides the slot writes its own hash under the existing pairing. 0155's H4 (an inherited hash discharging a *new* `Equals`) stays declined and is not needed.
5. **The residual (d).** A critic could ask why (d) survives at all. Because `class RNode: IIdent` with `Equals(other: IIdent)` is a genuine two-overload set at an `RNode` argument (one site exists), and 0158's reasoning about it is unchanged. Its remedy is now the own-typed spelling. Deleting (d) would re-admit `Set<RNode>` with no first-kind member — the hole 0158 closed.
6. **Word count.** The retrospective promised a ≈900-word deletion; the edit set is word-neutral (1,157 → 1,167). The simplification is real in rules and trackers, and the proposal says so rather than quoting the promise.
7. **Composition.** §3.6's hashable row carries three lanes' clauses in one line (RC12's (b), this (d), 0161's (e)); the lane's JSON edits them as disjoint substrings. §14.2's register row is also W5's territory (its remedy column only is touched here). W8's R7 touches stdlib §4 at the value-copy paragraph, not the equality-search paragraph. Named in the lane md for the composition step.
8. **Asymmetry is the author's, as with every override.** Under virtual dispatch a `Point3` that overrides the slot as `override Equals(other: Point3)` answers `false` at a plain `Point` argument, while `Point`'s member compares the same pair by `X` and answers `true`; a `Set<Point>` over such values then depends on insertion order. That is the contract every `Equals(Object?)` override already carries (§8.3's "must"), not something B introduces; A avoided it only by never reaching the override.
9. **History narration (process §7).** The normative sentences state the rule as it now stands; the two-member lineage lives here and in rationale §8.1.
