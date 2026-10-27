# Proposal 0204 — A pattern over a `char` scrutinee reads `char` constants

| | |
|---|---|
| **Status** | graduated-v0.345 — superseded — graduated v0.345 (accepted 2026-10-27 as a delegated ruling under the owner's 2026-10-08 direction ("Please resolve and decide all known spec related issues. Do proposals for issues that need them.") and the 2026-10-27 direction for this train ("Post the rulings including on 7276, consider whether any clarifications are needed in the spec, and do a new train to resolve known spec issues."), surfaced by proposal 0203's critique, option B — over a `char` scrutinee, nullable or not, a constant pattern's constant, a relational pattern's constant operand and a range pattern's bounds are `char` constants, an integer constant in any of the three forms being a compile error at the pattern, while the expression `c = 65` keeps §3.5's promotion; the status-quo split and integer bounds over a `char` by value declined; critique round 1 applied (the `char?` scrutinee); landed with the v0.345 changelog entry) |
| **Tracking issue** | none at drafting (the question was surfaced inside the v0.345 train, not filed). One `spec-tracking` compiler tracker is filed at landing (§7); it can ride proposal 0203's tracker as its `char` item. |
| **Target module(s)** | **spec**: §19.3 (one sentence appended to *Ordered scrutinee types*), Appendix C.7 (one bullet). **rationale**: §11.11 (one dated paragraph). **code-generation reference**: §7.1 (one `// WRONG` line in the existing block). **proposals**: `README.md` (this row), `DECLINED.md` (two `declined-at-proposal` rows). No stdlib text, no grammar change, no new keyword. |
| **Written against** | spec **v0.344** at `b4243d87f` (main), read together with proposal 0203's §19.3 edits (lane C3 of the v0.345 train, `scratch/v0345/C3.json`). Section numbers are v0.344's. |
| **Origin** | Proposal 0203 (lane C3, v0.345) reads a pattern's constants at the scrutinee's type under §5.1 and, over a `char` scrutinee, rejects a range pattern with integer bounds (`case c of 65..90`), since `int → char` is no §5.1 conversion. Its critic found that the *Constants* and *Relational* sentences of 0203 cover integer scrutinees only, so `case c of 65` stays §8.4's admitted `int(c) = 65` while `case c of 65..65` is rejected, and filed the question (C3.md, *To file* 3). Lane F folds it. |

*Reference convention: unprefixed §-references are language-spec references at v0.344, read with 0203's text where it changes §19.3. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the standard library companion declares; none invents an API.*

---

## 0. Summary

Under 0203 a `case` over a `char` reads its constants by two rules:

```bezel
var c: char := Console.ReadKey();
case c of
  65..90 => "upper",       // rejected by 0203: `65` is no `char` under §5.1
  65     => "A",           // admitted: §8.4's `=`, promoted by §3.5 to `int(c) = 65`
  >= 97  => "lower or beyond",   // admitted: §3.5 orders `char` and promotes
  _      => "other"
end;
```

This proposal makes the three forms read one rule. Over a `char` scrutinee every pattern constant is a `char`: an integer constant in a constant pattern, as a relational pattern's operand or as a range bound is a compile error at the pattern. The spellings are `'A'`, `>= 'a'` and `'A'..'Z'`, or a `case` over `int(c)`. The expression side is untouched: `c = 65` and `c < 122` keep §3.5's promotion. Reach: zero sites.

## 1. The problem

> Over an **integer** scrutinee — nullable or not, read at its underlying type — a constant pattern's constant converts to the scrutinee's type under §5.1 […] A relational pattern's constant operand and a range pattern's bounds are read the same way (below).
>
> — §19.3 *Constants*, proposal 0203's text (v0.345)

> A range pattern's bounds are read at the scrutinee's type as a range literal is target-typed at the left operand of `in` […]: over an integer or `char` scrutinee each written bound converts to the scrutinee's type under §5.1, a constant by its value, and a bound that does not is a compile error at the pattern
>
> — §19.3 *Inclusive and exclusive ranges*, proposal 0203's text (v0.345)

> `c < 'z'` and `c < 122` are unaffected: ordering lists `char`.
>
> — §3.5, v0.344

> **Constants:** […] a constant pattern is the equality test of [§8.4]
>
> — §19.3 *Relational*, v0.344 (the parenthetical on nullable scrutinees)

