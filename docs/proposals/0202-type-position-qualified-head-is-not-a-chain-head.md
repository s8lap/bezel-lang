# Proposal 0202 — A type position's qualified name has no chain head; a nested type ranks as a member at an expression chain head

| | |
|---|---|
| **Status** | graduated-v0.345 — superseded — graduated v0.345 (accepted 2026-10-27 as a delegated ruling under the owner's 2026-10-08 direction ("Please resolve and decide all known spec related issues. Do proposals for issues that need them.") and the 2026-10-27 direction for this train ("Post the rulings including on 7276, consider whether any clarifications are needed in the spec, and do a new train to resolve known spec issues."), [#7121](https://github.com/s8lap/bezel-lang/issues/7121), option B′ — a nested type ranks as §2.7's table ranks the enclosing type's members at each head shape: last at a chain head, where a same-named import or module declaration wins and the warning row fires, and innermost in a type position, bare or at the head of a qualified name, which is the bare-type-name row's and not a chain head; the remedy names the enclosing type's name, with its arguments where it is generic; critique round 1 applied (a qualified pattern without an argument list stays a chain head; the warning row's remedy cell; §35.1's reserved case names); landed with the v0.345 changelog entry) |
| **Tracking issue** | [#7121](https://github.com/s8lap/bezel-lang/issues/7121), with [#7085](https://github.com/s8lap/bezel-lang/issues/7085) (closed; the expression-chain-head reading this proposal confirms). One `spec-tracking` compiler tracker is filed at landing (§7). |
| **Target module(s)** | **spec**: §2.7 (the table's *Chain head*, *Bare type name* and *Warning* rows; the chain-head paragraph's remedy sentence and its closing sentence; one parenthetical in *Through nesting*), §11.6 (one clause in the type-position sentence). **rationale**: §2.4 (one dated paragraph). **proposals**: `README.md` (this row), `DECLINED.md` (three `declined-at-proposal` rows). No stdlib, guide or code-generation-reference text, no grammar change, no new keyword. |
| **Written against** | spec **v0.344** at `b4243d87f` (main). Section numbers are v0.344's. |
| **Origin** | [#7121](https://github.com/s8lap/bezel-lang/issues/7121), filed from the #7085 fix: §2.7's chain-head paragraph promises that a shadowed member "stays reachable by qualifying", and the fix measured a nested type of a generic enclosing type that no qualified spelling reaches once an import takes its name. The 2026-10-27 triage (`scratch/rule1024/R2.json`) recommended reading B (warn, with a remedy clause that names what is reachable) and flagged §2.7's "Nested types … are not reordered by this paragraph" as needing to say what it means at a chain head, since #7085 was closed on the reading that the import wins there. Lane C2 of the v0.345 train took the question further back, to the rank of a qualified name's head in a type position, and found the hole closes there. |

*Reference convention: unprefixed §-references are language-spec references at v0.344. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the standard library companion declares or declares its own; none invents an API. The former name of the language is not used anywhere in this document.*

---

## 0. Summary

§2.7's table decides a bare name's head shape "from tokens alone, before any lookup". Its *Chain head* row is "name followed by `.`", ranking the enclosing type's members "last, after aliases"; its *Bare type name* row ranks nested types innermost at "a name the grammar reads only as a `Type`". The head `C` of a type annotation `C.N` fits both descriptions. The compiler reads it as a chain head: inside `class Outer<T>` with a nested `C` and an imported `C`, a bare `: C` names the nested type and `: C.N` beside it names the import's `N`, and since §11.6 gives a type nested in a generic type no other qualified spelling, the nested `C`'s `N` is unreachable in that position (#7121, MEASURED by the coordinator). The paragraph's "stays reachable by qualifying" is false there.

This proposal settles the rank of a nested type at each head shape. At an **expression chain head** a nested type is a member like any other: last, so a same-named import or module declaration wins, the warning row fires, and the enclosing type's name — with its arguments where it is generic, `Outer<int>.C.M()` — reaches it (the reading #7085 was closed on). In a **type position**, bare or at the head of a qualified name, only types are candidates and a nested type is innermost: nothing shadows it, and `C.N` names the same `C` that `C` does. The remedy sentence is corrected to say what reaches a shadowed static or nested type. Reach: zero in-tree programs; the issue's review probes are the measured sites.

## 1. The problem

> | **Chain head** — name followed by `.` (directly, or with a balanced generic-argument group: [§22.12] 's scan) | `Color.Red;` `Registry<int>.M()` | **last, after aliases** | a reachable type outranks a same-named member; `property Color: Color` still lets `Color.Red` reach the choice type |
>
> — §2.7, v0.344 (the table)

> | **Bare type name** — a name the grammar reads only as a `Type`: an ascription, `new`, a generic argument, a base or interface list, the right operand of `is`, `is not` or `as`, a `typeof` or `default` operand, a destructuring pattern's head, a `where` clause […] | `private M: Box<int>? := null;` | **between local scope and module declarations, nested types only** | a type position names a type, and only types are candidates: a nested type of the name, declared or inherited, is the innermost answer […] |
>
> — §2.7, v0.344 (the table)

> A member shadowed at a chain head stays reachable by qualifying (`this.Data.W()`; the enclosing type's name for statics); a type shadowed by a member or local stays reachable via rule 2's module qualification. […] Nested types ([§11.6]) and [§19.8]'s scrutinee-driven pattern prefix-omission are not reordered by this paragraph.
>
> — §2.7, v0.344

> In a type position, therefore, no name passes through a generic enclosing type: a type nested in a generic type, at any depth, is named only where [§2.7]'s enclosing-type lookup reaches it, or reaches a non-generic type enclosing it below the generic one (`Node<int>` in `Tree`'s own declaration or in the body of a type deriving from `Tree`; `Inner.Leaf` there for a `Leaf` nested in a non-generic `Inner`).
>
> — §11.6, v0.344

Three things are open at once.

1. **The rank of a nested type at a chain head.** "Not reordered by this paragraph" can mean that a nested type keeps the table's rank for the head shape (last at a chain head) or that nested types are exempt from the chain-head demotion (innermost everywhere). #7085 measured the former on the tip (the import wins, printing `7`) and was closed on it.
2. **The head of a qualified name in a type position.** `C.N` in an annotation: the chain-head row's tokens ("followed by `.`") or the bare-type-name row's position ("read only as a `Type`")? The compiler takes the chain-head row. Then `: C` and `: C.N` in one body name two types, and a type nested in a generic type loses its only qualified spelling under a same-named import.
3. **The remedy sentence.** "The enclosing type's name for statics" omits nested types and omits the arguments a generic enclosing type needs (`Outer.C` is Appendix C.1's arity error; `Outer<int>.C` is the §11.6 spelling at a chain head).

The issue's readings: **A**, no warning where no remedy exists; **B**, warn, the remedy clause naming what §11.6 says is reachable; **C**, a grammar change admitting `Outer<...>.C.N` in a type position (the v0.316 deferral, `DECLINED.md`'s *Type arguments on an enclosing segment in a type position* row).

## 2. Why this is a proposal

The decision moves which type a program names: under the table's literal chain-head row, `: C.N` under a same-named import names the import's `N`; under this proposal it names the nested `C`'s. No program changes verdict in the tree, but a settled-looking table cell is re-read, so the stricter gate applies (process §2: "when unsure between two classes, take the stricter gate"). The alternative, reading B alone, would be a ruling; it leaves point 2's hole open and is evaluated below as option B.

## 3. Options

- **A — the warning is conditional** (#7121 reading A). No warning where the shadowed member has no qualified spelling. Point 2 stays as the compiler has it.
- **B — warn always; the remedy clause adapts** (#7121 reading B, the interim ruling). The warning names §11.6's reachability (the bare name inside the type's own bodies, a static through `Outer<int>.C`, a local declared without a type annotation) where no spelling exists. Point 2 stays as the compiler has it.
- **C — a grammar change** (#7121 reading C). A.5 admits type arguments on an enclosing segment, so `Outer<int>.C.N` is a type. Against the v0.316 deferral.
- **B′ — the type position has no chain head (decided).** Point 1: at an expression chain head a nested type ranks with the other members, last; the import wins; the warning row fires; `Outer<int>.C.M()` reaches it. Point 2: the head of a qualified name in a type position is the bare-type-name row's: nested types innermost, nothing shadowed, `C.N` naming the `C` that `C` names. Point 3: the remedy sentence names statics and nested types, with the enclosing type's arguments where it is generic. The warning keeps firing at expression chain heads, where a remedy now always exists.
- **D — status quo** (the compiler): point 1 as B′, point 2 the chain-head row, point 3 unchanged.

## 4. Evaluation

| | Soundness | Simplicity | Consistency | Migration reach |
|---|---|---|---|---|
| A | sound; a shadowing the programmer is not told about | a warning keyed on whether a remedy exists | the warning row's "warn" becomes conditional | 0 |
| B | sound; the hole stays (a nested type with no type-position spelling under an import) | a remedy clause with a three-way reachability text | §2.7's promise stays false in one case, now said aloud | 0 |
| C | sound | a grammar change and a new type-expression form | against the v0.316 deferral and its identity reasoning | 0 |
| B′ | sound; an added import can no longer change what a type annotation names | one rule per head shape; the remedy sentence is true everywhere | proposal 0189's posture ("a type position names a type … nested types are already found innermost first"); §22.12's derived-type reach for statics mirrored for nested types at expression heads | 0 (changed meaning in the probe shape, spec-ahead-of-impl) |
| D | sound; `: C` and `: C.N` name two types in one body | — | contradicts the bare-type-name row for a qualified head | 0 |

B′ is the smallest rule that makes every sentence of the paragraph true. It takes nothing from the expression-side design (#7085's reading, the `property Color: Color` idiom, the warning row) and removes the only case in which §2.7's "never silently change a chain that already resolved" fails for a type position.

## 5. Decision

**Option B′.**

1. A nested type ranks as the table ranks the enclosing type's members at each head shape: **last at an expression chain head** (a same-named import or module declaration wins, the warning row fires) and **innermost at a type position**, bare or at the head of a qualified name, where nothing shadows it.
2. The head of a qualified name in a type position (`C.N` in an ascription, a `new`, a generic argument, a base list, an `is` operand, a `typeof` or `default` operand, a pattern head, a `where` clause) is the bare-type-name row's, not a chain head. The chain-head row keeps every other name followed by `.`: an expression's, and a qualified pattern's without an argument list (`case C.N`), which A.7 reads as a constant or a type test by what it resolves to (critique round 1: the lane's "in an expression" wording left that pattern head in neither row).
3. A member shadowed at an expression chain head stays reachable by qualifying: `this.` for an instance member, the enclosing type's name for a static or a nested type, with its arguments where the enclosing type is generic (`Outer<int>.s`, `Outer<int>.C.M()`, §11.6).

Worked: inside `class Outer<T>` declaring `public class C` with a static `M()` and a nested `N`, under `import Lib;` whose public `C` has a static `M()`:

```bezel
var a: C := …;            // the nested C (bare type name; innermost)
var b: C.N := …;          // the nested C's N (a qualified name's head in a type position; innermost)
C.M();                    // Lib.C.M() — the import wins at the expression chain head; warning (§2.7)
Outer<int>.C.M();         // the nested C's M (the remedy: the enclosing type's name with its arguments)
```

## 6. Measurement

- In-tree programs with a nested type and a same-named import or module declaration used at a chain head or a qualified type head: **0** (`scratch/v0345/C2work/measure_names.py` over `tests/` gtest strings, lit, the rule and doc corpora, the spec, stdlib, guides and the code-generation reference; the 6965/5371/3584 nested-type suites declare no colliding import). The measured sites are #7085's and #7121's review probes (QUOTED: `/var/tmp/bezel-scratch/r35-review-semanames/probes/gen/e2_encl_128.bzl`, `e2_encl_129.bzl`, and #7121's three-row table).
- Rule corpus: `s2_7_0001` (a chain head resolves to a reachable type over a same-named member; banked `pass`), `s2_7_0003` (the warning; `pass`), `s11_6_0005`–`s11_6_0008` (no bare generic enclosing segment; the static-member and bare-name routes; all `pass`) keep their verdicts. No banked row moves. No case states point 1 or point 2 (gap, §8).
- *(Composition, v0.345.)* One banked rule case does move: `s2_7_0039` (#7085's case; `expect: warn`, banked `pass`) writes `using var i: Choice.Item := new Choice.Item();` inside `Outer`, which declares a nested `Choice` beside a module-level `Choice`. Both heads are type positions, so under point 2 they name the nested `Choice`: nothing warns and the program prints `2`. The case is not edited at landing (a banked verdict moves only with the compiler); the §7 tracker re-cuts it (`expect: run`, `stdout: 2`) and adds an expression-chain-head twin that keeps the warning leg. No other program in `tests/` or the corpora spells a qualified type-position head under a same-named module declaration or import (`scratch/v0345/comp_work/fold/scan_0202_typepos.py`).
- Guides: none writes a nested type under a same-named import.

## 7. Compiler facts and the tracker

- Expression chain head: `src/sema/ExprTypecheck_Exprs.cpp:2030` `lookupName(id->name, /*memberBeforeModuleType=*/&e != maTargetHint_)` ranks the enclosing type's members after module types at a chain head; `src/sema/TypeResolution_Names.cpp:7313–7420` `warnChainHeadTypeShadows` fires the warning row and the module-prefix warning. The #7085 closure measured the import winning at an expression chain head on the tip (QUOTED). Matches point 1.
- Type position: `src/sema/TypeResolution_Names.cpp:5430–5460` resolves a single-part name through `lookupName` (a nested type found innermost) and a multi-part name's head through the same `sym`, then walks the remaining parts as declared nested types (`:6011–6033`). #7121's table (QUOTED) measured `C.N` in a type position resolving its head to the imported `C`, so the head of a qualified type name is ranked as an expression chain head today. **Point 2 is spec-ahead-of-impl.**
- The remedy text at `TypeResolution_Names.cpp:7390–7400` prints `this.<name>`; the #7085 fix in flight (`r35-reland-7085`, unpicked) prints a nested-type remedy. Under point 3 the printed remedy for a nested type is `Outer.C` or `Outer<...>.C` (the arguments where `Outer` is generic), which always compiles at an expression chain head.
- **Tracker (`spec-tracking`).** Title: "§2.7 (v0.345, proposal 0202): the head of a qualified name in a type position is the bare-type-name row's — a nested type is found innermost at `C.N` in an annotation, whatever import or module declaration shares the name; the chain-head warning's nested-type remedy prints the enclosing type's name with its arguments". Body: in `TypeResolution_Names.cpp`'s multi-part resolution, rank the head segment as the single-part type-position lookup does (nested types innermost, before module declarations and imports); keep the expression chain head's rank (#7085); make `warnChainHeadTypeShadows`'s remedy for a shadowed nested type `Outer.C` / `Outer<...>.C`. Repro: §5's worked program (`var b: C.N` must name the nested `C`'s `N`; `C.M()` must warn and call `Lib.C.M()`).

## 8. Corpus cases to write (after landing)

- `s2_7`: a nested `C` and an imported `C`; `C.M()` at an expression chain head calls the import's (`run`, with the warning) and `Outer<int>.C.M()` the nested type's (`run`).
- `s2_7`: the same declarations; `var b: C.N := …;` names the nested `C`'s `N` (`run`).
- `s11_6`: `Inner.Leaf` in a generic `Tree<T>`'s body under an imported `Inner` names the nested `Inner`'s `Leaf` (`run`).

## 9. Declined alternatives (one `declined-at-proposal` row each)

- **Option A, suppressing the warning where no spelling reaches the nested type** (#7121 reading A). The warning's main message — the type wins for chain purposes — stands whether or not a remedy exists, and under the decision a remedy exists at every expression chain head while a type position shadows nothing. Reopening bar: none stated.
- **Option C, type arguments on an enclosing segment in a type position** (#7121 reading C). The v0.316 deferral stands (`DECLINED.md`, *Type arguments on an enclosing segment in a type position*): a grammar change whose arguments take no part in identity, and the need dissolves once a type position's qualified head is not a chain head. Reopening bar: the v0.316 row's (a recorded need a top-level declaration cannot meet, filed as a proposal).
- **Option D, the status quo** (the v0.344 compiler: a type position's qualified head ranked as a chain head). One annotation would name two types by `C` and `C.N`; a type nested in a generic type would have no spelling at all under a same-named import; the chain-head demotion exists for a member-versus-type idiom a type position cannot present (proposal 0189's posture). Zero measured sites. Reopening bar: none stated.

Option B is not declined as a direction: its warning-always half is part of B′, and its remedy clause is replaced by one that is true.

## 10. The exact normative edits (`scratch/v0345/C2.json`)

Ids `C2-7121-spec-2.7-table-chainhead`, `C2-7121-spec-2.7-table-baretype`, `C2-7121-spec-2.7-warning-row`, `C2-7121-spec-2.7-warning-row-remedy` (critique round 1), `C2-7121-spec-2.7-remedy`, `C2-7121-spec-2.7-not-reordered`, `C2-7121-spec-2.7-through-nested`, the #7121 half of `C2-7077-7121-spec-11.6-type-position`, `C2-7121-rat-2.4`, this file's README row (`C2-0202-readme-row`) and its three rows in `C2-declined-at-proposal-rows`.

> ▲ **§2.7, the table's *Chain head* row.** **Chain head** — name followed by `.` (directly, or with a balanced generic-argument group: [§22.12]'s scan), except the head of a qualified name in a type position, which is the bare-type-name row's (a qualified pattern without an argument list, which A.7 reads by what it resolves to, stays a chain head) | `Color.Red;` `Registry<int>.M()` | **last, after aliases**, nested types included | a reachable type outranks a same-named member or nested type; `property Color: Color` still lets `Color.Red` reach the choice type

> ▲ **§2.7, the table's *Bare type name* row** (the head cell, appended). … a `where` clause (a single-identifier pattern is not one: A.7 decides it by what the name resolves to) — and the head segment of a qualified name in such a position (`C.N` in an ascription), which is not a chain head

> ▲ **§2.7, the *Warning* row.** chain head resolves to a type while a same-named accessible member, a nested type included, exists | — | — | warn, *unless* … — the `property Color: Color` idiom; qualify `this.` to reach an instance member, the enclosing type's name a static or a nested type (the paragraph below)

> ▲ **§2.7, the chain-head paragraph.** A member shadowed at a chain head stays reachable by qualifying (`this.Data.W()`; the enclosing type's name for a static or a nested type, with its arguments where the enclosing type is generic — `Outer<int>.s`, `Outer<int>.C.M()` ([§11.6])); a type shadowed by a member or local stays reachable via rule 2's module qualification. […] A nested type ([§11.6]) ranks as the table ranks the enclosing type's members at each head shape: last at a chain head, where a same-named import or module declaration wins and the warning row fires, and innermost at a type position, bare or at the head of a qualified name, where nothing shadows it. [§19.8]'s scrutinee-driven pattern prefix-omission is not reordered by this paragraph.

> ▲ **§2.7, *Through nesting*.** … at the rank the table gives its members (a nested type at the rank the preceding paragraph gives it for the head shape); …

> ▲ **§11.6** (the type-position sentence's parenthetical, appended). … `Inner.Leaf` there for a `Leaf` nested in a non-generic `Inner`, the head `Inner` naming the nested type whatever same-named import or module declaration exists, since a qualified name's head in a type position is [§2.7]'s bare-type-name row's and no chain head …

**Changelog bullet (draft).**
- **§2.7 (the table's *Chain head*, *Bare type name* and *Warning* rows; the chain-head paragraph; *Through nesting*), §11.6, rationale §2.4 — a nested type ranks as a member at an expression chain head, and a type position's qualified name has no chain head (design change; [proposal 0202](proposals/0202-type-position-qualified-head-is-not-a-chain-head.md); [#7121](https://github.com/s8lap/bezel-lang/issues/7121) closes; [#7085](https://github.com/s8lap/bezel-lang/issues/7085) confirmed).** The chain-head row, read from tokens alone, put the head of `C.N` in a type annotation at the rank that lets a same-named import win, so `: C` and `: C.N` in one body named two types and a type nested in a generic type had no qualified spelling once an import took its name — against the paragraph's "stays reachable by qualifying". Now: at an expression chain head a nested type ranks with the other members, last, the import wins, the warning row fires and `Outer<int>.C.M()` reaches it (the reading #7085 was closed on); in a type position, bare or at the head of a qualified name, a nested type is innermost and nothing shadows it; the remedy sentence names statics and nested types with the enclosing type's arguments. **Declined:** suppressing the warning where no spelling reaches the type (reading A); type arguments on an enclosing segment (reading C; the v0.316 deferral stands); the status quo. Three `declined-at-proposal` rows. Kernel: no contact. Body-locality: local. Migration: *Changed meaning (spec-ahead-of-impl):* `C.N` in a type position under a same-named import or module declaration names the nested `C`'s `N`; 0 in-tree sites. *Doc-only:* the expression chain head (the compiler matches #7085's closure). **Compiler (tracked):** the type-position head is ranked as a chain head today.

## 11. Self-critique (adversarial pass)

- **Does point 2 reopen the "added import silently re-means a chain" hazard?** No; it closes it for type positions. Under the status quo, adding `import Lib;` turns `: C.N` from the nested type to the import's; under B′ nothing an import adds can preempt a nested type in a type position. At expression chain heads the warned exception stays exactly what §2.7's *Consequences* already record.
- **Is "decided from tokens alone, before any lookup" still true?** Yes. Whether a name stands in a type position or an expression is syntactic, as every row's shape is; the chain-head row is narrowed to expression heads by tokens, not by lookup.
- **Neighbouring cells.** A module-qualified type name, `Lib.C.N`, is rule 2's prefix match and unchanged. An alias-qualified head is rule 1's last step (aliases rank after module declarations and imports), so in a type position a nested type of the alias's name now outranks the alias at a qualified head, as it outranks a same-named import — the same change of meaning as the import case, 0 sites, and moot wherever §2.2's alias-collision rule ("a type declared in the file's module") already forbids the alias (critique round 1 corrected this bullet, which had called the alias rule 2's). A qualified pattern without an argument list (`case C.N`) stays a chain head: A.7 decides it by what it resolves to, so it is not a type position; in one match over a nested choice `C` shadowed by an imported `C`, `case C.Circle(var r)` (a destructuring head, a type position) finds the nested `C` and `case C.Empty` the import's — a loud mismatch against the scrutinee's type, never a silent one, remedied by §19.8's prefix omission or `Outer<int>.C.Empty`. A type parameter at a qualified head cannot arise (a type parameter has no members). `Outer.C` for a non-generic `Outer` at a type position was and is the nested `C` of `Outer` (a module-level `Outer` shadowed by a nested `Outer` is itself the bare-type-name row's innermost answer). A pattern head `C.N(var x)` is a type position (the bare-type-name row names "a destructuring pattern's head") and now finds the nested `C`; §19.8's prefix omission is untouched.
- **Does the expression-side rule break `Color.Red` on a nested enum `Color` under an imported `Color`?** The import wins at the chain head, loudly if it has no `Red`, with the warning; `Outer.Color.Red` reaches the nested enum. That is the status quo and #7085's closure; this proposal states it rather than changes it.
- **History narration.** The normative edits state the current rule; the story is in the rationale's dated paragraph and the changelog.
