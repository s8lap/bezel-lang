# Proposal 0197 — An enum-typed constant slot matches by identity

| | |
|---|---|
| **Status** | graduated-v0.340 — superseded — graduated v0.340 (accepted 2026-10-21 as a delegated ruling under the owner's 2026-10-08 direction ("Please resolve and decide all known spec related issues. Do proposals for issues that need them.") and the 2026-10-21 direction for this train ("let's do the next train that fixes all current spec issues"), [#6581](https://github.com/s8lap/bezel-lang/issues/6581), option B — a constant slot of enum type inside a destructuring pattern, positional or named, matches by §10.2's underlying-integer identity whatever the enum declares, as the top-level constant pattern does, a `null` in a nullable slot matching no constant; a record's constant slot keeps §19.3's `=` test; proposal 0192 §4 item 4 reversed; drafted as a ruling and re-classed at critique round 1 as a design change (the reversal of a decision an accepted proposal recorded), the changed selection over an enum declaring `operator =` recorded; landed with the v0.340 changelog entry) |
| **Tracking issue** | [#6581](https://github.com/s8lap/bezel-lang/issues/6581). No new compiler tracker. The landing comment on [#6602](https://github.com/s8lap/bezel-lang/issues/6602) (proposal 0192's tracker) strikes its item 3 (§7). |
| **Target module(s)** | **spec**: §10.6 (the constant-slot clause replaced), §19.3 *Named destructuring* (an exception clause in the constant-slot sentence), Appendix A.7 (the grammar comment), Appendix C.1 (the slot-list bullet). **rationale**: §9.3 (a dated note on 0192's paragraph). **code-generation reference**: §7.3 (one clause in 0192's `Color` line). **proposals**: `README.md` (this row), `DECLINED.md` (one `declined-at-proposal` row). Proposal 0192 is graduated history and is not edited. |
| **Written against** | spec **v0.339** at `d60128c46` (main). Section numbers are v0.339's. |
| **Origin** | Proposal 0192 (v0.339, [#5929](https://github.com/s8lap/bezel-lang/issues/5929)) decided in its §4 item 4 that a constant slot of enum type is §19.3's `=` test, and so is rejected over an `Equals`-declaring enum without the overloads. Its lane's critic filed the question the same day (#6581). Lane B2 of the v0.340 train drafted it as a ruling. Its critic re-classed it as a design change because it reverses a decision an accepted proposal recorded (§2). |

*Reference convention: unprefixed §-references are language-spec references at v0.339. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the standard library companion declares or declares its own; none invents an API. The former name of the language is not used anywhere in this document.*

---

## 0. Summary

v0.339 gave an enum two ways to test one value against one constant, and they can disagree:

```bezel
public enum Color Red, Green;
  public method Equals(other: Color): bool => int(this) = int(other);
  public method GetHashCode(): int => int(this);
end Color;
public record Pixel(C: Color, X: int);
…
case p.C of Red => …            // admitted: §10.6 — a constant pattern over an enum matches by identity
case p of Pixel(Color.Red, var x) => …   // rejected at v0.339: a constant slot is §19.3's `=`, and 0192 takes Color's `=` away
```

This proposal makes the slot take the top-level pattern's test. A constant slot of enum type, positional or named, matches by §10.2's underlying-integer identity, whatever the enum declares. A `null` in a slot of nullable enum type matches no constant. A record's constant slot keeps §19.3's `=` test. Reach: zero sites. The compiler's constant-pattern lowering already uses the intrinsic compare, so there is nothing new to implement.

## 1. The problem

> A constant pattern over an enum scrutinee matches by [§10.2]'s identity of the value with its underlying integer, on every enum, whatever members it declares, and [§19.6]'s coverage reads the same identity; a constant *slot* of enum type inside a destructuring pattern is [§19.3]'s `=` test and takes that operator's verdict ([§10.5]).
>
> — §10.6, v0.339

> 4. **`case` is untouched.** […] A constant slot of enum type inside a positional or named destructuring pattern (`Pair(x, Color.Red)`) is §19.3's `=` test, so over an `Equals`-declaring enum without the overloads it is the same compile error the operator is — the mirror of §19.3's named-pattern constant-slot example for a record.
>
> — proposal 0192 §4 (*Decision*), item 4

The slot and the top-level pattern differ in a second way. On an enum that declares `operator =`/`<>`, the slot runs the user's operator, while `case p.C of Red` uses the identity. 0192 §10 called the slot consequence "the record's existing posture, at zero sites". But a record has no identity to fall back on, and an enum does: §10.6 already uses it for every top-level constant pattern.

## 2. Why this is a proposal

The v0.339 text is not underspecified here. 0192's *Decision* section states reading 1 in so many words, §8 of that proposal plans a `reject` corpus case for it, and tracker #6602 carries it as item 3. Reversing a settled rule, recorded in an accepted proposal's decision one revision ago, is a design change under the process's §2, even though no `DECLINED.md` row names reading 2 and the change admits rather than rejects. Rationale §9.3 records the same pattern of reversal-through-proposal at v0.336 (proposal 0176, "reversing the v0.335 decline").

## 3. Options

- **A — status quo (reading 1).** The enum slot is §19.3's `=` test. Over an `Equals`-declaring enum without the overloads it is rejected, and the remedy is the overloads or a guard (`Pixel(var c, var x) when c.Equals(Color.Red)`). Over an enum declaring `=`, the slot runs that operator.
- **B — identity (reading 2, decided).** The enum slot matches by §10.2's identity, exactly as the top-level constant pattern does.
- **C — identity only where the enum has no `=`.** This keeps the declared operator's verdict where one is declared and falls back to identity otherwise.

## 4. Evaluation

| | Soundness | Simplicity | Consistency | Migration reach |
|---|---|---|---|---|
| A | sound | a third test spelling for enums | `Pixel(Color.Red, _)` and `case p.C of Red` differ on one enum | 0 |
| B | sound: identity is the enum's value, and no user code runs during matching (so the slot is never a [§19.3] write point by a call) | one sentence; one test for both spellings | the slot follows §10.6 | 0 (newly admitted) |
| C | sound | a rule keyed on whether the operator is declared | still two answers on an enum that declares `=` | 0 |

B is the simplest. The test is the one §10.6 already defines and the compiler already emits, and a reader never has to ask which spelling of the constant test they wrote.

## 5. Decision

**Option B.** A constant slot of enum type inside a destructuring pattern, positional or named, matches by §10.2's underlying-integer identity, whatever the enum declares. Over a slot of nullable enum type, a `null` value matches no constant. A record's constant slot keeps §19.3's `=` test, including §19.3's rejection over a record that bears no `=`. §10.5 *Ordering* already says identity "decides … §10.6's constant matching" and needs no edit.

## 6. Measurement

- Enum constant slots in `tests/`: `record_pattern_constants_615_test.cpp:225–234` (`TwoColor(Color.Red, Color.Blue)` over a plain enum: identity and `=` agree, unaffected). No destructuring pattern in `tests/`, the rule corpus, the doc corpus, the guides or the Tour places a constant slot over an enum that declares `Equals` or `operator =` (critic's re-check over the enums declaring `operator =`: `enum_operators_1582_test.cpp`, `hashable_constraint_2087_test.cpp`, `equality_v0158_test.cpp`, `pure_operator_rule6_3610_test.cpp`, `s10_5_0007`, `s10_5_0008`, `s25_2_0002`: none destructures). **0 sites** newly admitted or changing selection.
- Banked rule-corpus rows: none move. `s10_6_0001`, `s19_3_0008`, `s19_3_0019` and `s19_3_0023` involve no `Equals`-declaring enum.

## 7. Compiler facts and the tracker

- The generic constant-pattern lowering (`src/codegen/LlvmBackend_Exprs2.cpp:15040`, resolving an enum constant by name, then `emitEquals`) compares by the intrinsic, never by a declared `Equals` or `=`. This was read from the code, not probed (no compiler binary in this environment). 0192's sema rejection at `=` (#6602 item 1) is unimplemented, so the slot and the operator agree today by accident.
- **No new tracker.** The landing comment on #6602 strikes item 3 and replaces it with this requirement: after item 1 lands, an enum-typed constant slot keeps the identity compare and is not routed through the `=` check.

## 8. Corpus cases to write (after landing)

0192 §8's planned case "a constant slot of such an enum's type inside a positional destructure is rejected" is **not** to be written. Its replacements:

- `s10_6`: `Pixel(Color.Red, var x)` over an `Equals`-declaring `Color` without the overloads → `run`, selecting by identity.
- `s10_6`: the same with an enum that declares `operator =`/`<>` whose verdict differs from identity → `run`, selecting by identity.
- `s19_3`: a named slot `Pixel(C = Color.Red)` → `run`. A `Color?` slot holding `null` against `Color.Red` → no match.

## 9. Declined alternatives (one `declined-at-proposal` row)

- **Option A, the status quo** (#6581 reading 1; proposal 0192 §4 item 4). The two spellings of one test would differ on one enum, and identity is what §10.6 already chose for the constant pattern on every enum. A record's constant slot keeps its `=`, because a record has no canonical identity to fall back on. Reopening bar: none stated.
- *Option C* is not given a row: it is a hybrid of A and B that keeps A's disagreement wherever an enum declares `=`.

## 10. The exact normative edits (`scratch/v0340/B2.json`)

Ids `B2-6581-spec-10.6`, `B2-6581-spec-19.3`, `B2-6581-spec-A.7`, `B2-6581-spec-C.1`, `B2-6581-codegen-7.3-fence`, the 0197 half of `B2-0196-0197-rationale-9.3`, this file's README row (`B2-0196-0197-readme-rows`) and its row in `B2-declined-at-proposal-rows`.

> ▲ **§10.6** (the clause after "…reads the same identity;"). a constant *slot* of enum type inside a destructuring pattern ([§19.3]), positional or named, matches by the same identity, whatever the enum declares ([§10.5]) — `Pixel(Color.Red, var x)` over `record Pixel(C: Color, X: int)` is admitted and selects as `case p.C of Red => …` does; over a slot of nullable enum type a `null` value matches no constant.

> ▲ **§19.3.** A **constant slot is the equality test** `scrutinee.N = c` under the slot's declared type's `=` ([§8.4], null-safe over a nullable slot) — except that a slot of enum type, positional or named, matches by [§10.2]'s underlying-integer identity, as a constant pattern over an enum scrutinee does ([§10.6]), whatever the enum declares; […]

> ▲ **Appendix A.7** (grammar comment). A constant slot is the "=" equality test (§8.4); an enum-typed slot matches by §10.2's identity (§10.6).

> ▲ **Appendix C.1.** […] and a constant slot an `=` test — an enum-typed slot [§10.2]'s identity test, [§10.6] — ([§19.3]).

**Changelog bullet (draft).**
- **§10.6, §19.3, Appendix A.7 and C.1, rationale §9.3, the code-generation reference §7.3: an enum-typed constant slot in a destructuring pattern matches by §10.2's identity ([proposal 0197](proposals/0197-enum-constant-slot-matches-by-identity.md); [#6581](https://github.com/s8lap/bezel-lang/issues/6581) closes; proposal 0192 §4 item 4 and tracker [#6602](https://github.com/s8lap/bezel-lang/issues/6602) item 3 reverse).** v0.339 made the top-level constant pattern match by identity on every enum but left the constant *slot* on §19.3's `=` test. So `Pixel(Color.Red, var x)` was rejected over an `Equals`-declaring enum while `case p.C of Red` was admitted, and over an enum declaring `=` the slot ran the operator. The slot, positional or named, now takes the same identity, and a `null` in a nullable slot matches no constant. A record slot keeps the `=` test. **Declined:** the `=` test for the enum slot (the status quo). One `declined-at-proposal` row. Kernel: no contact. Body-locality: local (a pattern). Migration: *Newly admitted (relative to v0.339's text):* the slot over an `Equals`-declaring enum without the overloads, 0 sites. *Changed selection:* the slot over an enum declaring `operator =`, 0 sites. **Compiler:** the constant-pattern lowering already compares by the intrinsic. #6602's item 3 is struck.

## 11. Self-critique (adversarial pass)

- **Does this reopen 0192?** No. 0192's option C, under which an `Equals`-declaring enum has no `=`, stands in full. Only its §4 item 4 consequence for the slot is reversed, and that item recorded no declined alternative. The row this proposal adds declines the status quo.
- **Write points.** §19.3's write-point list names "a constant slot's `=` during matching" as a call. An enum-typed slot now runs no call, so it is never a write point. A record slot still is. The text needs no edit: the bullet speaks of the `=` a slot runs.
- **Generic slots.** A slot declared at a type parameter `T` and read at `Color` is "of enum type" at that instantiation, and no constant of type `T` can be written in a generic body, so no generic-body case arises.
- **History narration.** The normative edits state the current rule. The reversal is told in the rationale's dated note and the changelog.