0203's range sentence names `char`; its constant and relational sentences name integer scrutinees only. So over a `char`, `case c of 65..90` is rejected while `case c of 65` and `case c of >= 65` fall back to the expression rules: §8.4's `=` and §3.5's ordering, both of which promote `char` to `int` under §5.1's `char → int` row. `case c of 65..65` and `case c of 65` are then one test spelled two ways with opposite verdicts.

## 2. Why this is a proposal

v0.344 admits `case c of 65` by composition: §19.3 makes a constant pattern §8.4's `=` test, and §8.4 with §3.5 admits `c = 65`. Rejecting it narrows a stated admission, which is a design change under the process's §2 stricter-gate rule, even at zero sites and even though 0203 already drew the same line for the range form. `DECLINED.md` has no row on it.

## 3. Options

- **A — status quo under 0203.** The range form reads `char` bounds; the constant and relational forms read promoted integers. `case c of 65` is admitted and `case c of 65..65` is rejected.
- **B — every pattern constant over a `char` is a `char` (decided).** The three forms read the scrutinee's type under §5.1, as 0203 reads them over an integer scrutinee. An integer constant is a compile error at the pattern. The expression `c = 65` is unchanged.
- **C — every pattern constant over a `char` admits an integer by value.** An integer constant converts to `char` when it is a valid code point, in all three forms: `case c of 65..90` is `'A'..'Z'`.

## 4. Evaluation

| | Soundness | Simplicity | Consistency | Migration reach |
|---|---|---|---|---|
| A | sound | two rules over one scrutinee | `case c of 65` admitted, `case c of 65..65` rejected | 0 |
| B | sound: a `char` compares with a `char`; no truncation or surrogate question arises | one rule, 0203's own, for every scrutinee type a pattern constant can meet | the pattern/expression split is the one 0203 already drew (`b > 300` beside `case b of > 300`) | 0 (newly rejected: zero sites) |
| C | sound, but needs a rule for a surrogate-valued bound (`0xD800`, a compile error at `char(0xD800)` under §5.2) and for `0x110000` | a conversion §5.1 does not have, private to pattern position | the one position where an integer becomes a `char` implicitly; `char(65)` is explicit everywhere else | 0 (newly admitted: zero sites) |

B is the simplest. It states nothing 0203 did not already state for integer scrutinees, and a reader of a `case` over a `char` sees `char` literals in every arm.

## 5. Decision

**Option B.** Over a `char` scrutinee every pattern constant is a `char`: a constant pattern's constant, a relational pattern's constant operand and a range pattern's bounds convert to `char` under §5.1, which converts no integer to `char`, so `case c of 65`, `case c of >= 65` and `case c of 65..90` are compile errors at the pattern. The spellings are `'A'`, `>= 'A'` and `'A'..'Z'`, or a `case` over `int(c)`. A `char?` scrutinee is read at `char` (the sentence says "nullable or not", as *Constants* does for integers); a `char` scrutinee §7.4 has narrowed non-null, and a `char` slot of a destructuring pattern, are read the same way (the *Ordered scrutinee types* paragraph already says the rule holds at every position the patterns take). A `char` constant written as an escape (`'A'`) is a `char`. The expression side is untouched: `c = 65` keeps §8.4's and §3.5's promotion, as `b > 300` keeps its comparison beside the rejected `case b of > 300` (0203).

## 6. Measurement

- No `case` over a `char` scrutinee in `tests/` (gtest C++ program strings, lit `.bzl`), `tests/conformance/rules/cases/`, `tests/conformance/cases/`, the spec, the stdlib, the guides or the code-generation reference writes an integer constant in a constant, relational or range pattern (a scan over every `name: char` binding and every `case name of` arm list that follows it; `scratch/v0345/Fwork/measure.py`). The hits over `x` in `case_pattern_range_divergence_3319_test.cpp` are programs whose `x` is `double`, `decimal`, `int` or `long`; the file's `char` rows use `'a'..'z'`.
- Banked rule-corpus rows: none mention a `char` scrutinee with an integer constant. `s19_3_*` and `s16_6_*` keep their verdicts.
- The compiler's gate (`checkCasePatternConstantRanges`, `src/sema/ExprTypecheck.cpp:15202`) returns before any check for a `char` scrutinee (`caseIntScrutineeRange`, `:136`, lists the six narrower integer types; `long`/`ulong` are handled beside it), so a `char` scrutinee with an integer constant is presumably admitted and lowered as an `int` compare today, in all three forms. Unmeasured (no compiler binary in this container); the tracker's first step is the probe.

