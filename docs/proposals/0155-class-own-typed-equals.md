# Proposal 0155 — A class's own-typed `Equals`: the hash pairing, and `=` at a subclass operand

| | |
|---|---|
| **Status** | graduated-v0.332 — superseded — graduated v0.332 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": options H2 and E1 — a class's non-`override` own-typed `Equals` obliges the class's own `GetHashCode` override, and `=` stays the call `a.Equals(b)`, a present operand read at its stripped type; critique round 1 applied, 4 blocking and 8 non-blocking findings resolved, shared with the #5666, #5671 and #5695 rulings; that round replaced the drafted operator tie-break (E2), the ranking v0.330 declined, with the issue's own reading, decided the sealed-base question (only the class's own hash override discharges the obligation), and counted nineteen flipping tests for #5695; landed with the v0.332 changelog entry) |
| **Tracking issue** | [#5668](https://github.com/s8lap/bezel-lang/issues/5668) (`spec-question`, `needs-owner-decision`: the compiler-enforced `GetHashCode` pairing), [#5670](https://github.com/s8lap/bezel-lang/issues/5670) (`spec-question`, `needs-owner-decision`: `=` on a class with an own-typed `Equals`); one `spec-tracking` compiler tracker is filed at landing (§7) |
| **Target module(s)** | **spec** — §8.3 (two sentences appended to the pairing paragraph), §8.4 (two sentences), Appendix C.1 (one clause); **stdlib** — §2.9 (two sentences rewritten); **rationale** — §8.1 (one paragraph); **code-generation reference** — §7.4 (the existing `Object`-member pair updated); **`DECLINED.md`** — six `declined-at-proposal` rows |
| **Written against** | spec/stdlib **v0.331** (HEAD `2679585`). Section numbers are v0.331's. |
| **Origin** | Both questions were filed by the v0.330 landing of [#5664](https://github.com/s8lap/bezel-lang/issues/5664), which admitted a class's non-`override` `Equals(other: T)` beside the inherited `Equals(Object?)` (spec §12.2) and named the two consequences it did not decide (changelog v0.330, *Questions filed*). The owner directed on 2026-10-08: "Please resolve and decide all known spec related issues. Do proposals for issues that need them." This one needs a proposal because it adds a rejection (#5668). #5670 is decided with it, because its options and declines share the pairing's vocabulary. |

*Reference convention: unprefixed §-references are language-spec references at v0.331; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

*Critic note (v0.332 critic, round 1).* The lane's first draft adopted, for #5670, a tie-break for `=` alone (option E2 below). The critic refuted it (§3, §10.1): it is the ranking v0.330 declined, relocated from §22.6 to §8.4, and the evidence offered for it is that decline's own remedy. The decision is now the issue's own recommendation, reading (a): `=` stays the call, with the stripping sentence. This note is the proposal's history; the normative text carries none of it.

---

## 0. Summary

Since v0.330 a class may declare `Equals(other: Point)` beside `Object`'s `Equals(Object?)`, and `=`, `a.Equals(b)` at `Point` operands, a comparer-less `Set<Point>` or `Dictionary<Point, V>`, and every use through a type parameter at `Point` compare by it. Two questions follow:

1. **The pairing (#5668).** §8.3's compiler-enforced rule reads `override Equals` only, so a class with an own-typed `Equals` and no `GetHashCode` compiles and a `Set<Point>` keeps two equal points. **Decision:** the rule reads a class's non-`override` `Equals` taking its own type (`C` or `C?`) as it reads an override — the v0.327 enum/choice precedent ([#5598](https://github.com/s8lap/bezel-lang/issues/5598)) carried to classes. The obligation is the declaring class's own: an inherited `GetHashCode` override does not discharge it, so under a base's `sealed override GetHashCode()` a class declares no `Equals` of either kind.
2. **`=` at a subclass operand, and at a nullable one (#5670).** **Decision:** `=` and `<>` stay the call `a.Equals(b)` (§8.4), so `p = d` at a subclass-typed `d` stays §22.6's ambiguity, as `p.Equals(d)` is and as v0.330's §12.2 already states. §8.4 gains one sentence: the null-safe wrapper reads a present operand at its stripped type, so `p = q` at `q: Point?` and `p = q!` select the same member.

No new syntax, no new kernel clause, no change to §22.6 or §12.2. Newly admitted: nothing.

---

## 1. The problem

### 1.1 #5668: the pairing reads only `override`

§8.3, v0.331:

> `GetHashCode` must satisfy: if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`.

> A type that overrides `Equals` must also override `GetHashCode`. The compiler enforces this. On an enum or a choice, whose `Object`-slot `Equals` takes its own type and is declared `override` or plain, the two spellings declaring one member ([§10.5], [§35.2]), the rule reads both spellings of both members: a plain `Equals` obliges `GetHashCode`, and a plain `GetHashCode` discharges the obligation.

The hash rule reads every `a.Equals(b)`. The enforced rule reads `override Equals`. A class's own-typed `Equals` is not an override (§12.2), so this compiles and misbehaves:

```bezel
public class Point: IEquatable<Point>
  public readonly property X: int;
  public constructor(x: int) X := x; end constructor;
  public method Equals(other: Point): bool => X = other.X;
end Point;
// var s := new Set<Point>(); s.Add(new Point(1)); s.Add(new Point(1));
// s.Count is 2: Equals(Point) says equal, the identity hash says different
```

Stdlib §2.9 and the code-generation reference already tell the author to override `GetHashCode()`; nothing checks it. The divergence is exactly the one §25.2's co-declaration rule exists to prevent for records, enums and choices ("`a = b` true while a `Set<T>` treats them as distinct"), and v0.327 (#5598) closed it for an enum's or a choice's plain `Equals`.

### 1.2 #5670: `=` at a subclass operand, and at a nullable one

§8.4: "`a = b` invokes `a.Equals(b)`"; "For classes, `=` always invokes `Equals`." §12.2 (v0.330):

> an argument that converts to both parameter types and matches neither exactly — one of a class deriving from `Point`, or a `Point` where the overload is declared at `Point?` — is §22.6's ambiguity, remedied by binding the argument at the intended parameter's type first. `a = b` and `a <> b` ([§8.4]) and a use through a type parameter ([§9.2]) are such calls.

- **Shape 1 — a subclass operand.** `p = d` at `d: Point3` is a compile error, as `p.Equals(d)` is. v0.330's text decides this (§12.2's last quoted sentence) and v0.330's `rejected` row *Ranking a class's own-typed `Equals` above the inherited `Equals(Object?)` at an argument that converts to both* declines ranking the own-typed member at such an argument ("A subclass-typed argument stays §22.6's ambiguity, remedied by binding at the intended type"). What v0.330 filed as open was a different design: whether `=` should **bind a fixed member** rather than resolve the call (changelog v0.330: "Whether `=` on classes should instead bind a fixed member is filed as a question").
- **Shape 2 — a nullable operand.** §8.4's wrapper decides the absent cases first and "only when both operands are present dispatch[es] … to `Equals`". No text says whether the present operand is then read at `Point` or at its declared `Point?`. At `Point?`, `Equals(Point)` is inapplicable and `Equals(Object?)` — identity unless overridden — is selected, so `p = q` and `p = q!` could answer differently: a silent wrong answer.

---

## 2. Options

**For #5668:**

| | Option | Effect |
|---|---|---|
| **H1** | Status quo: the author's obligation, unchecked (#5668 reading 2) | `Set<C>` silently keeps equal elements apart |
| **H2** | **Extend the enforced pairing to a class's non-`override` own-typed `Equals`, discharged by the class's own `GetHashCode` override — adopted** | the class declares `override GetHashCode()`, or the declaration is rejected |
| **H3** | A warning at the declaration instead | the divergence stays legal |
| **H4** | H2, with an inherited non-`Object` `GetHashCode` override also discharging it (the lane's sealed-base question) | a subclass may lean on a base's hash written for another `Equals`; under a sealed base hash an `Equals` becomes declarable |

**For #5670 (every option takes the stripping sentence for shape 2, which no option disputes):**

| | Option | `p = d` at `d: Point3` | `d1 = d2` at two `Point3`s, `Point3` overriding `Equals(Object?)` | `c = u` at `class C` with `Equals(other: U)` (not own-typed) |
|---|---|---|---|---|
| **E1** | **`=` is the call `a.Equals(b)` (status quo; #5670 reading (a)) — adopted** | compile error (§22.6), as `p.Equals(d)` | compile error; the author binds at `Point` or at `Object?` | `Equals(U)` (the call) |
| **E2** | `=` is the call, with one tie-break: at a tie between `Object`'s `Equals(Object?)` and one own-typed `Equals`, the own-typed member (#5670 reading (b), narrow form) | `Equals(Point)` | `Point`'s non-virtual `Equals(Point)`, bypassing `Point3`'s override | `Equals(U)` |
| **E3** | `=` binds one fixed member: the own-typed `Equals` where the class declares one and the operand converts to it, else `Equals(Object?)` (#5670 reading (b) as filed) | `Equals(Point)` | `Equals(Point)` | `Equals(Object?)` (changes) |
| **E4** | `=` always binds `Equals(Object?)`, the `Object` slot (#5670 reading (c)) | `Equals(Object?)` | `Point3`'s override | `Equals(Object?)` (changes) |

---

## 3. Evaluation

**Soundness.** None of these options touches ownership or a borrow; the stake is a silent wrong answer. H1 keeps one (`Set<C>` dedup). H4 admits one: an inherited hash answers for an `Equals` its author never saw. E4 creates one: `p = q` at two `Point`s and `Set<Point>.Contains(q)` would compare by different members, because §9.2 binds the hash containers to the own-typed member — and it would reverse v0.330's stated answer for `=` at `Point` operands. E2 and E3 pick a member the author did not choose at `d1 = d2` (the table's third column). E1 has no silent answer: every operand it admits reaches the member the call reaches, and every operand it refuses is refused with a remedy.

**Simplicity.** H2 is one clause in an existing rule and one predicate in an existing check (`checkEqualsHashCodePairing`). E1 needs no new rule: §12.2 already lists `a = b` among the ambiguous calls, and the §8.4 sentence only restates it where a reader of `=` looks. E2 defines an operator-only tie-break with three conditions (a tie, exactly one own-typed overload, `C` or `C?`). E3 needs a member-selection rule of its own with a fallback. E4 is short but needs a second rule for the containers or accepts the split.

**Consistency.** H2 is v0.327's enum/choice decision carried to the third kind that can now declare a plain own-typed `Equals`. E1 keeps `=` and `a.Equals(b)` the same call at every operand, the one property §8.4 states ("`a = b` invokes `a.Equals(b)`"). Its asymmetry — `p = d` refused, `d = p` admitted (the argument `p` is exact at `Equals(Point)`) — is the call's own asymmetry, which §12.2 already states for `p.Equals(d)` and `d.Equals(p)`; E1 adds none. E2 removes that asymmetry for `=` only, so `=` and `.Equals` would differ in what compiles. E3 departs from the call at a non-own-typed overload (`c = 5` on `class C: IEquatable<int>`).

**Relation to recorded declines.** v0.330's `rejected` row declines *ranking a class's own-typed `Equals` above the inherited `Equals(Object?)` at an argument that converts to both*, naming the subclass-typed argument explicitly, on two grounds: §22.6 ranks no implicit conversion above another, and a tie-break keyed on one member name would be the only exception. E2 is that ranking, applied at `=` rather than at the call, and it is keyed on the member name. A reopening needs *new evidence against the stated reasons* (process §2). The evidence the first draft offered — a `Set<Point>` holding `d`, `=` over `T`-typed operands at `Point`, and `d = p` all compare by `Equals(Point)` — was stated in the same v0.330 entry (changelog v0.330, "`=` and the hash containers use the own-typed `Equals`"), and it is not against the reasons: in each case the subclass value is already bound at `Point` (a `Set<Point>` element, a `T = Point` argument, an exact argument), which is the row's own remedy. E1 is the reading v0.330 wrote; E3 is the question v0.330 actually filed, and it is declined on its merits below.

**User-facing cost.** E1 keeps a compile error at `p = d` with the remedy in the diagnostic (`var pp: Point := d;`). That error is static, local and never a wrong answer. H2 costs one `override GetHashCode()` line in a class that declares typed equality; stdlib §2.9 already asks for it. H2 without H4 makes an `Equals` undeclarable under a sealed base hash; that is the existing rule's consequence for an `Equals` override, and a base that wants subclasses to define equality leaves its hash open.

**Migration reach** (measured by scan of `tests/`, the ten documents, `tests/conformance/` and `tests/conformance/rules/`; §6).

- H2 newly rejects five accepting programs: four in `tests/` and one rule-corpus case. H2's no-inherited-discharge clause rejects nothing new: the compiler already reads the class's own members only, and no class in `tests/` or `docs/` declares `sealed override GetHashCode()`.
- E1 and the stripping sentence move no verdict in `tests/` or either corpus.

---

## 4. Decision

**H2 and E1**, with the stripping sentence. Under the owner's 2026-10-08 delegation this is accepted as a delegated ruling and graduates in v0.332.

- A class that declares a non-`override` `Equals` whose one parameter is the class's own type, `C` or `C?`, must override `GetHashCode()`. `C` is the declaring class with its own type parameters. The obligation and its diagnostic are §8.3's, the same as an `Equals` override's, and only the class's own `GetHashCode` override discharges either. An `Equals` taking any other type obliges nothing (`Equals(other: int)` on `class C: IEquatable<int>`, `tests/codegen/builtin_interface_value_1115_test.cpp`).
- `a = b` and `a <> b` on a class invoke the member the call `a.Equals(b)` selects (§22.6); an operand at which the call is ambiguous is the same error at the operator. A present operand of §8.4's null-safe wrapper is read at its type with nullability stripped.

---

## 5. The exact normative edits (▲)

1. **Spec §8.3**, appended to the pairing paragraph after "…carries no such obligation ([§35.7])."
   ▲ "On a class the rule reads one more member: a non-`override` `Equals` taking the declaring class's own type, at `C` or `C?` (`C` with its own type parameters — `Equals(other: Point)` beside the inherited `Equals(Object?)`, [§12.2]), obliges the `GetHashCode` override as an `Equals` override does, because `=` and `<>` ([§8.4]), a `Set<C>` or `Dictionary<C, V>` built without a comparer, and every use through a type parameter at `C` ([§9.2]) compare by it. An `Equals` taking any other type (`Equals(other: int)` for `IEquatable<int>`) obliges nothing. On a class the obligation is the declaring class's own: a `GetHashCode` override it inherits does not discharge it, so a class whose base declares `sealed override GetHashCode()` declares neither an `Equals` override nor an own-typed `Equals`."
2. **Spec §8.4**, first paragraph, after "…therefore does not reach `=` and `<>`."
   ▲ "A present operand is read, where the operators dispatch, at its type with nullability stripped, so `p = q` at `q: Point?` and `p = q!` select the same member."
3. **Spec §8.4**, second paragraph, after "…classes customize equality by overriding `Equals`." The sentence itself is unchanged; rule-corpus cases `s8_4_0002` and `s8_4_0003` quote it.
   ▲ "On a class the member invoked is the one the call `a.Equals(b)` selects ([§22.6]), so an operand at which that call is ambiguous is the same compile error at `=` and `<>`: `p = d` at `d: Point3`, a subclass of a `Point` that declares `Equals(other: Point)`, as `p.Equals(d)` is ([§12.2]), remedied by binding `d` at `Point` first."
4. **Spec §12.2**: unchanged. Its "`a = b` and `a <> b` ([§8.4]) and a use through a type parameter ([§9.2]) are such calls" is the rule E1 keeps.
5. **Spec Appendix C.1**, the `GetHashCode` bullet, after "…except on a choice owning at every instantiation ([§8.3])."
   ▲ "A class's non-`override` `Equals` taking the class's own type, `C` or `C?`, requires the `GetHashCode` override as an `Equals` override does, declared by the class itself ([§8.3])."
6. **Stdlib §2.9**: the text from "and whose hash is `GetHashCode()`" through "…to agree with its `Equals(T)` (the compiler's pairing check reads an `override Equals` only), and overrides" becomes
   ▲ "and whose hash is `GetHashCode()`; an `Object`-typed receiver or argument reaches `Equals(Object?)`, and `a = b` reads a present `T?` operand at `T` (spec §8.4), where the call `a.Equals(b)` reaches `Equals(Object?)`. Such a class overrides `GetHashCode()` to agree with its `Equals(T)`, as spec §8.3's hash rule requires and its compiler-enforced pairing checks, and overrides".
7. **Rationale §8.1**: a v0.332 paragraph after the v0.330 #5664 paragraph (the JSON carries its text).
8. **Code-generation reference §7.4**: the existing `Object`-member pair. The rejected block's `Point` gains its `GetHashCode` override, a `Tag` class without one is added as a rejected line, and `var eq := p = sub;` is marked as the same ambiguity; the accepted block gains `var eq := p = pp;`. The note now says that `p = sub` is ambiguous as `p.Equals(sub)` is, and that the plain own-typed `Equals` obliges `override GetHashCode()`.
9. **`DECLINED.md`**: six `declined-at-proposal` rows (§9).

---

## 6. Measurement

Scan: `scratch/v0332/U4work/scan_eq2.py` finds every `class C … end C` that declares a non-`override`, non-`static` `Equals` whose parameter is `C`, `C<…>` or `C?`. Every hit was then checked by hand. The scan covered `tests/`, `docs/` and the corpora.

| Site | Today | Under H2 | Note |
|---|---|---|---|
| `tests/codegen/primitive_equals_constraint_863_test.cpp` `user_class_equals_via_constraint` (`Pt`) | run, 10 | **reject** | fixture gains `override GetHashCode()` |
| `tests/sema/constraint_iequatable_other_self_4863_test.cpp`: the shared prelude's `C: IEquatable<C>`, three accept controls (`class_conforming_to_other_other_constraint_accepted_control_spec_14_2`, `identity_iequatable_self_constraint_accepted_control_spec_14_2`, `unconstrained_generic_accepted_control_spec_14_2`) | accept | **reject** | prelude gains `override GetHashCode()`; `C2`'s `Equals(Other)` is not own-typed and obliges nothing |
| `tests/sema/generic_interface_type_arg_conversion_4892_test.cpp` `same_instantiation_generic_class_base_chain_accepted_and_run` (`C`, line 320) | run | **reject** | fixture gains `override GetHashCode()` |
| `tests/lit/sema/sema_576_icomparable_iequatable__class_implements_iequatable_resolves_2_9.bzl` (`Point`) | accept, NODIAG | **reject** | fixture gains `override GetHashCode()` |
| rule corpus `l2_9_0003` (`Point`), banked `pass` | run, `True` | **reject** | the case's program is amended by the tracker; its rule (a class implements `IEquatable<T>` by hand) is unchanged |
| `tests/sema/variant_generic_declared_iface_reject_1940_test.cpp` (`Box`), `generic_interface_type_arg_conversion_4892` line 57 (`D`, `C`), `constraint_receiver_sigsub_5469_test.cpp` (`CS`), 4863 case 1 | reject | reject (one more diagnostic) | each asserts its own diagnostic, which still fires |
| `interface_witness_param_type_5097`, `generic_interface_multi_instantiation_dispatch_5115`, `operator_groups_746` (`K?`) | accept | accept | already override `GetHashCode()` |
| docs: code-generation reference §7.4's rejected block (`Point`) | — | edited (§5 item 8) | the accepted block already overrides |
| stdlib `String` | — | unchanged | declares `override GetHashCode()` |
| `sealed override GetHashCode` on a class (H4's subject) | — | — | zero sites in `tests/` and `docs/` (the two hits are enum/choice reject fixtures, `sema_enum_choice_override_3837__*`) |

E1 and the stripping sentence: a subclass-typed or nullable operand of `=` on a class with an own-typed `Equals` was searched for in `tests/` and in both corpora. There are **zero sites**. The v0.330 corpus list named a `p = d` reject case; it was never written, and under E1 it is still a reject (§8).

---

## 7. Compiler facts and the tracker

Source-read at `2679585`; no build was run in this lane.

- `src/sema/TypeResolution_Checks.cpp:4185–4214`: `checkEqualsHashCodePairing` collects `override` members only (`if (!ms || ms->isOperator || !hasMod(ms, "override")) continue;`), and only among the type's own `members`, so an inherited `GetHashCode` override already does not discharge the obligation (H2's last clause is the compiler's present reading). The header comment says why an `IEquatable<T>` `Equals(T)` is exempt: "an interface *implementation*, not an `override`". It reads neither v0.327's enum/choice plain `Equals` nor a class's own-typed one.
- `src/codegen/LlvmBackend_Calls.cpp:1651–1677`: `classEqualsOverride`, used by the collection-membership lowering, returns the first single-parameter `Equals` with a pointer-typed parameter found up the class chain. How the binary `=` resolves at a subclass operand and at a nullable operand is unprobed. The v0.330 tracker [#5669](https://github.com/s8lap/bezel-lang/issues/5669) carries the overload-set resolution probe, and this tracker extends it.
- **Tracker to file (spec-ahead-of-impl, §8.3 / §8.4, v0.332):**
  - (1) `checkEqualsHashCodePairing` reads a class's non-`override` `Equals` whose parameter is the class's own type (`C` or `C?`) as an `Equals` override. The diagnostic gets the class spelling ("a class that declares `Equals(other: C)` must also override `GetHashCode` (§8.3)"), and the four `tests/` fixtures and the lit fixture in §6 gain `override GetHashCode()`.
  - (2) `=` and `<>` on class operands resolve as the call `a.Equals(b)`. Probes: `p = d` (expect §22.6's ambiguity, as `p.Equals(d)`), `d = p` (expect `Equals(Point)`), `p = q` at `q: Point?` and `p = q!` (expect the same member, `Equals(Point)`). Regression rows: whichever probe disagrees.
  - (3) The membership lowering's `classEqualsOverride` must not select `Equals(Point)` for an `Object`-typed query.

## 8. Corpus cases to write (after landing)

- `s8_3_*` reject: class with `Equals(other: Point)` and no `GetHashCode` override.
- `s8_3_*` accept: the same class with `override GetHashCode()`.
- `s8_3_*` accept: `class C: IEquatable<int>` with `Equals(other: int)` and no `GetHashCode` override.
- `s8_3_*` reject: a subclass declaring an own-typed `Equals` under a base's `sealed override GetHashCode()`.
- `s8_4_*` reject: `p = d` at a subclass operand (ambiguous, as the call).
- `s8_4_*` run: `p = q` at `q: Point?` and `p = q!` agree.

---

## 9. Declined alternatives (one `declined-at-proposal` row each)

1. **H1, the status quo.** It leaves the silent `Set<C>` divergence that §25.2's co-declaration rule and v0.327's enum/choice ruling were each adopted to close. *Reopening bar:* none stated.
2. **H3, a warning.** The divergence is a wrong answer, not a style question, and v0.327 chose a rejection for the same shape on enums and choices. *Reopening bar:* evidence of a class whose own-typed `Equals` must stay without a matching hash.
3. **H4, an inherited-hash discharge.** An inherited hash was written for the base's `Equals`, not the one the class declares; the pairing exists so each `Equals` is declared beside a hash that answers for it. A base that seals its hash has fixed its subclasses' equality with it. *Reopening bar:* a class hierarchy that must refine equality under a sealed hash.
4. **E2, an operator tie-break.** It is the ranking the v0.330 `rejected` row declined, relocated to §8.4; its evidence is that row's own remedy (§3); at two subclass operands it takes the base's non-virtual `Equals(Point)` over a subclass's `Equals(Object?)` override without the author's choosing; and `=` and `.Equals` would differ in what compiles. *Reopening bar:* a reopening of the v0.330 row with evidence against its stated reasons.
5. **E3, a fixed member.** It splits `=` from the call where the call is unambiguous (`c = 5` against `c.Equals(5)` on `class C: IEquatable<int>`), and it needs a member-selection rule of its own beside §22.6. *Reopening bar:* none stated.
6. **E4, the `Object` slot.** It splits `=` from `Set<C>`, `Dictionary<C, V>` and `=` through a type parameter, which §9.2 binds to the own-typed member, and it reverses v0.330's answer for `=` at `C` operands. *Reopening bar:* a redesign that binds the hash containers to the slot as well.

---

## 10. Self-critique (adversarial pass)

1. **Is E1 the asymmetry the first draft objected to?** `p = d` is refused and `d = p` admitted. That is §12.2's stated asymmetry of the call (`p.Equals(d)` against `d.Equals(p)`), which v0.330 wrote; E1 adds no asymmetry of its own, and every admitted `=` reaches the member its call reaches.
2. **The stripping sentence at an overload declared at `C?`.** With only `Equals(other: Point?)` declared, `p = q` at `q: Point?` is read at `Point`, where the call is §12.2's ambiguity (a `Point` argument at a `Point?`-declared overload). Without stripping, the operand at `Point?` would be exact. Stripping is kept: it makes `=` independent of an operand's nullability, which is the wrapper's purpose, and v0.330's §12.2 and stdlib §2.9 already steer the declaration to `T`, not `T?`.
3. **An interface-declared typed `Equals`** (`e = d` at `e: IEquatable<Point>`) is the call, as everywhere under E1. Nothing to add.
4. **A sealed base hash.** Decided (H2's last clause, H4 declined). It is the compiler's present per-class reading and rejects nothing in `tests/` or `docs/`.
5. **Surfaced, not decided here: §9.2 at a subclass instantiation.** At `S = Point3` (a subclass of a `Point` declaring `Equals(other: Point)`), §9.2's first kind "reaches the member the same use on an S-typed operand binds", and that use (`x.Equals(y)` or `x = y` at two `Point3`s) is §22.6's ambiguity under v0.330's text, before and after this proposal. §9.2's "no member" clause does not name an ambiguity, and §3.6 counts `Point3` hashable, so whether `Set<Point3>` compares by `Equals(Point)` or is rejected at the instantiation is undecided. It belongs to the T-path's kind mapping, not to `=`, and E2 would not have settled it (the `.Equals` use stays ambiguous under E2). Filed as a `spec-question` (lane md).
6. **History narration (process §7).** The normative sentences state the rule as it now stands; the options, the critic round and the "v0.330 filed it" story are recorded here and in the rationale.
