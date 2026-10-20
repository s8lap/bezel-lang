# Proposal 0192 — An enum that declares its own `Equals` has no `=` until it overloads it: one equality rule for every value kind

| | |
|---|---|
| **Status** | graduated-v0.339 — superseded — graduated v0.339 (accepted 2026-10-20 as a delegated ruling under the owner's 2026-10-08 direction ("Please resolve and decide all known spec related issues. Do proposals for issues that need them.") and the 2026-10-19 "Proceed with drafting", [#5929](https://github.com/s8lap/bezel-lang/issues/5929), option C — an enum declaring its own-typed `Equals` and no homogeneous `=`/`<>` overload has no `=` or `<>`, the use being a compile error at the expression until the overloads are declared, as §8.4 states for a record and a value-like choice; every other consumer — direct calls, hash containers, interpolation, boxed dispatch, §9.2's kinds through a type parameter, and a generated record, choice or inline-array comparison over such an enum component — reaches the declared pair; a constant pattern over an enum scrutinee matches by §10.2's underlying-integer identity on every enum; critique round 1 applied (the generated component comparisons stated in §9.2 and §35.7; the dispatch option declined as the enum analogue of proposal 0178's option C, its reasons applied by analogy, not as that row; *Ordering*'s scoping made explicit); landed with the v0.339 changelog entry) |
| **Tracking issue** | [#5929](https://github.com/s8lap/bezel-lang/issues/5929). One `spec-tracking` compiler tracker is filed at landing (§7). |
| **Target module(s)** | **spec**: §8.4 (one sentence widened), §10.5 (the replacement paragraph gains two sentences; *Ordering*'s two equality sentences scoped), §10.6 (one sentence). **rationale**: §9.3 (one dated paragraph). **code-generation reference**: §2's *Equality operator* row (one clause); §7.3 gains one *Looks right, is rejected* pair. **`DECLINED.md`**: two `declined-at-proposal` rows. No stdlib edit, no grammar change, no new keyword. |
| **Written against** | spec/stdlib **v0.338** at `d09319a91` (main). Section numbers are v0.338's. |
| **Origin** | Proposal 0178 (v0.337) made declared equality user equality on a record and a value-like choice and recorded in its §3 and §10 that "the one asymmetry B keeps is the enum's `=`", filing it as [#5929](https://github.com/s8lap/bezel-lang/issues/5929) at the landing. The 2026-10-19 ruling lane `L3` classed the issue NEEDS-SPEC-CHANGE: §8.4's general sentence and §10.5's "every consumer" send `=` to the declared `Equals`, §10.5's *Ordering* says the underlying integer "decides equality", and §9.2's *Through a type parameter* already routes `=` over `T` at an enum to the declared member. It needs a proposal because every reading that resolves the contradiction either narrows §10.5's stated admission of `=` on every enum (this proposal) or adds a dispatch rule a recorded decline refused (0178 option C). |

*Reference convention: unprefixed §-references are language-spec references at v0.338; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API. The former name of the language is not used anywhere in this document.*

---

## 0. Summary

Since v0.337 a record or a value-like choice that declares its own-typed `Equals` has no `=`: §8.4 says so ("A record or a value-like choice that declares its own `Equals` and no homogeneous overload has no `=` at all — the use is §9.2's and §35.7's compile error, not a dispatch to `Equals`"), and the author who wants the operator writes the two overload lines §25.2's co-declaration rule governs. The enum was left on its own model, and three of its sentences disagree about what `Color.Red = Color.Blue` means when `Color` declares `Equals(other: Color)`: the declared `Equals` (§8.4's general rule; §10.5's "replaces the compiler-generated default for every consumer"; §9.2's *Through a type parameter*, under which `a = b` over `T` at `Color` reaches the declared member), or the underlying integers (§10.5's *Ordering*: "§10.2's identity of a value with its underlying integer decides equality"). The compiler lowers the concrete `=` to an integer compare and the type-parameter `=` to the declared `Equals` — one operator with two definitions, which is exactly the divergence 0178 removed from the choice.

**Decision.** The enum joins the rule the record and the value-like choice already follow. An enum that declares its own-typed `Equals` and no homogeneous `=`/`<>` overload has no `=` or `<>` at all: a use at the enum's static type is a compile error at the expression, remedied by declaring the overloads (§25.2 obliges nothing further, the pair being already declared) or by calling `.Equals`. Every other consumer is unchanged and already stated by §10.5: direct calls, hash containers, string interpolation, boxed dispatch, and §9.2's three kinds through a type parameter reach the declared members. `case` over an enum scrutinee is untouched: a constant pattern matches by §10.2's underlying-integer identity on every enum, whatever members it declares, and §19.6's enum coverage reads the same identity. Inside the declared `Equals` the underlying values compare as `int(this) = int(other)`, the spelling every site in the repository but two already uses.

Newly rejected (§6): `=` or `<>` at the static type of an enum declaring its own-typed `Equals` without the overloads — two gtest bodies (`this = other` inside the `Equals` itself, which under the issue's recommended reading would have called itself forever), zero lit, zero rule-corpus, zero documentation sites. Newly admitted: nothing. Newly defined at run time: nothing — the one answer that changed between the readings is now a compile error.

---

## 1. The problem

### 1.1 Three sentences, two answers

| Passage | What it says at v0.338 | Reading |
|---|---|---|
| §8.4, first paragraph | "`a = b` invokes `a.Equals(b)`, with a null-safe wrapper … dispatch to the overloaded operator where one is declared (§25.2), or to `Equals` otherwise" | the declared `Equals` |
| §10.5, replacement paragraph | a declared `Equals`, `GetHashCode` or `ToString` "**replaces the compiler-generated default for every consumer**: direct calls, hash containers …, string interpolation …, and virtual dispatch through a boxed reference" — `=` is not in the list, and "every consumer" covers it | the declared `Equals` |
| §9.2, *Through a type parameter* | "`=` and `<>` over `T`-typed operands … are §8.4's `Equals` … At a record, an enum or a choice S, the first kind reaches S's own-typed `Equals`, declared or generated" | the declared `Equals` (through `T`) |
| §10.5, *Ordering* | "Its values compare for equality under §8.4, by underlying integer (§8.3) … §10.2's identity of a value with its underlying integer decides equality and aliasing" | the underlying integer |
| §8.4, the v0.337 sentence | "A record or a value-like choice that declares its own `Equals` and no homogeneous overload has no `=` at all" | names two kinds, not the enum |

```bezel
public enum Color
  Red, Green, Blue;
  public method Equals(other: Color): bool => true;   // every value equal to every other
  public method GetHashCode(): int => 0;
end Color;

static class Demo
  public static method Same<T>(a: T, b: T): bool where T: hashable => a = b;   // §9.2's first kind
  public static method Run()
    Console.WriteLine($"{Color.Red = Color.Blue}");       // v0.338: §8.4 says True, §10.5 Ordering says False; the compiler prints False
    Console.WriteLine($"{Same<Color>(Color.Red, Color.Blue)}");   // v0.338: True (the declared Equals); the compiler prints True
    Console.WriteLine($"{new Set<Color>() is Object}");   // a Set<Color> consults the declared pair (§10.5)
  end Run;                                               // ▲ v0.339: `Color.Red = Color.Blue` is a compile error; Same<Color> is True
end Demo;
```

### 1.2 What the compiler does today

- **The concrete `=`** on enum operands lowers to the integer compare whatever the body declares: `emitValueEquality` (`src/codegen/LlvmBackend_Layout.cpp:8516`, "Primitives and enums compare by value").
- **The declared `Equals`** is wired into hash containers and the box's `Equals` thunk (`emitEnumEquals`, `src/codegen/LlvmBackend_Layout.cpp:49337`; the box arm at `:40168` and `:48780`), and a direct `.Equals` call binds it. So `Set<Color>` and `=` disagree on the program above — §25.2's own named hazard ("`a = b` true while a `Set<T>` treats them as distinct", rationale §7.6).
- **Sema** has no rule at `=` on an enum operand keyed on a declared `Equals`; §9.2's use-site error (`checkHashContainerKeys` and the equality-use check in `src/sema/TypeResolution_Checks.cpp`) is keyed on records and choices.
- **Two gtest rows** (`tests/sema/override_equals_parameter_type_5610_test.cpp:112` and `:263`) declare `Equals(other: Color): bool => this = other` and run `Color.Red.Equals(Color.Red)`, expecting `True`. They pass today only because `=` is the intrinsic integer compare; under the issue's recommended reading the body is a self-call.

## 2. Options

- **A — Status quo.** The concrete `=` stays intrinsic beside a replaced `Equals`; the type-parameter `=` reaches the declared member (§9.2). One operator, two definitions. The texts stay as they are, with *Ordering* read as the governing sentence.
- **B — `=` reaches the declared `Equals`** (#5929 reading 1, the issue's recommendation; the `L3` ruling lane's recommendation). §10.5's replacement sentence names `=`/`<>` among the consumers; *Ordering* is scoped to the default; a sentence names `int(this) = int(other)` as the spelling inside the body; the compiler gives the self-call a diagnostic.
- **C — The enum joins the record and the choice** (this proposal). An enum declaring its own-typed `Equals` and no homogeneous overload has no `=`; §8.4's sentence names all three kinds; *Ordering* is scoped to the default; constant patterns are stated to match by §10.2's identity.
- **D — C, keyed on `Equals` or `GetHashCode`**, as §9.2's second disqualifier and §35.7 key the record's and the choice's loss of the generated package.

## 3. Evaluation

**Soundness.** A, as the compiler implements it, lets `=` and a `Set<E>` disagree on one pair of values — the three-surface divergence §25.2 exists to prevent and rationale §7.6 records as the reason a homogeneous overload strips the auto package. B and C both close it. B opens a new hazard in the one place the member is written: `Equals(other: E): bool => this = other` becomes unbounded recursion, and the two repository bodies that declare an enum `Equals` with `=` are both of that shape; B needs a compiler diagnostic for a self-call to be safe. C has no run-time face: the one expression whose answer the readings disputed is a compile error. D is sound too.

**Simplicity.** C adds no rule: it widens one existing sentence by three words ("A record, an enum or a value-like choice …") and scopes *Ordering* to the default it describes. B adds a dispatch rule that exists on no other value kind — §8.4's dispatch of `=` to `Equals` is the class's rule, and 0178 declined extending it to the record and the choice (its option C: "It reverses §9.2's recorded use-site error and §25.2's state-all-three discipline … for a convenience two overload lines spell", `DECLINED.md`'s `declined-at-proposal` row) — plus the self-call diagnostic. Adopting B for the enum would be the analogue of that declined option on the third kind. It is not a reopening of 0178's row — the row names "a record or value-like choice", and 0178 §10 recorded the enum's `=` as "§9.2's recorded posture, not this proposal's", left undecided and filed as #5929 — so the decline does not bind B; its reasons transfer by analogy, which is the ground here: the overload lines are the same two lines on an enum, §25.2 already obliges nothing further once the pair is declared, and B would make the enum the one value kind whose `=` dispatches. (The self-call B creates in `Equals(other: E): bool => this = other` is the trap a class's own `Equals` body already has under §8.4; it weighs against B only because the two repository bodies that use `=` on such an enum are exactly that shape.) D states one rule for the three kinds by the letter of §9.2's disqualifier, but the record's lone-`GetHashCode` strip is a consequence of its *generated package* going (§9.2's second disqualifier is about auto-hashability), and an enum has no generated package: its `=` is intrinsic, and a lone `GetHashCode` on an enum keeps §8.3's invariant trivially (equal values are the same integer). D would reject `=` on three enums in the repository for no hazard; C keys on §8.4's own word, "declares its own `Equals`", on every kind.

**Consistency.** Under C the three value kinds read alike: declaring the own-typed `Equals` is taking over equality; every consumer §10.5 lists reaches the declared members; `=` and `<>` are the author's to declare, and when declared, §25.2's co-declaration rule already requires the pair beside them. §9.2's *Through a type parameter* needs no edit: the first kind at an enum "reaches S's own-typed `Equals`, declared or generated", which is now the only `=` such an enum has, as it is for a record. `case` is unchanged on every enum (§10.6 states it). The one enum-specific sentence that remains, *Ordering*'s "by underlying integer", becomes a description of the default.

**Migration reach** (§6). C rejects two gtest bodies, both inside the declared `Equals`, remedied by the `int(this) = int(other)` spelling the same files' other enums use; zero lit, zero rule-corpus, zero documentation sites; no banked verdict moves.

## 4. Decision

**Option C.** The rules, stated at their homes:

> ▲ **§8.4.** A record, an enum or a value-like choice that declares its own `Equals` and no homogeneous overload has no `=` at all — the use is §9.2's, §10.5's and §35.7's compile error, not a dispatch to `Equals` — while the same use through a type parameter instantiated at that type reaches the declared member (§9.2).

> ▲ **§10.5** (after "It is the only member that fills the slot."). A declared `Equals` also retires the intrinsic `=`: an enum that declares its own-typed `Equals` and no homogeneous `=`/`<>` overload has no `=` or `<>` at all — a use at the enum's static type is a compile error at the expression, as on a record (§8.4, §9.2), remedied by declaring the overloads, which §25.2's co-declaration rule already pairs with the members the enum declares, or by calling `.Equals` — while hash containers, boxed dispatch, a use through a type parameter (§9.2 *Through a type parameter*) and the generated equality of a record, choice or inline array holding the enum as a component (§9.2, §35.7, §15.1.2) reach the declared member. Inside the declared `Equals` the underlying values compare as `int(this) = int(other)`.

> ▲ **§9.2** (the auto-hashable paragraph) and **§35.7** (the generated `=`). "The generated `=` compares each auto-property by its declared type's `=`" gains "(an enum component that has none, declaring its own `Equals` without the overloads, by that `Equals`, §10.5)"; "every corresponding payload compares equal by its type's `=`" gains "(an enum payload that has none by its declared `Equals`, §10.5)". An enum stays an admissible component on every enum (§9.2's list is unchanged), so no record or choice loses its generated package; the component comparison is §10.5's "every consumer" read at the generated member.

> ▲ **§10.5, *Ordering*.** Its values compare for equality under §8.4 — by underlying integer (§8.3) on an enum that declares no `Equals` of its own, by the declared overload on one that declares the operators, and not at all on one that declares its own `Equals` without them (above) — but `<`, `<=`, `>` and `>=` are not intrinsic on an enum … §10.2's identity of a value with its underlying integer decides the generated equality, aliasing and §10.6's constant matching; it supplies no operator.

> ▲ **§10.6.** A constant pattern over an enum scrutinee matches by §10.2's identity of the value with its underlying integer, on every enum, whatever members it declares, and §19.6's coverage reads the same identity; a constant *slot* of enum type inside a destructuring pattern is §19.3's `=` test and takes that operator's verdict.

Consequences, each already the rule on at least one kind:

1. **`=`/`<>` on such an enum are the author's to declare.** Declaring `operator =`/`<>` restores them (§25); the pair §25.2 obliges is already declared. A direct `.Equals(…)` call is admitted throughout, as §10.5 says.
2. **Nothing else about the enum moves.** It stays hashable (§3.6's hashable row never excluded an enum), `Set<E>`/`Dictionary<E, V>` consult the declared pair (§10.5), boxed `Equals` dispatch reaches it (§8.6, §10.5), `where T: hashable` admits it, and §9.2's three kinds reach the declared members as before.
3. **A record, choice or inline array holding such an enum keeps its generated equality**, comparing that component by the declared `Equals` (§9.2, §35.7, §15.1.2 — the last already reads "the element type's equality"); the enum stays an admissible component, so nothing moves out of auto-hashable or out of §9.8's serialization eligibility, which reuses §9.2's component rule. Measured: no record, choice or inline array in `tests/` or `docs/` has a component typed by an `Equals`-declaring enum (critic census).
4. **`case` is untouched.** `case c of Red => …` matches by identity on every enum (§10.6, new sentence); §19.6's exhaustiveness proof reads the same identity. A constant slot of enum type inside a positional or named destructuring pattern (`Pair(x, Color.Red)`) is §19.3's `=` test, so over an `Equals`-declaring enum without the overloads it is the same compile error the operator is — the mirror of §19.3's named-pattern constant-slot example for a record.
5. **The body of a declared `Equals` compares `int(this) = int(other)`**, as every rule-corpus case and all but two gtest programs already do; `this = other` in that position is the compile error of consequence 1, never a self-call.

## 5. The exact normative edits (▲)

The machine-actionable list is `scratch/v0339/A4.json` (edits with ids `A4-5929-*`; every `old` occurs exactly once at `d09319a91`). In prose:

**Spec.**
- **§8.4**: "A record or a value-like choice that declares its own `Equals` and no homogeneous overload has no `=` at all — the use is §9.2's and §35.7's compile error" → "A record, an enum or a value-like choice that declares its own `Equals` and no homogeneous overload has no `=` at all — the use is §9.2's, §10.5's and §35.7's compile error".
- **§10.5** replacement paragraph: the two §4 sentences inserted after "It is the only member that fills the slot."
- **§10.5** *Ordering*: "Its values compare for equality under §8.4, by underlying integer (§8.3), but" → the §4 text; "decides equality and aliasing; it supplies no operator." → "decides the generated equality, aliasing and §10.6's constant matching; it supplies no operator."
- **§10.6**: the §4 sentence after the example.
- **§9.2** (auto-hashable paragraph) and **§35.7** (generated `=`): the two parentheticals of §4 (edits `A4-5929-s9.2-component`, `A4-5929-s35.7-payload`, added by the lane critic).

**Rationale.** §9.3 gains one dated paragraph (v0.339, proposal 0192) after the 0178 paragraph, recording the decision and the two declines.

**Code-generation reference.** §2's *Equality operator* row gains the clause "an enum declaring its own-typed `Equals` likewise has no `=`/`<>` until it overloads them (inside the body compare `int(this) = int(other)`)". §7.3 gains one *Looks right, is rejected* pair after the `Priority` ordering example: an enum `Color` whose `Equals(other: Color)` body is `this = other` (ERROR, with the `int(this) = int(other)` remedy) and a `Color.Red = Color.Green` use (ERROR, declare the overloads or call `.Equals`). No pins in prose.

**`DECLINED.md`.** Two `declined-at-proposal` rows (§9), inserted at the group's end.

**Stdlib, grammar, guides.** No edit: the stdlib declares no enum with an `Equals` member; the grammar is untouched; the guides' enums declare no equality member (checked by the §6 census).

## 6. Measurement

Census method: every `enum … end` span in `tests/` (gtest C++ string programs, lit, both conformance corpora) and `docs/` scanned for a member `Equals(x: E)` at the enum's own type (22 spans in 17 files), and in each such file every `=`/`<>` whose operands are of the enum type, by `this`, by `E.Constant` on both sides, by a local of that type, or by a declared `operator =`.

| Set | gtest | lit | rule corpus | doc corpus | docs (spec, stdlib, guides, code-generation reference) |
|---|---|---|---|---|---|
| enums declaring the own-typed `Equals` | 11 spans in 7 files (`value_kind_own_typed_equals_5955_test.cpp` ×5, `equality_v0158_test.cpp:327`, `override_equals_parameter_type_5610_test.cpp` ×2, `interface_object_slot_witness_6015_test.cpp:66`, `enum_user_members_1583_test.cpp:42`, `record_generic_constraint_test.cpp:348`, `enum_operators_1582_test.cpp:110`) | 3 (`sema_enum_operators_1582__equality_pair_admitted_on_enum_spec_25_2`, `sema_enum_operators_1582__override_gethashcode_now_accepted_on_enum_spec_10_5`, `sema_enum_choice_override_3837__equals_gethashcode_tostring_override_admitted_on_enum_spec_10_5`) | 8 (`s8_3_0035`, `s8_3_0040`, `s8_3_0047`, `s8_3_0048`, `s8_3_0069`, `s10_5_0008`, `s10_5_0011`; `s10_5_0007` declares the overloads only) | 0 | 0 |
| … of which also overload homogeneous `=`/`<>` (unaffected: §25.2's rule) | 3 programs (`equality_v0158_test.cpp:258`, `:278`, `:325`; `enum_operators_1582_test.cpp:108`) | 2 (the two `1582` files) | 2 (`s10_5_0007`, `s10_5_0008`) | 0 | 0 |
| … `=`/`<>` at the enum's static type with no overload — **newly rejected** | **2** (`override_equals_parameter_type_5610_test.cpp:112` `enum_override_equals_own_type_accepted_spec_10_5`, `:263` `enum_equals_own_type_plain_runs_spec_10_5`; both `Equals(other: Color): bool => this = other`) | **0** | **0** (every body compares `int(this) = int(o)`) | 0 | 0 |
| `=`/`<>` on an enum declaring no `Equals` (unchanged) | many (`enum_operators_1582_test.cpp:159`'s `this = High` is in a program declaring `operator +` only) | — | — | — | — |
| enums declaring `GetHashCode` without the own-typed `Equals` (unchanged under C; would be rejected under D) | 4 spans (`override_equals_parameter_type_5610_test.cpp:80`, `:96`, `:176`, `:188`, all also declaring a rejected `Equals(Object?)`) | 3 (all rejected programs) | 2 (`s10_5_0009`, `s10_5_0010`, both rejected) | 0 | 0 |

**Verdicts.** No lit, rule-corpus or doc-corpus program changes verdict; no banked `baseline.tsv` row moves. Two gtest programs flip accept → reject at the `this = other` body; both are repaired by the `int(this) = int(other)` spelling and then keep their stdout `True` oracle, which the implementation tracker names.

**Words.** Spec, token-level over the four replaced spans: net about +170 words, all in §10.5 and §10.6; the rationale and the code-generation reference are excluded.

## 7. Compiler facts and the tracker

One `spec-tracking` issue at landing, "§8.4 / §10.5 (v0.339): an enum declaring its own `Equals` has no `=` — proposal 0192", carrying:

1. **Sema rejects `=`/`<>` at the static type of an enum declaring its own-typed `Equals` and no homogeneous overload**, with the record's use-site diagnostic shape (the remedy names `operator =`/`<>` and `.Equals`). The predicate exists for records (`isAutoEqualityBearingBodyRecord`'s name-keyed scan, `src/sema/SymbolCollection.cpp:2188`); the enum needs the same scan at the `=` type-check. Codegen's `emitValueEquality` integer arm is then unreachable for such operands and needs no change; the through-`T` path and the box thunks (`emitEnumEquals`) already reach the declared member.
2. **Two gtest rows flip:** `tests/sema/override_equals_parameter_type_5610_test.cpp` `enum_override_equals_own_type_accepted_spec_10_5` (`:112`) and `enum_equals_own_type_plain_runs_spec_10_5` (`:263`) — their `Equals` bodies become `int(this) = int(other)`; the rows' oracles (`42`; stdout `True`) are unchanged.
3. **Generated component comparisons** (a record's or value-like choice's generated `=`, an inline array's element-wise equality) reach the declared `Equals` of an enum component that has no `=`; today `emitValueEquality` compares such a component by integer (spec-ahead-of-impl, zero measured sites).
4. **Constant patterns** over an enum scrutinee keep lowering by integer identity (`case` on enums never consulted `Equals`); a constant *slot* of enum type inside a destructuring pattern over an `Equals`-declaring enum without the overloads is rejected as its `=` is.

## 8. Corpus cases to write (after landing)

- §10.5 / §8.4: `enum Color` declaring `Equals(other: Color)` and `GetHashCode()` — `Color.Red = Color.Green` rejected (`reject`, `diagnostic: operator`); `Color.Red.Equals(Color.Green)` and `new Set<Color>()` admitted (`accept`); `Same<Color>(Color.Red, Color.Green)` under `where T: hashable` prints the declared answer (`run`); `case c of Red => …` still selects by identity (`run`).
- §10.5: the same enum with `operator =`/`<>` declared — `Color.Red = Color.Green` dispatches to the overload (`run`; `s10_5_0008` already pins the admission).
- §10.6: a constant slot of such an enum's type inside a positional destructure is rejected (`reject`).

## 9. Declined alternatives (one `declined-at-proposal` row each)

- **Option A, the status quo** (#5929 reading 2): the concrete `=` intrinsic on the underlying integer beside a replaced `Equals`, the type-parameter `=` reaching the declared member. One operator with two definitions — `=` and `Set<E>` disagreeing on one pair — is the divergence §25.2's co-declaration rule and §9.2's second disqualifier exist to prevent, and the one 0178 removed from the choice. Reopening bar: none stated.
- **Option B, `=`/`<>` dispatching to the declared own-typed `Equals` on an enum** (#5929 reading 1; the issue's and the `L3` lane's recommendation). It is the enum analogue of proposal 0178's declined option C (0178's row names the record and the value-like choice; 0178 §10 left the enum undecided, so the row does not bind this option and its reasons are applied by analogy): a dispatch rule no other value kind would have, for a convenience the two overload lines spell, and it makes the natural body `this = other` an unbounded self-call that needs its own diagnostic. Reopening bar: the bar 0178's row states — a measured burden of the two overload lines in real programs — met together with a stated answer for the self-call.

## 10. Self-critique (adversarial pass)

- **Is the enum's `=` a "stated admission" that this narrows?** Yes — §10.5's *Ordering* admits `=` on every enum in so many words — which is why this is a proposal and not a ruling, though the text it narrows contradicts two other sentences and the compiler's own through-`T` behaviour.
- **Does `case` survive?** A constant pattern over an enum scrutinee was never defined through `=` (§19.3's *Constants* names the form only; the `=` test is stated for a constant *slot* inside a destructuring, §19.3's purity list and Appendix C.1). The §10.6 sentence states what every enum does today and keeps §19.6's coverage proof. The constant-slot consequence (item 3 of §4) is the record's existing posture, at zero sites.
- **`Array<E>.Contains` and the equality search.** Stdlib §4's equality search reaches `Equals`/`GetHashCode` (the §14.2 register's row), never `=`, so it reaches the declared pair on an enum as on a record. Unchanged.
- **`in` over a range of enums, relational patterns.** Not intrinsic on an enum (§10.5 *Ordering*, §19.3 *Ordered scrutinee types*); unchanged.
- **A lone `GetHashCode`.** Kept outside the rule on purpose (option D declined in §3): no hazard, and §8.3 says a lone `GetHashCode` obliges nothing.
- **Through `T` at an enum.** §9.2's first kind "reaches S's own-typed `Equals`, declared or generated" — already the declared member, so no edit and no new divergence: the concrete `=` is an error, the generic one reaches the member, exactly the record's two faces.
- **Composition.** Lane `A4`'s other edits touch §10.5 (the reserved-names sentence after "co-declaration obligation."), §9.2, §8.3 and §8.5; the anchors are disjoint from this proposal's. Lane `E`'s editorial edits name §2.7, §11.3, §11.2, §12.7 and §28.x; no shared paragraph. The README row and the `DECLINED.md` rows anchor on the group-closing headings, as prior trains did; the coordinator orders 0187–0192.
- **History narration.** The normative edits state the current rule only; the story is in rationale §9.3's dated paragraph and the changelog.