## 7. Compiler facts and the tracker

- `caseIntScrutineeRange` (`src/sema/ExprTypecheck.cpp:136`) has no `Char` arm; `checkCasePatternConstantRanges` (`:15202`) therefore gates no pattern over a `char` scrutinee. Proposal 0203 §7 item (v) already asks for the range form's `char` gate.
- **Tracker (`spec-tracking`), filed at landing — or added to 0203's tracker as its `char` item.** Title: "§19.3 (v0.345, proposal 0204): over a `char` scrutinee reject an integer constant in a constant, relational or range pattern". Body: in `checkCasePatternConstantRanges`, when the scrutinee's primitive is `Char`, report the §5.1 pattern-constant error for every folded integer constant (a `ConstantPattern` member, a `RelOpPattern` operand, a `RangePattern` bound), naming `'A'`, `>= 'A'`, `'A'..'Z'` and `case int(c) of …` as the remedies; a `char` literal passes. Probe first: `case c of 65 => 1, _ => 0` over `var c: char := 'A';` (expected today: compiles and prints `1`; required: compile error at `65`). Rows: the three rejected forms, their three admitted `char`-literal twins, the `int(c)` scrutinee twin (admitted, `int` rule), a `|` alternative mixing `'A'` with `66` (rejected at `66`), and `case c of 65` over `var c: char? := 'A';` (rejected; `case c of 'A'` admitted).

## 8. Corpus cases to write (after landing)

