# Proposal 0158 — A type whose `Equals` is ambiguous at its own operands is not hashable

| | |
|---|---|
| **Status** | graduated-v0.333 — superseded — graduated v0.333 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option B, against the issue's recommended reading — a class or interface type at which `a.Equals(b)` over two of its operands is §22.6's ambiguity has no member for §9.2's first kind and is not hashable (§3.6 clause (d)); critique round 1 applied, 1 blocking and 6 non-blocking findings resolved; that round edited §8.3's pairing paragraph, which named the `C?` form the new clause makes not hashable, corrected the remedy in §9.2's diagnostic, the §14.2 register row and stdlib §4, dropped a hash-mix ground against the inherited member that the adopted remedy shares, and counted two `C?`-declared sites, not one; landed with the v0.333 changelog entry) |
| **Tracking issue** | [#5716](https://github.com/s8lap/bezel-lang/issues/5716) (`spec-question`: a comparer-less `Set<Point3>`, `Point3` deriving from a class that declares `Equals(other: Point)`); one `spec-tracking` compiler tracker is filed at landing (§7) |
| **Target module(s)** | **spec**: §3.6 (the **hashable** row gains a clause (d)), §8.3 (one clause on the pairing paragraph's `C?` form), §9.2 *Through a type parameter* (the no-member clause names the ambiguity; the diagnostic sentence), §14.2 (the equality-search register row's remedy), Appendix C.1 (one sentence in the `GetHashCode` bullet). **stdlib**: §2.9 (one clause), §4 *Equality search at an element type with no equality* (three sentences amended). **rationale**: §8.1 (one dated paragraph). **code-generation reference**: §7.4 (one line in each block of the existing `Object`-member pair). **`DECLINED.md`**: four `declined-at-proposal` rows |
| **Written against** | spec/stdlib **v0.332** at `6a3f45f` (main). Section numbers are v0.332's. |
| **Origin** | Surfaced by the v0.332 critic of proposal 0155 (finding N8, `scratch/v0332/U4.critic.md`; 0155 §10 item 5) and filed as #5716. The owner directed on 2026-10-08: "Please resolve and decide all known spec related issues. Do proposals for issues that need them." It needs a proposal because the decision removes a stated admission: §3.6 counts every class **hashable**. |

*Reference convention: unprefixed §-references are language-spec references at v0.332; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

Since v0.330 a class may declare `Equals(other: Point)` beside `Object`'s `Equals(Object?)`, and the two form one overload set at the class and at every class deriving from it (§12.2). At two operands of a deriving `Point3`, `x.Equals(y)` and `x = y` are §22.6's ambiguity, and §12.2 says that "a use through a type parameter ([§9.2]) [is] such a call". But §3.6 counts every class **hashable**, `Set<T>` and `Dictionary<K, V>` require only that, and §9.2's no-member clause names no ambiguity. So `new Set<Point3>()` is admitted with no member to compare by.

**Decision (option B).** A class or interface type at which `a.Equals(b)` over two operands of that type is §22.6's ambiguity has no member for §9.2's first kind and is **not hashable**. `Set<Point3>`, `Dictionary<Point3, V>`, `where T: hashable` at `Point3`, an equality search over `Point3` elements and an `=` through a type parameter at `Point3` are compile errors at their sites. The remedies are a collection at `Point`, which holds `Point3` values and compares by `Equals(Point)`, or a comparer.

The issue's recommendation (reading 1: reach the inherited own-typed member) is declined. It is the ranking v0.330 declined and v0.332 declined again for `=` (0155 option E2), relocated to §9.2, and it falls to the ground recorded against E2: at a `Point3` that overrides `Equals(Object?)` it compares by `Point`'s non-virtual `Equals(Point)`, against the override the subclass's author wrote.

No new syntax, no new keyword, no new mechanism: one clause in an existing derived property, keyed to §22.6. Newly admitted: nothing. Newly rejected in `tests/`, `docs/` and the corpora: nothing (zero sites).

---

## 1. The problem

The issue's program (spec v0.332):

```bezel
public class Point: IEquatable<Point>
  public readonly property X: int;
  public method Equals(other: Point): bool => X = other.X;
  public override method GetHashCode(): int => X;
end Point;

public class Point3: Point
  public readonly property Z: int;
end Point3;

var s := new Set<Point3>();       // which Equals does the set use?
```

The text says four things that do not fit together:

1. §12.2: "an argument that converts to both parameter types and matches neither exactly — one of a class deriving from `Point`, or a `Point` where the overload is declared at `Point?` — is §22.6's ambiguity … `a = b` and `a <> b` ([§8.4]) and a use through a type parameter ([§9.2]) are such calls."
2. §9.2 *Through a type parameter*: "At every other S, each kind reaches the member the same use on an S-typed operand binds". At `S = Point3` the first kind's use binds no member, because it is ambiguous.
3. §9.2's no-member clause: "Where S has no member for the use's kind (for the first kind and `GetHashCode`, exactly where the same use on an S-typed operand draws the use-site error above, [§35.7]'s or [§15.1.2]'s; …), the instantiation is rejected at its site". The list is closed and does not name §22.6's ambiguity.
4. §3.6's **hashable** row excepts (a) records and (b) choices without equality, and (c) nullable types and inline arrays over a non-hashable type. Every class is hashable. §9.2 closes with "Every **hashable** S ([§3.6]) has a member for all three kinds", which is false at `Point3`. Stdlib `Set<T>` and `Dictionary<K, V>` require only **hashable** (stdlib §4.8, §4.10), and §3.6 rule 2 says a standard-library type's member bodies contribute no interior requirement.

So a user generic's `=` at `Point3` is arguably rejected by items 1 to 3, but `Set<Point3>` is admitted by item 4 with its comparison unspecified. The same gap reaches `Dictionary<Point3, V>`, `OwningSet<Point3>`, LINQ's hashable block at `Point3`, the stdlib equality search (`Array<Point3>.Contains`, whose admissibility is keyed on "a member for … the first kind"), and every other overload set that is ambiguous at its own type:

- a class whose own-typed `Equals` is declared at `C?` (§12.2's second example: a `Point` argument at a `Point?`-declared overload);
- a class implementing an interface `I` whose `Equals(other: I)` it declares (`class RNode: IIdent` with `Equals(other: IIdent)`): an `RNode` argument converts to `IIdent` and to `Object?`, matching neither exactly.

### 1.1 What the compiler does today

Source-read at `6a3f45f`; no build was run.

- `src/sema/SymbolCollection.cpp:2434` `isEqualityBearing`, the one predicate behind the `hashable` constraint, the hash-container key check (`TypeResolution_Checks.cpp:4330`, `checkHashContainerKeys`) and the `=` use-site error, returns `true` for every class (the fall-through at the end of the record and choice arms). `Set<Point3>` compiles.
- `src/codegen/LlvmBackend_Calls.cpp:1651–1677` `classEqualsOverride`, used by the membership lowering, returns the **first** single-parameter `Equals` with a pointer-typed parameter found walking up from the element class. At `Set<Point3>` it returns `Point`'s `Equals(Point)`. If `Point3` declares `override Equals(Object?)` it returns that override instead. If `Point` declared both, it returns whichever comes first in `Point`'s member list. That is no rule the text states.
- How `x.Equals(y)` resolves at two `Point3` operands is unprobed. The v0.330 tracker [#5669](https://github.com/s8lap/bezel-lang/issues/5669) carries the §22.6 resolution probe, and the v0.332 tracker [#5724](https://github.com/s8lap/bezel-lang/issues/5724) carries the `=` probes.

---

## 2. Options

| | Option | `Set<Point3>`, `Point3` declaring nothing | `Set<Point3>`, `Point3` overriding `Equals(Object?)` to compare `Z` too (and `GetHashCode()`, as §8.3 obliges) | `Eq<Point3>(a, b)` with `Eq<T>(a: T, b: T) => a = b` |
|---|---|---|---|---|
| **A** | The first kind at a class reaches the own-typed `Equals` the class declares or inherits (#5716 reading 1, the issue's recommendation) | `Point.Equals(Point)`; hash `Point.GetHashCode()` | `Point.Equals(Point)` (X only, non-virtual), not the override | `Point.Equals(Point)`, where `a = b` at two `Point3`s is an error |
| **A′** | A, but the nearest `Equals` up the chain wins, an `Equals(Object?)` override or an own-typed one (what `classEqualsOverride` does) | `Point.Equals(Point)` | `Point3`'s override | as the set |
| **B** | **The use is §22.6's ambiguity, as §12.2 says, so the type is not hashable and the instantiation is rejected (#5716 reading 2) — adopted** | rejected; `Set<Point>` or a comparer | rejected; same remedies | rejected at the instantiation |
| **C** | At such a type the first kind reaches the `Object` slot, `Equals(Object?)` | `Object`'s identity `Equals`; hash `Point.GetHashCode()` | `Point3`'s override | `Equals(Object?)` |
| **D** | Let `Point3` declare its own `Equals(other: Point3)`, which is exact at two `Point3`s | the author adds the member | the author adds the member | the member, once declared |

The status quo is not an option. It admits the instantiation and leaves the compared member unstated, and §9.2's last sentence ("Every **hashable** S … has a member for all three kinds") contradicts it.

---

## 3. Evaluation

**Soundness.** No option touches ownership or a borrow. The stake is a silent wrong answer, which proposal 0155 treated as decisive ("the divergence is a wrong answer, not a style").
- **A** gives one. In the table's third column, `Point3`'s author overrode `Equals(Object?)` to compare `Z` as well. A `Set<Point3>` would compare by `Point`'s non-virtual `Equals(Point)`, which ignores `Z`, and nobody chose the base's equality for a `Point3` collection. `DECLINED.md` records this ground against E2 ("at two subclass operands it would take the base's non-virtual `Equals(Point)` over the subclass's `Equals(Object?)` override without the author's choosing"). The stake is which equality is used, not a hash mismatch: a `Point3` `GetHashCode()` override that hashed `Z` would already break §8.3's hash rule against the inherited `Equals(Point)`, which `p.Equals(q)` at `Point`-typed operands reaches for `Point3` values, so a consistent `Point3` hashes by `X` alone (critic correction; see §10 item 9).
- **A′** takes the override in that column but ranks by declaration depth. The same `Point3` values in a `Set<Point>` and a `Set<Point3>` then compare by different members.
- **C** gives one in the second column. `Object`'s identity `Equals` keeps apart the points a `Set<Point>` merges. 0155 declined the `Object` slot for `=` on the same split (option E4).
- **B** gives none. Every type it admits has exactly the member the same call selects, and every type it refuses is refused statically, with a remedy.

**Consistency.** §9.2's T-path is defined so that a use through a type parameter means what the same use at the substituted type means ("the member the same use on an S-typed operand binds"). §12.2 applies that to this exact case ("a use through a type parameter … [is] such a call"), and §8.4 makes `d1 = d2` a compile error at two `Point3`s. Under B, `Set<Point3>`, `Eq<Point3>` and `d1 = d2` all agree. Under A and A′, a type parameter admits a comparison the direct spelling refuses, so wrapping `=` in a generic function would be a way around §22.6. B also makes §9.2's last sentence true again: every hashable S has a first-kind member.

**Relation to recorded declines.** v0.330's `rejected` row declines "Ranking a class's own-typed `Equals` above the inherited `Equals(Object?)` at an argument that converts to both (… a subclass-typed argument)". Its grounds are that §22.6 ranks no implicit conversion above another and that a tie-break keyed on one member name would be the only exception. It adds: "A subclass-typed argument stays §22.6's ambiguity, remedied by binding at the intended type." Option A is that ranking, applied where §9.2 reads the use. 0155's E2 row declined the same ranking relocated to §8.4, with the bar "a reopening of the v0.330 row with evidence against its stated reasons". The issue offers two grounds for A:
- The compiler already picks the inherited member. Implementation state is not a design ground, and §1.1 shows the compiler does not implement A either: it implements A′.
- Rejecting a hash set of a subclass is a surprising cost. That is a cost argument, not evidence against the row's reasons, and the row names its remedy.

So process §2's bar is not met. B is the reading v0.330's §12.2 already wrote for the type-parameter use, carried to the derived property that admits the stdlib containers.

**Simplicity.** B is one clause in §3.6's **hashable** row and one in §9.2's no-member clause, both keyed to §22.6 rather than to a list of shapes. It covers the subclass case, the `C?` declaration and the interface-parameter case with one test, and it needs no rule about which inherited member is "own-typed". A needs a definition of the inherited own-typed member, which leaves the `RNode` shape (an `Equals(IIdent)` that is not own-typed) undecided. A′ needs a ranking by declaration depth that §22.6 does not have. D reopens §12.2's no-overload-across-the-boundary rule, which v0.330 kept for exactly this member ("a class's own `Equals(other: Point)` blocks that name in its subclasses as any member does").

**User-facing cost.** B refuses `Set<Point3>`, `Dictionary<Point3, V>`, `OwningSet<Point3>`, `xs.Distinct()` over `Point3`, `Array<Point3>.Contains` and `=` through a type parameter at `Point3`. The remedies:
- **The collection at `Point`.** `new Set<Point>()` holds `Point3` values (a reference upcast) and compares by `Equals(Point)`, the base author's equality. A value read back is a `Point`, and `as Point3` or a type pattern recovers the subclass.
- **A comparer.** An `IEqualityComparer<Point3>` passed to the constructor (stdlib §4.8). Inside it, `a.Equals(b)` is the same ambiguity, so the comparer binds one operand at `Point`, or at `Object?` when the subclass's override is wanted. The author makes that choice in the code.
- **A predicate**, for the equality search (`xs.Any(lambda(x) => x.Id = id)`), as stdlib §4 already says.

`Point3` cannot become hashable by declaring a member: `Equals(other: Point3)` is blocked by §12.2 (option D), and an `Equals(Object?)` override does not remove the ambiguity. Choosing which equality a `Point3` collection uses then falls to the author who forms the collection, which is where §22.6's ambiguity puts it.

**Public contract (§3.6 rule 3).** Adding an own-typed `Equals` to a public non-sealed class flips its subclasses' **hashable**. That change already breaks `d1 = d2` and `d1.Equals(d2)` in client code (§12.2, v0.330), so B adds a consequence to an existing break rather than a new kind of break.

**Migration reach** (measured, §6): zero sites in `tests/`, `docs/`, `tests/conformance/` and the rule corpus. Nothing flips, and the change is spec-ahead-of-impl.

---

## 4. Decision

**Option B.** Under the owner's 2026-10-08 delegation this is accepted as a delegated ruling and graduates in v0.333.

- A class or interface type at which the call `a.Equals(b)` over two operands of that type is §22.6's ambiguity (§12.2) has no member for §9.2's first kind and is not **hashable**. Examples: a class deriving from one that declares `Equals(other: Point)`, and a class whose own `Equals` is declared at `C?`.
- A first-kind use through a type parameter at such a type is rejected at the instantiation. That covers `=`, `<>`, `Equals` with a `T`-typed argument, and the stdlib equality search, which reads the same definition. The diagnostic names the candidates in place of the missing member. The remedies are a type argument at the base that declares the own-typed member, or a comparer (for a `C?`-declared member, a comparer, or redeclaring the member at `C`).
- The second kind and `GetHashCode` are unchanged. At a class, the second kind reaches `Equals(Object?)` and `GetHashCode` reaches the virtual member.

---

## 5. The exact normative edits (▲)

1. **Spec §3.6**, the **hashable** row: "every type except (a) …, (b) …, and (c) a nullable type `T?` …" becomes "… (b) …, (c) a nullable type `T?` …" and, after "…through [§8.4]'s null-safe wrapper or [§15.1.2]'s element-wise equality;", ▲ "and (d) a class or interface type at which the call `a.Equals(b)` over two operands of that type is [§22.6]'s ambiguity ([§12.2]) — a class deriving from one that declares `Equals(other: Point)`, or a class whose own `Equals` is declared at `C?` — which has no member for [§9.2]'s first kind;"
2. **Spec §9.2** *Through a type parameter*, the no-member clause: after "…the use-site error above, [§35.7]'s or [§15.1.2]'s" ▲ ", and for the first kind also where that use is [§22.6]'s ambiguity ([§12.2]: at a class deriving from one that declares `Equals(other: Point)`, the inherited member is not reached through a type parameter, as it is not by `x.Equals(y)` at two operands of the class)". Same paragraph: "the diagnostic naming the use, the type argument, the missing member and the `hashable` remedy" gains ▲ ", or, at an ambiguity, the candidates in place of the missing member".
3. **Spec Appendix C.1**, the `GetHashCode` bullet, after "…declared by the class itself ([§8.3])." ▲ "A class or interface type at which `a.Equals(b)` over two operands of that type is [§22.6]'s ambiguity — a class deriving from one that declares `Equals(other: Point)`, or one whose own `Equals` is declared at `C?` — is not hashable, and a first-kind equality use through a type parameter at it rejects the instantiation ([§3.6], [§9.2])."
4. **Stdlib §2.9** (`IEquatable<T>`), after "…and whose hash is `GetHashCode()`;" ▲ "at a class `D` deriving from such a class that comparison is spec §22.6's ambiguity, as `d1.Equals(d2)` is, so `D` is not **hashable** (spec §3.6) and a `Set<D>` or `Dictionary<D, V>` without a comparer is rejected: the collection is formed at `T`, which holds `D` values, or given a comparer;"
5. **Stdlib §4**, *Equality search at an element type with no equality*:
   - "…or an inline array of one." becomes ▲ "…or an inline array of one, or is spec [§22.6]'s ambiguity, as for a class deriving from one that declares `Equals(other: Point)` (spec §9.2, §12.2)."
   - "The remedy is to declare equality on the element type, which makes it **hashable**, or to search with a predicate" becomes ▲ "The remedy is to declare equality on the element type, which makes it **hashable** (at the ambiguity, to form the collection at the base that declares the own-typed `Equals`), or to search with a predicate".
   - "The owning collections' members of the same names never fire the rule, since their `where T: class` element type always has the member;" becomes ▲ "The owning collections' members of the same names fire the rule only at that ambiguity, since their `where T: class` element type otherwise always has the member;".
6. **Rationale §8.1**: a v0.333 paragraph after the v0.332 0155 paragraph (the JSON carries its text).
7. **Code-generation reference §7.4**: the rejected block gains `var seen := new Set<Point3>();` marked as the same ambiguity. The accepted block gains `var seen := new Set<Point>(); seen.Add(sub);`. The note's prose is not edited (the reference's maintenance rule: a new clause lands as a *Looks right, is rejected* pair, never as a sentence appended to a paragraph).
7a. **Spec §8.3**, the pairing paragraph, after "…and every use through a type parameter at `C` ([§9.2]) compare by it" ▲ "; at `C?` those uses are [§22.6]'s ambiguity and `C` is not **hashable** ([§3.6]), and a call with a `C?`-typed argument still selects it". Without it, §8.3 would say a `Set<C>` compares by a `C?`-declared member that clause (d) makes not hashable.
7b. **Spec §14.2**, the conditionally-admitted register's equality-search row, remedy column: "declaring equality on the element type, which makes it **hashable**" gains ▲ ", or, at a type [§3.6]'s clause (d) excepts, forming the collection at the base that declares the own-typed `Equals`". `Point3` cannot declare equality (§12.2), so the stated remedy alone would be unavailable.
8. **`DECLINED.md`**: four `declined-at-proposal` rows (§9).

Not edited:
- §12.2, whose sentence "a use through a type parameter ([§9.2]) [is] such a call" is the rule B reads.
- §8.4.

---

## 6. Measurement

Scan: `scratch/v0333/V4work/scan.py` (output `scan.out`). It lists every class declaring a non-`override`, non-`static`, single-parameter `Equals` whose parameter is not `Object`. For each it flags an own-typed parameter at `C?`, a parameter naming one of the class's own bases, and every class in the same file deriving from a declaring class. The scan covered `tests/` (gtest sources, lit `.bzl`, `tests/conformance/` and the rule corpus `tests/conformance/rules/cases`) and `docs/`, including the proposals. Every hit was then checked by hand.

| Shape | Sites | Under B |
|---|---|---|
| A class deriving from one that declares a non-`Object` `Equals` | **0** | — |
| A class whose own-typed `Equals` is declared at `C?` | 2. (i) `tests/sema/operator_groups_746_test.cpp` `iequatable_equals_t_not_an_override_1193`: `class K: IEquatable<K>` with `Equals(other: K?)`, asserting only the absence of §8.3's pairing error; no `Set<K>`, no `=`, no instantiation at `K` (found by the critic's re-scan, `scratch/v0333/V4crit/scan2.py`; the lane's scan kept only the first class of each name per file). (ii) `tests/conformance/cases/stdlib_0147.bzl`, a pre-v0.330 copy of `String`'s declaration (`Equals(other: string?)`). It is a stdlib transcription that the conformance manifest marks `skip:signature` (`tests/conformance/manifest.tsv:660`), with no `Set<string>`, no `=` and no instantiation at it. The current stdlib declares `Equals(other: string)` (stdlib §9.6), and `string` is hashable under B | no verdict moves |
| A class declaring `Equals(I)` and implementing `I` | 1: `tests/module/xmod_imported_class_vtable_2792_test.cpp` `RNode: IRank, IIdent` with `Equals(other: IIdent)`. Every `Equals` use is through an `IIdent` receiver (`x.Equals(y)`, exact at `IIdent`) or an `Object` receiver. No hash container, no equality search, no `=` at `RNode` operands, no type-parameter use | no verdict moves |
| Interfaces extending `IEquatable<X>` with a sub-interface | 0 sub-interfaces (`IIdent: IEquatable<IIdent>` only, exact at `IIdent`) | — |
| Every other own-typed `Equals` (`Point`, `Pt`, `C`, `D`, `CS`, `Tag`, `SC`, …) | exact at the declaring class | hashable, unchanged |
| Stdlib classes with a non-`Object` `Equals` | `String` (`Equals(other: string)`, exact) | unchanged |

**Newly rejected: none. Newly admitted: none.** Corpus verdicts: none flip. No banked baseline moves.

---

## 7. Compiler facts and the tracker

- `src/sema/SymbolCollection.cpp:2434–2480` `isEqualityBearing`: classes and interfaces fall through to `true`. It decides the `hashable` constraint (`TypeResolution_Checks.cpp:11066–11086`), the hash-container key check (`TypeResolution_Checks.cpp:4330`, `checkHashContainerKeys`) and the `=` use-site error.
- `src/codegen/LlvmBackend_Calls.cpp:1651–1677` `classEqualsOverride`: the first pointer-parameter `Equals` up the chain (option A′'s behavior, §1.1).
- **Tracker to file** (spec-ahead-of-impl, §3.6 / §9.2, v0.333; cross-link #5724 and #5669):
  1. `isEqualityBearing` returns `false` for a class or interface type at which `a.Equals(b)` over two operands of the type has no exact `Equals` and more than one applicable one: an inherited non-`Object` `Equals` beside `Equals(Object?)`, or an own one at `C?` or at an interface the class implements. `hashableFailureComponentPath` gets a reason clause naming the candidates and the two remedies.
  2. The generic T-path rejects, at the instantiation, a first-kind use at such a type: `=`, `<>`, and `Equals` with a `T`-typed argument.
  3. The stdlib equality search (`Contains`, `Remove`, `IndexOf`, `LastIndexOf`, `Find`, the span family, `x in xs`) at such an element type is a compile error. The owning collections included.
  4. Regression rows: `new Set<Point3>()`, `new Dictionary<Point3, int>()`, `Counter<Point3>`, `Eq<Point3>(a, b)`, and `Array<Point3>.Contains`, each rejected. `new Set<Point>()` holding a `Point3` value and comparing by `Equals(Point)` is admitted and runs.

---

## 8. Corpus cases to write (after landing)

- `s3_6_*` reject: `new Set<Point3>()` at a `Point3` deriving from a `Point` with `Equals(other: Point)` and `override GetHashCode()`.
- `s3_6_*` reject: `Dictionary<Point3, int>`.
- `s3_6_*` run: `Set<Point>` holding two `Point3` values with equal `X`, `Count` is 1.
- `s9_2_*` reject: `Eq<T>(a: T, b: T): bool => a = b` called at `Point3`.
- `s9_2_*` run: the same `Eq` at `Point` with `Point3` arguments bound at `Point`.
- `l4_*` reject: `Array<Point3>.Contains(p)`; accept: the predicate form `Any`.
- `s3_6_*` reject: a class with `Equals(other: C?)` only, as a `Set<C>` element.

---

## 9. Declined alternatives (one `declined-at-proposal` row each)

1. **A, the inherited own-typed member** (#5716 reading 1). It is the ranking the v0.330 `rejected` row declined and 0155's E2 row declined again, relocated to §9.2, and the issue's grounds (implementation state, cost) are not evidence against those reasons. At a subclass overriding `Equals(Object?)`, it compares by the base's non-virtual `Equals(Point)` over that override without the author's choosing. Through a type parameter it admits what `d1 = d2` refuses. *Reopening bar:* a reopening of the v0.330 row with evidence against its stated reasons.
2. **A′, the nearest `Equals` up the chain.** It ranks overloads by declaration depth, a rule §22.6 does not have, and the same values compare differently in a `Set<Point>` and a `Set<Point3>`. *Reopening bar:* none stated.
3. **C, the `Object` slot.** An un-overridden slot is identity, so a `Set<Point3>` would keep apart the points a `Set<Point>` merges. It is the split 0155 declined for `=` (E4). *Reopening bar:* a redesign that binds the hash containers to the slot as well.
4. **D, a subclass's own `Equals(other: Point3)`.** It is an overload across the inheritance boundary, which §12.2 refuses for every member and which v0.330 kept refusing for `Equals(T)`. *Reopening bar:* a reopening of §12.2's rule against overloads across the inheritance boundary.

---

## 10. Self-critique (adversarial pass)

1. **Is B a ruling rather than a design change?** For a user generic, §12.2 plus §9.2 arguably already reject: the use binds no member. But §9.2's closed list of errors does not name the ambiguity, and §3.6 states every class hashable, which admits `Set<Point3>` under §3.6 rule 2's stdlib exemption. Removing a stated admission is a design change (process §2), hence this proposal.
2. **Does (d) reach interface types correctly?** It is conditional on the ambiguity. An interface with an exact `Equals(I)` (`IIdent: IEquatable<IIdent>`), or with no non-`Object` `Equals`, is unaffected. A sub-interface of `IEquatable<IBase>` is affected, as the subclass is. There are zero sites.
3. **Records, enums, choices.** §9.2 maps their first kind to a declared member by kind, not by a call, so (d) never reaches them. A record's `Equals(T)` is exact.
4. **Nullable and inline-array forms.** `Point3?`, `[N]Point3` and `[N]Point3?` follow through clause (c). A record cannot have a class component and stay auto-hashable (§9.2's admissible component types), so no record flips.
5. **The second kind at a class.** `x.Equals(o)` with `o` of any other type reaches `Equals(Object?)` at a class whose first kind is ambiguous, as the same call does. B leaves it alone. One edge is not decided here, because no text question arises: an argument whose type is another type parameter substituted with `Point3` is read at the body's own typing, `Object`'s surface (§9.2: "type-checks once against that surface").
6. **Constraints.** `where T: Point` does not imply hashable. §14.2 says nothing is inferred from a constraint, so a `Set<T>` in such a body is decided by substitution under §3.6 rule 2, and `Outer<Point3>` is rejected where it forms `Set<Point3>`. That is the existing chain, with (d) as one more reason.
7. **Matrix.** B only removes admissions. Its remedies use positions the borrow-flow matrix already decides: an upcast (R2/C4), a borrowing-collection store (R2/C3), and a retained comparer, which stdlib §4 already makes a borrow. No cell moves (lane md).
8. **History narration (process §7).** The normative sentences state the rule as it now stands. The options, the issue's recommendation and the 0155 lineage are recorded here and in the rationale.
9. **The hash in option A's column (critic).** The first draft said A would compare by `Equals(Point)` and hash by a `Z`-including `Point3` override. That program already breaks §8.3's hash rule against the inherited `Equals(Point)`, and the same mix arises in the adopted remedy, a `Set<Point>` holding such `Point3` values, so it does not separate A from B. The ground against A is the E2 ground (the base's equality over the override, unchosen) and the §12.2 consistency ground (a type parameter would admit what `d1 = d2` refuses), which this revision states.
10. **Over-rejection at `C?` (critic).** Clause (d) makes a class whose only own-typed `Equals` is declared at `C?` not hashable. That follows the text: §12.2 names that shape as §22.6's ambiguity, and stdlib §2.9 tells authors to declare at `T`, not `T?`, for that reason. §8.3's pairing paragraph is amended (§5 item 7a) so that it no longer says a `Set<C>` compares by such a member.
