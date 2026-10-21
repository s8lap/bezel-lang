# Proposal 0196 — The `Object`-slot `Equals` parameter declares no default

| | |
|---|---|
| **Status** | graduated-v0.340 — superseded — graduated v0.340 (accepted 2026-10-21 as a delegated ruling under the owner's 2026-10-08 direction ("Please resolve and decide all known spec related issues. Do proposals for issues that need them.") and the 2026-10-21 direction for this train ("let's do the next train that fixes all current spec issues"), [#6584](https://github.com/s8lap/bezel-lang/issues/6584), option C — a default value on the `Object`-slot `Equals` parameter is a compile error at the declaration, on a class in either spelling and on every value kind: every path to the slot passes one argument, so the default could serve only a direct `x.Equals()`; the default as no part of the slot's signature (the v0.339 text's reading) and the member as an ordinary method filling no slot (the v0.339 compiler's box) declined; drafted as a ruling and re-classed at critique round 1 as a design change, §8.3's and §9.2's words having admitted the defaulted member as the slot; landed with the v0.340 changelog entry) |
| **Tracking issue** | [#6584](https://github.com/s8lap/bezel-lang/issues/6584). One `spec-tracking` compiler tracker is filed at landing (§7). |
| **Target module(s)** | **spec**: §8.3 (one sentence after the one-spelling sentence), §9.2 *Dispatch modifiers on value kinds* (one clause), Appendix C.1 (one sentence in the `Equals` bullet). **rationale**: §9.3 (one dated paragraph). **code-generation reference**: §7.4 (one *Looks right, is rejected* block and one clause in its blockquote). **proposals**: `README.md` (this row), `DECLINED.md` (two `declined-at-proposal` rows). No stdlib or guide text, no grammar change, no new keyword. |
| **Written against** | spec **v0.339** at `d60128c46` (main). Section numbers are v0.339's. |
| **Origin** | A data point on [#6011](https://github.com/s8lap/bezel-lang/issues/6011) (the R30 review of the #6015 fix-forward): at `8cb5f314` the box reached a record's declared `Equals(other: R := …)`; after #6015 it compares member-wise. The 2026-10-19 ruling on #6011 split the question out; v0.339 filed it. Lane B2 of the v0.340 train drafted it as a ruling; its critic re-classed it as a design change (§2 below), and this file records it. |

*Reference convention: unprefixed §-references are language-spec references at v0.339. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the standard library companion declares or declares its own; none invents an API. The former name of the language is not used anywhere in this document.*

---

## 0. Summary

§8.3 and §9.2 key the `Object` slot on the member's parameter type: on a class, "an `Equals` whose one parameter is the declaring class's own type, `C` or `C?` … **is the `Object` slot's implementation**"; on a value kind, "the `Equals` is the one taking the kind's own type". Neither mentions a default value, and §22.5 admits a default on any unmodified parameter. So by the words, `Equals(other: R := …)` is the slot. The compiler disagrees with itself: the box excludes such a member (`declaredPairFillsObjectSlots`'s `ps->hasDefault` test) and compares member-wise, while hash containers match `Equals` by name and arity and reach the declared body. One type has two equalities.

This proposal makes the declaration a **compile error**: the slot's parameter declares no default value, on a class in either spelling and on every value kind. Every path to the slot passes exactly one argument, so a default there could serve only a direct `x.Equals()` call on a receiver whose static type declares it. Reach: zero sites.

## 1. The problem

> On a class, an `Equals` whose one parameter is the declaring class's own type, `C` or `C?` (`C` with its own type parameters), **is the `Object` slot's implementation** […]. Every path reaches that one member, virtually: `=` and `<>` ([§8.4]), a `Set<C>` or `Dictionary<C, V>` built without a comparer, a use through a type parameter at `C` or at a class deriving from `C` ([§9.2]), `IEquatable<C>`, and an `Object`- or interface-typed receiver
>
> — §8.3, v0.339

> On every value kind the `Equals` is the one taking the kind's own type — a generic record's or choice's with its own type parameters — declared `override` or plain, the two spellings declaring one member:
>
> — §9.2 *Dispatch modifiers on value kinds*, v0.339

Every route the first quotation lists calls the slot with one argument. A default on its parameter is reachable only by `x.Equals()` spelled with no argument, at a receiver whose static type is the declaring type or a subclass that keeps the default. An override may restate the parameter with a different default or none, since §12.1 matches parameter types and modifiers and says nothing of defaults, so even that call's meaning depends on the receiver's static type. The issue lists three readings:

1. the default is not part of the slot's signature, so the member fills the slot (what the words above say);
2. a defaulted parameter is a different shape, so the member is an ordinary method that fills no slot (the compiler's box);
3. a default on the slot's parameter is a compile error at the declaration (the issue's recommendation when measured reach is zero).

## 2. Why this is a proposal

Reading 1 is what §8.3 and §9.2 state: the member has one parameter of the own type. Reading 3 rejects a declaration those sentences make the slot's implementation, so it narrows a stated admission. Under the process's §2 that is a design change, and the stricter gate applies where the class is in doubt. v0.339 landed its reserved-names rejections (#6191, #6305) as rulings on similar zero-site footing. Here the admission is explicit in the slot's own defining sentence, so this file takes the proposal path.

## 3. Options

- **A — status quo text (reading 1).** The member fills the slot. A direct `x.Equals()` fills the default; every other path passes the argument. The compiler's box must then be fixed to reach the member.
- **B — the box's reading (reading 2).** A defaulted own-typed parameter makes the member ordinary. It fills no slot, obliges no `GetHashCode`, and the type keeps its generated equality.
- **C — reject the default (reading 3, decided).** `Equals(other: C := …)` on a class in either spelling (`override Equals(other: Object? := null)` included), and `Equals(other: R := …)` on a record, enum or choice, is a compile error at the declaration naming the remedy: drop the default.

## 4. Evaluation

| | Soundness | Simplicity | Consistency | Migration reach |
|---|---|---|---|---|
| A | sound once the box is fixed | a default with no reachable use except one call spelling, whose meaning varies with the receiver's static type | the slot keyed on type alone, as today | 0 |
| B | sound, but one member is the slot by its type and not the slot by its default; hash containers and the box must both change to agree | adds a "fills no slot" carve-out keyed on a default | contradicts §8.3's "an `Equals` whose one parameter is …" | 0 |
| C | sound | one sentence; nothing is left for a reader to decide | the slot is defined by its signature, and a signature with a dead default is refused, as §9.2 refuses a static `Equals` on a value kind | 0 (newly rejected) |

C is the smallest rule: it leaves no semantics to define. A needs a decision about defaults under virtual dispatch that the language has never had to make for this member. B keeps the two-equalities state the issue measured.

## 5. Decision

**Option C.** The `Object`-slot `Equals` parameter declares no default value. The error is at the declaration, on a class in either spelling and on every value kind. `GetHashCode()` and `ToString()` have no parameter to default. A `ToString(format: string := "G")` is not the slot by arity (§12.2, §10.5, unchanged).

## 6. Measurement

- `grep -rnE 'Equals\([a-zA-Z_]+: [A-Za-z0-9_<>?, ]+:='` over `tests/` (gtest C++ strings, lit `.bzl`, the rule corpus and the doc corpus) and the normative documents: **0 hits** (critic's re-run). The #6011 R30 data point was a review probe outside the tree (QUOTED).
- `GetHashCode(`/`ToString(` with a defaulted parameter: 0 in `tests/`. `builtin_named_arg_binding_3424_widen_test.cpp:205,213` are `Convert.ToString` calls with named arguments, not declarations.
- Stdlib: `string`'s `Equals(other: string?, ignoreCase: bool)` declares no default and has two parameters. It is an ordinary overload. Unchanged.
- Banked rule-corpus rows: none move. `s8_3_0034`–`0049`, `s9_2_0021`–`0028` and `s10_5_0009`–`0011` declare the own-typed member without a default.

## 7. Compiler facts and the tracker

- No rejection exists. `src/codegen/LlvmBackend_Setup.cpp:47793–47815`: `declaredPairFillsObjectSlots` returns false when the `Equals` parameter `hasDefault`, so the box keeps the structural slots. `src/codegen/LlvmBackend_Calls3.cpp:157–163`: `userValueEqualityPair` takes any one-parameter `Equals` at the own type, with no default check, and wires it into hash containers. `src/sema/TypeResolution_Checks.cpp:723–740`: `objectSlotEqualsSpelling` is keyed on the parameter type only.
- **Tracker (`spec-tracking`).** Title: "§8.3 (v0.340, proposal 0196): reject a default value on the `Object`-slot `Equals` parameter at the declaration; the box and the hash containers disagree on such a member today". Body: add the check beside `objectSlotEqualsSpelling` / `checkObjectSlotOverrideSignature` (`TypeResolution_Checks.cpp:7708`) for both class spellings and every value kind's own-typed `Equals`. Once that lands, the `hasDefault` arm of `declaredPairFillsObjectSlots` is unreachable. Repro: the code-generation reference's `Coin`, and a class `Equals(other: C := …)`.

## 8. Corpus cases to write (after landing)

- `s8_3`: `record Coin(Amount: int)` declaring `Equals(other: Coin := default(Coin))` and `GetHashCode()` → `reject`.
- `s8_3`: a class declaring `Equals(other: C? := null)` and `override GetHashCode()` → `reject`. A class declaring `override Equals(other: Object? := null)` → `reject`.

## 9. Declined alternatives (one `declined-at-proposal` row each)

- **Option A, the default as no part of the slot's signature** (#6584 reading 1, the v0.339 words). Every path to the slot passes one argument. The default serves only a direct no-argument call on a receiver whose static type declares it, a call that no other route to the slot (`=`, a hash container, boxed dispatch, `IEquatable<T>`, a type parameter) can make, and whose default an override may restate differently or drop. Reopening bar: a measured use of the no-argument `Equals()` call.
- **Option B, the member as an ordinary method that fills no slot** (#6584 reading 2, the v0.339 compiler's box). One member would be the slot by its parameter type and not the slot by its default. That is the compiler's state: hash containers reach the declared body while the box compares member-wise. One type would have two equalities, the divergence §25.2's co-declaration rule exists to prevent. Reopening bar: none stated.

## 10. The exact normative edits (`scratch/v0340/B2.json`)

Ids `B2-6584-spec-8.3`, `B2-6584-spec-9.2`, `B2-6584-spec-C.1`, `B2-6584-codegen-7.4-fence`, `B2-6584-codegen-7.4-quote`, the rationale half of `B2-0196-0197-rationale-9.3`, this file's README row (`B2-0196-0197-readme-rows`) and its two rows in `B2-declined-at-proposal-rows`.

> ▲ **§8.3** (after "A class writes one spelling — the own-typed one with or without `override` — and a second declaration in the other is a compile error ([§12.2])."). The slot's parameter declares **no default value** ([§22.5]): on a class in either spelling and on a value kind alike, `Equals(other: C := …)` is a compile error at the declaration — every path to the slot passes one argument, so a default there could serve only a direct `x.Equals()` call, and the member's signature, not a default on it, is what fills the slot.

> ▲ **§9.2.** On every value kind the `Equals` is the one taking the kind's own type — a generic record's or choice's with its own type parameters; its parameter declares no default ([§8.3]) — declared `override` or plain, the two spellings declaring one member:

> ▲ **Appendix C.1** (the `Equals` bullet, appended). The slot's parameter declares no default value, on a class or a value kind ([§8.3]).

**Changelog bullet (draft).**
- **§8.3, §9.2, Appendix C.1, rationale §9.3, the code-generation reference §7.4: the `Object`-slot `Equals` parameter declares no default ([proposal 0196](proposals/0196-object-slot-equals-declares-no-default.md); [#6584](https://github.com/s8lap/bezel-lang/issues/6584) closes).** §8.3 and §9.2 key the slot on the parameter's type. By their words `Equals(other: R := …)` was the slot. The compiler's hash containers reached the declared body, and its box compared member-wise. The declaration is now a compile error on every kind and in either class spelling, because every path to the slot passes one argument. **Declined:** the default as no part of the signature (reading 1); the member as an ordinary method that fills no slot (reading 2). Two `declined-at-proposal` rows. Kernel: no contact. Body-locality: local (declaration-site). Migration: *Newly rejected (spec-ahead-of-impl):* the declaration, 0 sites. **Compiler (tracked):** no rejection exists. `declaredPairFillsObjectSlots` keeps the member off the box while `userValueEqualityPair` wires it into hash containers.

## 11. Self-critique (adversarial pass)

- **Is a rejection the simplest sound option?** Yes. A and B each need the compiler's two paths to agree on one member, and A also leaves a default whose one use varies with the receiver's static type. C removes the member shape.
- **Neighbouring cells.** `Equals(other: C, flag: bool := false)` has two parameters. §8.3 does not make it the slot, and `userValueEqualityPair` requires one parameter, so it is an ordinary overload. A direct `x.Equals(y)` prefers the slot member under §22.6's defaulted-arity tie-break. This proposal leaves it unchanged. An `Equals(other: int := 0)` (for `IEquatable<int>`) is not the slot and stays admitted.
- **Interface members.** `IEquatable<T>.Equals(other: T)` is a stdlib declaration without a default. An implementing class's own-typed `Equals` is the slot and is covered.
- **History narration.** The normative edits state the current rule. The story is in the rationale's dated paragraph and the changelog.