- `s19_3`: `case c of 65 => …` over a `char` → `reject` (needle: the §5.1 pattern-constant message).
- `s19_3`: `case c of >= 65 => …` over a `char` → `reject`.
- `s19_3`: `case c of 65..90 => …` over a `char` → `reject` (this is 0203's planned `char` case; one case serves both proposals).
- `s19_3`: `case c of 'A'..'Z' => …, 'a' => …, >= '0' => …` over a `char` → `run`.
- `s3_5`: `c = 65` and `c < 122` over a `char` as expressions → `run` (the expression side is unchanged; a positive control).

## 9. Declined alternatives (two `declined-at-proposal` rows)

- **Option A, the status quo under 0203.** Three pattern forms over one scrutinee would read their constants by two rules, and `case c of 65` beside the rejected `case c of 65..65` has no principle behind it. Reopening bar: none stated.
- **Option C, integer constants converted to `char` by value.** §5.1 converts no integer to `char`, constant or not — `char(65)` is §5.2's explicit conversion — so pattern position would be the one position with a private conversion; a surrogate-valued or out-of-range bound would need a rule of its own; and `'A'`, `>= 'A'` and `'A'..'Z'` are the spellings. Reopening bar: a §5.1 constant conversion from integer to `char`, at which point the pattern follows it.

## 10. The exact normative edits (`scratch/v0345/F.json`)

Ids `F-0204-spec-19.3-ordered-char`, `F-0204-spec-C.7`, `F-0204-cg-7.1`, `F-0204-rationale-11.11`, `F-0204-readme-row`, `F-0204-declined-at-proposal-rows`.

> ▲ **§19.3** (appended to *Ordered scrutinee types*). Over a `char` scrutinee — nullable or not, read at its underlying type, as an integer one is (*Constants*, above) — every pattern constant is a `char`: a constant pattern's constant, a relational pattern's constant operand and a range pattern's bounds convert to `char` under [§5.1], which converts no integer to `char`, so `case c of 65`, `case c of >= 65` and `case c of 65..90` are compile errors at the pattern — written `'A'`, `>= 'A'` and `'A'..'Z'`, or matched over an explicit `int(c)` — while the expression `c = 65` keeps [§3.5]'s promotion, as `b > 300` does beside the rejected `case b of > 300`.

> ▲ **Appendix C.7** (a bullet before the record-destructuring bullet). Over a `char` scrutinee, nullable or not, a constant pattern's constant, a relational pattern's constant operand and a range pattern's bounds are `char` constants; an integer constant there is a compile error at the pattern ([§19.3]).

> ▲ **Code-generation reference §7.1** (one `// WRONG` line after the `char`-arithmetic line). `case ch of 65 => ... end;   case ch of >= 65 => ... end;   case ch of 65..90 => ... end;   // WRONG over a `char` `ch`: a pattern constant is read at the scrutinee's type and no integer converts to `char` (§19.3, §5.1) — `'A'`, `>= 'A'`, `'A'..'Z'`, or `case int(ch) of …`; the EXPRESSION `ch = 65` is admitted (§3.5)`

**Composition note.** Proposal 0203's new Appendix C.7 bullet ends "as a range pattern's bounds do over a `char` scrutinee". With this proposal the clause is true but narrower than the rule; the coordinator may read it as "as every pattern constant does over a `char` scrutinee (the bullet below)". Lane F does not edit 0203's text. (Applied at the v0.345 composition, in the bullet and in 0203's ▲ text.)

**Changelog bullet (draft).**
- **§19.3 *Ordered scrutinee types*, Appendix C.7, rationale §11.11, the code-generation reference §7.1: over a `char` scrutinee every pattern constant is a `char` ([proposal 0204](proposals/0204-char-pattern-constants-are-chars.md); surfaced by 0203's critique).** 0203 reads a pattern constant at its scrutinee's type under §5.1 and so rejects `case c of 65..90` over a `char`, but its constant and relational sentences named integer scrutinees only, leaving `case c of 65` as §8.4's promoted `int(c) = 65`. The three forms now read one rule: an integer constant over a `char` scrutinee is a compile error at the pattern, spelled `'A'`, `>= 'A'`, `'A'..'Z'` or matched over `int(c)`. The expression `c = 65` is unchanged. **Declined:** the split (status quo); integer bounds converted to `char` by value. Two `declined-at-proposal` rows. Migration: *Newly rejected:* an integer constant in a constant or relational pattern over a `char` scrutinee — 0 sites in `tests/`, the corpora, the guides and the stdlib. *Spec-ahead-of-impl:* the compiler gates no pattern over a `char` scrutinee (`caseIntScrutineeRange` has no `Char` arm); tracker filed at landing, or 0203's tracker extended.

## 11. Self-critique (adversarial pass)

- **Does it reopen 0203?** No. 0203's principle — a pattern constant converts to the scrutinee's type under §5.1, or is an error — is applied to the two forms 0203's sentences did not reach. 0203's `char` range cell keeps its verdict.
- **Over-rejection.** Zero sites. A program that compares a `char` against an integer code point in a `case` writes `case int(c) of 65 => …`, one conversion at the scrutinee. A `char` constant in every escape form (`'A'`, `'\U0001F600'`) is a `char` and is unaffected.
- **Nullable and narrowed scrutinees.** A `char?` scrutinee is already rejected at a relational or range pattern (§19.3's nullable rule) and takes a constant pattern as §8.4's null-safe `=`; the new sentence reads the constant at `char` there too (`case c of 65` over a `char?` is rejected, `case c of 'A'` admitted) — stated in the sentence itself ("nullable or not, read at its underlying type"), as *Constants* states it for integer scrutinees, since the paragraph's "every position the two patterns take" covers the relational and range forms and a narrowed scrutinee, not a constant pattern over an un-narrowed `char?` (critic fix). A scrutinee §7.4 has narrowed non-null is a `char`. Both follow from "every position the two patterns take" in the same paragraph.
- **Destructuring slots.** A `char` slot (`Pixel(var x, 'A')` over `record Pixel(X: int, C: char)`) is a constant slot, §19.3's `=` test under the slot's declared type; the paragraph's "a destructuring slot, whose tested type is the slot's type" carries the new sentence to it, so `Pixel(var x, 65)` is rejected too. Zero sites.
- **Every other passage.** §3.5's `c < 122` sentence is expression-level and stays true; §8.4 is untouched; §16.3's `Range<char>` says nothing about integer bounds; 0203's §16.1 target-typing at a range pattern's scrutinee already converts bounds to `char`; stdlib §9.8's "`Contains` agrees with range patterns on the stepless forms" stays true (a `Range<char>` value's `Contains` takes a `char`). Guides: none matches a `char` against an integer.
- **History narration.** The normative edits state the rule; the story is in the rationale paragraph and the changelog.
