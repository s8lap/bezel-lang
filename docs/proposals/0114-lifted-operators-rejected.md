# Proposal 0114 — Arithmetic, bitwise, shift, and relational operators do not lift over nullable value types

| | |
|---|---|
| **Status** | graduated-v0.267 — superseded — graduated v0.267; accepted — accepted (owner, 2026-08-20, on the evidence of [`docs/research/2026-08-20-null-lifting-investigation.md`](../research/2026-08-20-null-lifting-investigation.md), with graduation requested in the same direction). The three-lane investigation behind it is that report; this proposal is the design it carries. |
| **Tracking issue** | [#3294](https://github.com/s8lap/bezel-lang/issues/3294) |
| **Target module(s)** | **spec** — [§7.11](../Bezel_Language_Spec.md#sec-lifted-operators) (rewritten), [§6.6](../Bezel_Language_Spec.md#sec-compound-assignments) *Nullable value-typed targets* (replaced), [§19.3](../Bezel_Language_Spec.md#sec-patterns) and [§16.6](../Bezel_Language_Spec.md#sec-empty-ranges-and-equal-bound-ranges) (the companion ruling, D5), [§4.6](../Bezel_Language_Spec.md#sec-string) / [§9.2](../Bezel_Language_Spec.md#sec-body-records) (citation trims, editorial), Appendix C.1 (one bullet); **rationale** — new §6.4, the entry #3294 is half about; **stdlib** — no change; **`DECLINED.md`** — two decline-index rows. Appendix A is untouched: this proposal adds no syntax and removes no token. |
| **Written against** | spec/stdlib/rationale/changelog **v0.266** (HEAD `1107b1e`, branch `claude/single-pass-compiler-no-ast-k0x20d`). Every quotation was re-verified against that tree; every counted figure in §4 was re-derived at that tree rather than carried from the investigation memos. |
| **Currency** | v0.266. **Quoted:** spec §3.5, §4.6, §5.1, §5.4, §6.1, §6.5, §6.6, §7.4, §7.7, §7.8, §7.9, §7.10, §7.11, §8.4, §9.2, §16.3, §16.6, §17.2, §17.5, §17.6, §18.5, §19.3, §19.5, §22.15, §22.16, §25.2, Appendix C.1; stdlib §9.8; rationale §11.6; changelog v0.130. **Cited without quotation:** spec §12.7, §19.6, §21.2, §31.4; stdlib §1, §4.4. **Re-verify before graduation:** §7.11, §6.6, §19.3, §16.6, §4.6, §9.2, §22.16, §7.10, §25.2, Appendix C.1 — this proposal rewrites the first two and amends the next four. |
| **Origin** | Issue [#3294](https://github.com/s8lap/bezel-lang/issues/3294), owner-raised 2026-08-20 while reviewing v0.265's #2158 ruling, which fixed §6.6's *typing* for `byte? += 1` and **inherited §7.11's semantics without re-examining them** — the silent-decision failure `CLAUDE.md` rule 4 exists to prevent. Three parallel lanes reported 2026-08-20; the decision and the lanes' one disagreement are recorded in [`docs/research/2026-08-20-null-lifting-investigation.md`](../research/2026-08-20-null-lifting-investigation.md). |

*Reference convention: unprefixed §-references are **language-spec** references at v0.266; **stdlib**, **rationale**, **changelog**, and **process** references are marked as such. References to this document's own sections are always written "this proposal's §N". ▲ marks proposed normative text, landed nowhere. Every quotation is verbatim from the v0.266 tree; bold and italics inside quotations are the source's own.*

---

## 1. Motivation

§7.11 is a **settled rule that is not a recorded decision.** It landed at v0.130 as one of four unrelated items in a revision whose own summary calls them *"spec/stdlib additions that complete partially-stated surfaces"* (changelog:5045) — a gap-fill beside an `Array<T>` copy-form addition and a source-extension correction. The rationale has **no entry** on it: §6 *Nullability* carries exactly three entries (§6.1 field narrowing, §6.2 postfix `!`, §6.3 interpolating a nullable) and none is about operators. Nothing anywhere records why propagation was chosen, what it was chosen over, or what cost was judged acceptable. Under `CLAUDE.md` rule 4 that is the condition this proposal exists to end — in one direction or the other.

Three pieces of evidence carry the decision. They are the strongest three, not all of them.

### 1.1 The fail-open guard

```bezel
var flags: uint? := ParseFlags(header);      // null on malformed input
if (flags & ADMIN) <> 0 then
  GrantAdmin();                              // reached when flags is null
end;
```

`flags & ADMIN` lifts to `uint?` and is `null` (§7.11, *Arithmetic, bitwise, and shift operators*: *"A lifted binary operator produces `null` if *either* operand is `null`"*). `null <> 0` is `true` (§8.4: *"`null = nonNull` and `nonNull = null` are `false`"*; `<>` is the negation). **The authorization test passes on missing input.** Both spellings compile clean and disagree: `= 0` gives `false`, `<> 0` gives `true`.

The shape is not isolated. `requires(not (retries > Max))` **passes** on a null `retries` — lifted `>` is `false`, `not false` is `true` — while the positive spelling `requires(retries <= Max)` faults. Same contract, opposite outcome, chosen by spelling, and §22.16's own *"Clause granularity is semantic"* guidance makes both spellings equally idiomatic. `while spent < budget do … end;` never executes when `spent` is null. A comparator built from lifted `<` reports `null ~ 1` and `null ~ 2` while `1 < 2`, violating the total order stdlib §4.4 requires, whose stated consequence is an *"unspecified permutation"*.

**The defence #3294 recorded for the status quo is false as stated.** That defence — Bezel is not SQL, because §7.4 requires narrowing before a nullable reaches a non-nullable position — holds for *numbers*: member access, indexing, `for` sources, `Range<T>` bounds, `Console.WriteLine(Object)`, `i += n` into a non-nullable target, and `and`/`or`/`not` were each checked and each holds the line. No lifted null ever becomes a wrong `int`. But the null does not need to become a number to do damage: it becomes a **boolean**, through two total exits that require no narrowing at all — relational lifting, which yields non-nullable `bool` by construction, and §8.4 equality, which is total. A wrong `false` in a retry cap or a wrong `true` in a bit test is not a smaller failure than a wrong number.

### 1.2 §7.11 contradicts its own *Safety* paragraph, and §18.5 contradicts §7.11

**(a) Internal.** §7.11's *Safety* paragraph states, without qualification:

> "Lifting never silently produces a non-nullable value from nullable operands; it only defines what the operators *mean* on nullable operands."

Its *Relational operators* paragraph, two paragraphs above (spec:1143 against spec:1149), is the counterexample:

> "The relational operators `<`, `<=`, `>`, `>=` lift to yield **`bool`** — *not* `bool?`. A lifted relational comparison yields `false` if *either* operand is `null`."

Two nullable operands, one non-nullable `bool`, silently. The *Safety* sentence is true of the arithmetic arm it opens with and false of the section as a whole. This is a spec-internal contradiction under `CLAUDE.md` rule 1 **whatever #3294 decides** — and it is precisely the sentence the status quo's defence leans on.

**(b) Cross-section.** §18.5, defending its own null case, states (spec:3855–3857):

> "the dispatch is performed by the language with a defined null case, the same posture as the null-safe `=`/`<>` of [§8.4] — and **unlike [§4.6]'s ordering rule, where a null participant has no defensible result**, rendering one does: making it *visible*."

The spec therefore holds, in its own words, that a null participant in an ordering comparison **has no defensible result**. §4.6 acts on it (spec:622): *"Ordering requires **non-nullable** operands: an operand of static type `string?` is a compile error — narrow first"*. §7.11 asserts the opposite for value types — *"the result is simply `false`"* — and calls it a "mirror". Same operator, same absence, opposite verdicts, distinguished only by reference-vs-value kind, which has no bearing on whether an ordering of an absent value is defensible.

### 1.3 §7.11 already rejects lifting, in the same section, for the same reason

§7.11's *No three-valued logic* paragraph:

> "The boolean operators `and`, `or`, `not` ([§17.2]) operate only on `bool`; there is no lifted `and`/`or`/`not` over `bool?`, and a `bool?` operand to a boolean operator is a compile error. **Code that needs to combine `bool?` values handles the `null` case explicitly** (e.g. with `??` ([§7.7]) or a narrowing check)."

That is this proposal's rule, its remedy, and its diagnostic — already normative, already shipped, one paragraph away. §7.11 contains both postures and argues for neither; **nothing anywhere records why arithmetic differs from `and`.**

The same asymmetry is shipped a second time, in the compiler. `src/sema/ExprTypecheck_Exprs.cpp:4441–4482` rejects lifting for **user-defined** operators with the diagnostic:

> *"operator `X` does not lift over a nullable operand of a user-defined-operator type — lifting of user-defined operators over `T?` is deferred (§7.11); unwrap the operand(s) with `!` or `??` to the non-nullable type first"*

(the resolution-side gate is at `:4154–4165`). So today `total += price` silently poisons when `price: decimal?` and is a compile error naming `!`/`??` when `price: Money?`. Same program shape, same domain, opposite answers, decided by whether the author reached for a primitive or a record — and nothing records why. **The rule this proposal states, the diagnostic it proposes, and the migration idiom it names are all already shipped, for every non-primitive value type.**

### 1.4 The sticky accumulator, and why it is the *statement* that hurts

```bezel
var total: int? := 0;
for var p in prices do
  total += p;                     // one null p poisons total permanently
end for;
```

`total += p` desugars (§6.6 *Desugaring*) to `total := total + p` with the target evaluated once; `total + p` is §7.11's lifted `+`, which is `null` if either operand is; §6.6 stores that through *"the target's single write"*. Once `total` is null every later iteration reads null, produces null, writes null. Nothing in §6.6, §7.11, §17.6, or §21 diagnoses it.

Worse shapes exist and need no null input at all: `var total: int?;` is legal without an initializer (§7.9 *Default initialization*: a nullable local *"gets the type's default — `null`"*, and definite-assignment does not fire because nullable types are defaultable), so the loop above is a **statically complete no-op**. And `n++` on an `int?` is, by §6.6's *"The same reading governs the `++`/`--` forms ([§17.6])"* plus §17.6's *"`x++` is equivalent to `x := x + 1`"*, a statement whose entire purpose is to change a variable and whose defined behavior on the null path is to change nothing.

The failure postures inside one statement are also inconsistent, and §6.6 says so in one sentence: `b += 100` on `b: byte?` *"raises `OverflowException` when `b` is present and the sum leaves `byte`'s range, and assigns `null` when `b` is null."* Loud for a representational failure that loses one value; silent for a value failure that loses the entire accumulated history.

---

## 2. The design ▲

### D1 — Arithmetic, bitwise, shift, and relational operators do not lift

**Decision.** The binary operators `+`, `-`, `*`, `/`, `mod`, `&`, `|`, `^`, `<<`, `>>`, the unary operators `+`, `-`, `~`, and the relational operators `<`, `<=`, `>`, `>=` are **not defined** on a nullable value type. An operand whose static type is a nullable value type — on either side, and in the mixed `T?`/`T` pairing alike — is a **compile error**.

**Reason.** §1.1's guard is a value-level bug the type system can see; §1.2 shows the spec already holds, in two places, that the two results this rule produces (a silent `null` that accumulates, and a `false` that has no defensible claim to be the answer) are not answers; §1.3 shows the language already rejects exactly this for `bool?` and for user-defined operators, twice, with the same remedy, and records no reason for the difference. This decision removes the difference rather than defending it.

**Relational is included, and it is where the evidence is strongest, not weakest.** The retain-relational case was argued in the investigation and lost on three grounds. (i) Rejecting arithmetic alone does close §1.1's headline example at the `&` step — but it does not reach the residue: `while spent < budget`, `requires(not (retries > Max))`, and the comparator case involve no arithmetic at all. (ii) §18.5 states in the spec's own words that ordering a null *"has no defensible result"*, and §4.6 acts on it for `string?`; retaining relational lifting for value types leaves that contradiction standing. (iii) It is the construct a peer language shipped and then **removed in a breaking release** (this proposal's §6, SE-0121), with semantics strictly *better* than Bezel's.

**Keying.** The rejection is a property of the **operand's static type at that program point**, so a nullable operand proved non-null by §7.4 flow narrowing is a non-nullable operand and the ordinary operator applies unchanged; no separate rule is needed for it, and no narrowing analysis is added. (The implementation already computes this predicate at the site: `src/sema/ExprTypecheck_Exprs.cpp:5206` types the lifted result non-nullable when every nullable operand is proved present — #914. D1 changes what happens on the other branch.)

**Out of scope by construction:** nullable *reference* operands, which §7.11 never governed — §4.6 already rejects `string? < string?`, and equality on them is §8.4's.

### D2 — §8.4 equality (`=`, `<>`) is retained, unchanged

**Decision.** `=` and `<>` keep their null-safe totality: `null = null` is `true`, `null = nonNull` is `false`, `<>` is the negation. Nothing in this proposal touches §8.4, and equality is not "lifting" in the first place — §7.11 defines lifting as *"the operators are defined on `T?` operands in terms of the underlying operator on `T`"*, whereas §8.4 defines `a = b` as *"`a.Equals(b)`, with a null-safe wrapper"*, a total function that predates and is independent of `T?`.

**Reason, in three parts.**

1. **Equality over a nullable has a defensible total answer; ordering does not.** Two absences are the same absence — that is a fact about the values, not a convention — and one absence is not the same as a presence. §18.5 says exactly this: a null participant has no defensible result *in ordering*, and it draws the contrast with the null-safe `=`/`<>` it is modelling itself on. The proposal keeps the half §18.5 endorses and removes the half it denies.
2. **Equality's null case is information-preserving; relational's is a choice of one of two booleans.** `null = null` is `true` because they *are* the same absence. `null < null` is `false` because `true` would be worse — which is not a completion of anything.
3. **Every flow-analysis language surveyed keeps `==` while rejecting lifted ordering** (this proposal's §6): TypeScript rejects in the relational branch of the checker and permits equality in the other; Kotlin specifies equality with explicit null handling and rejects `compareTo` on a nullable receiver; Ada defines equality on access values with *"both are equal to the null value"* and never defines ordering for them at all; Swift's SE-0121 removed the four ordering operators and kept `==`/`!=` in the same document, saying they are *"still useful, and their results unsurprising"*.

**And removing it would make null-checking harder, not safer.** Equality is how a program *tests* for absence. `x <> null` is the narrowing test §7.4 is built on. Removing it leaves `??`, `?.`, and pattern `null` as the only spellings — strictly worse, for no gain.

### D3 — `??`, `?.`, `?(args)`, `??=`, and `!` are retained, unchanged

**Decision.** No change to §7.5, §7.6, §7.7, §7.8, or §6.6's `??=` paragraph. These are the sanctioned propagation and assertion forms and they are the migration idiom for D1.

**Reason.** They are *marked* — see D4 — and they are allocation-free: `??` and narrowing are language operators, not combinator calls. The surveyed languages' substitute idioms are closure-taking combinators (`map`, `and_then`, `?.let`), and §27.5 makes those cost an allocation (*"Captured variables are heap-allocated in a closure object"*). **The replacement idiom for this proposal is `??` plus §7.4 narrowing, and it must not become a combinator API.** This is the one place the cross-language evidence is deliberately not copied.

### D4 — The principle: *unmarked* propagation is what is rejected

**Decision.** The rule this proposal enforces is recorded, in the rationale and in §7.11's replacement text, as: **propagation is spelled, never inferred.**

**Reason, and it reframes the whole question.** Every propagating construct in Bezel already carries `?` in its token — `?.` (§7.5), `?(` (§7.6), `??` and `??=` (§7.7, §6.6). A reader who sees the token knows an absent value is being routed around. Lifted `+` and lifted `<` are the **sole unmarked exceptions**: nothing in `a + b` or `a < b` tells a reader that this operator has a null case, and §7.11 has never shown one in an example (it contains no code block).

So the principle is not *"propagation is wrong"* — Bezel propagates in four places on purpose, and every surveyed language propagates through member access. It is *"unmarked propagation is wrong"*. Rejecting the two unmarked cases makes the language's own convention exceptionless, and turns a rule that currently reads as an inherited default into a stated one. This is also the reason D2 is not an inconsistency: `=` is not propagation at all — it is a **total** function that returns an answer, not a routed-around absence.

### D5 — Companion ruling: relational and range **patterns** keep working, and §19.3/§16.6 say so directly

**Decision.** §19.3's relational patterns (`> 70`, `< 50`, …) and §16.6's range patterns (`90..100`, `0..<60`) remain **admitted over a nullable value-typed scrutinee**, with the rule stated in §19.3 and §16.6 themselves rather than delegated to §7.11:

> ▲ **Nullable scrutinees.** A relational or range pattern over a scrutinee of nullable value type is refutable and **`null` matches neither**: the pattern's comparison is performed on the present value, and an absent scrutinee simply fails the pattern and falls through to the next arm. `null` is matched by the explicit `null` pattern, by an untyped `var x` binding, or by `_` ([§19.5](#sec-type-pattern-matching-never-matches-null)).

**Reason, in four parts. This is the half of #3294's "relational needs separate treatment" that the proposal answers rather than discovers later.**

1. **A pattern is not an operator, and the harm model does not transfer.** The defect D1 removes is that `a < b` *yields a usable non-nullable `bool`* into the program's control flow — a value that composes with `not`, gets stored, gets returned, and negates into a definite `true`. A pattern yields no value. It selects an arm, in the textual order §19.4 fixes, and a non-match falls to the **next arm the author wrote**. There is no `not` to launder anything through, and no result to store.
2. **§19.5 already established exactly this posture, for exactly this reason.** *"A bare type pattern `T` matches only non-null values … `null` is matched only by the explicit `null` pattern, by untyped `var x` patterns …, or by `_`."* Patterns in Bezel are already refutable membership tests over a possibly-null scrutinee, with `null` handled by a pattern of its own. D5 extends a landed rule to the two pattern kinds that were riding a different section for it; it invents nothing.
3. **Rejecting them instead would break the natural spelling with no remedy.** `case n of > 30 => "Hot", null => "Unknown", _ => "Mild"` is the *good* program — it handles absence in the same construct, visibly. Rejecting the relational arm would force narrowing *before* the `case`, which is precisely impossible for a `case` whose whole job is to discriminate on a nullable, `null` arm included.
4. **It costs nothing to implement and it is a rule-4 gain.** The behavior already exists on a **separate code path** from the operator: `src/codegen/LlvmBackend_Exprs2.cpp:8990–9030` lowers nullable relational and range patterns as `hasValue AND comparison` (`nv.relop.match`, `nv.range.match`), independent of `LlvmBackend_Exprs.cpp:7534–7543`'s `lift.rel`. `tests/codegen/case_relop_nullable_1074_test.cpp` asserts it in its own header — *"a null scrutinee never matches a relational pattern; a present value matches iff the comparison holds"*. **That behavior is stated in no normative document today**; it is derived from §7.11 and lives only in the implementation and its regression test. D5 writes it down. Deleting §7.11's relational paragraph without D5 would leave it stated nowhere at all.

**Two residuals, stated rather than glossed.**

- *The fall-through is still a `_` arm the author wrote.* `case n of > 30 => Deny(), _ => Allow()` still admits a null to `Allow()`. That is true of every refutable pattern in the language today (§19.5 has the same property for type patterns), the `_` arm is written by the author and visible at the site, and the remedy — a `null` arm — is in the same construct. §19.6 already records that range and relational patterns *"do not contribute to static exhaustiveness proofs"*, so no exhaustiveness claim changes.
- *§19.3's named-slot rule cites §7.11 for a constant slot* (spec:4050): *"A **constant slot is the equality test** `scrutinee.N = c` … null-safe, lifted per [§7.11] for a nullable slot"*. That citation is about **equality**, which §8.4 owns outright and D2 retains; the §7.11 half is redundant today and is dropped as editorial. §9.2's admissible-component bullet (spec:1332, *"the null-safe `=` of [§8.4]/[§7.11]"*) is the same trim. **No auto-hashability, auto-equality, or serialization verdict changes.**

### D6 — Compound assignment and `++`/`--` inherit the rejection; `??=` is structurally excluded

**Decision.** A read-modify-write compound assignment (`+=`, `-=`, `*=`, `/=`, `mod=`, `&=`, `|=`, `^=`, `<<=`, `>>=`) whose left-hand side's static type, or whose right operand's static type, is a nullable value type is a compile error, at every lvalue kind (local, field, property, indexer slot). The `++`/`--` forms inherit it through §6.6's *"The same reading governs the `++`/`--` forms ([§17.6])"*. `??=` is **unaffected**.

**Reason.** The inheritance is not a separate rule: §6.6's *Desugaring* paragraph makes `x op= e` *"equivalent to `x := x op e`"*, so the compound form is rejected exactly because its expansion is. That agreement is the whole point — see this proposal's §3.2 on why rejecting the sugar alone was declined.

`??=`'s exclusion is **structural, not an enumeration**: every rule above is keyed on the term §6.6 itself uses, and §6.6 states *"**`??=`** is not a read-modify-write: `x ??= e` assigns `e` only when `x`'s current value is null"*. There is no list to get wrong. This matters because `??=` is the *anti*-lifting operator — its entire purpose is the null case — and it is half the migration idiom:

```bezel
var total: int? := 0;
total ??= 0;                        // §7.4: after `x ??= expr` with a non-nullable expr, x is narrowed
for var p in prices do
  total += p ?? 0;                  // ordinary non-nullable compound assignment
end for;
```

though the better answer, and the one the diagnostic should teach, is `var total: int := 0;` and `total += p ?? 0;` — which says at the accumulation site what a missing price means.

### D7 — One diagnostic family, naming the three remedies the language already has

**Decision.** The rejection is reported by one message family naming, in order: a §7.4 narrowing check, `??` (§7.7), and `!` (§7.8). The compound and `++`/`--` forms add the accumulator note.

**Reason.** Both halves already ship, in the shape this proposal generalizes:

- `src/sema/ExprTypecheck_Exprs.cpp:4471` (user-defined operators): *"unwrap the operand(s) with `!` or `??` to the non-nullable type first"*.
- `src/sema/ExprTypecheck.cpp:12276–12290` (§6.6's nullable-RHS rejection): *"narrow the right-hand operand first (`?? <fallback>`, `!`, or a §7.4 check) (§6.6)"*.

The second is already the exact wording for the exact remedy set, on the exact adjacent rule. Widening it from "nullable RHS into a non-nullable target" to "nullable operand at an operator" is a change of predicate, not of message design. Remedy order is deliberate: **narrowing first** (it is the only one that costs nothing at runtime and the one §7.4 trains users to reach for), `??` second (it forces the identity element to be written where the domain question lives), `!` last (it is an assertion, and §7.8 makes it fault).

### D8 — The user-defined-operator deferral closes as a **decline**

**Decision.** §7.11's *"Lifting of user-defined operators … is **deferred**; a future revision may define how (or whether) a user-declared `operator +` on a value type lifts"* becomes a recorded decline: user-declared operators do not lift, and a nullable operand at a user-defined-operator position is a compile error. Behavior is **unchanged** — the deferral has always shipped as a rejection (§1.3) — only its status changes, with a `DECLINED.md` decline-index row and a rationale record.

**Reason.** After D1 nothing lifts. Continuing to defer *"how (or whether) a user-declared `operator +` lifts"* would be deferring a question whose answer the same revision settles for every built-in operator, which is not a deferral but an unrecorded decision. Closing it also permanently closes an **ownership** question the deferral kept open: a lifted binary operator over `owned`-nullable operands would have to define what happens to the *present* operand when the other is null — dispose it, leak it, or move it into the null result — and no surveyed language answers that, because none lifts over move-only payloads.

**This is the one decision in this proposal that goes beyond the literal scope of the 2026-08-20 acceptance.** It is severable: dropping D8 leaves every other decision, every count, and every edit unchanged, and leaves §7.11's deferral sentence standing beside a section that lifts nothing. Carried to the owner as Q2 (this proposal's §9).

### D9 — The normative surface

**§7.11** is rewritten. Its replacement, in full ▲:

> ▲ **7.11 Operators and nullable value types**
>
> The built-in operators of [§3.5](#sec-operators-on-primitive-types) are defined on their operand types, not on the nullable forms of those types. For a nullable value type `T?`, an operand in an arithmetic, bitwise, shift, or relational position is a **compile error**: the binary operators `+`, `-`, `*`, `/`, `mod`, `&`, `|`, `^`, `<<`, `>>`, the unary operators `+`, `-`, `~`, and the relational operators `<`, `<=`, `>`, `>=` do not lift. The rule applies on either side and to the mixed `T?`/`T` pairing alike, and the diagnostic names the remedy: a [§7.4](#sec-member-access-on-nullable-values) narrowing check, `??` ([§7.7](#sec-null-coalescing-and-=)), or `!` ([§7.8](#sec-null-forgiving)). Because the rule reads the operand's static type at that point, an operand narrowed non-null by [§7.4](#sec-member-access-on-nullable-values) is a non-nullable operand and the ordinary operator applies unchanged.
>
> **Equality is unaffected.** `=` and `<>` are null-safe for every type, value and reference alike, per [§8.4](#sec-=-and): `null = null` is `true`, `null = nonNull` is `false`. Equality is not an operator defined in terms of an underlying operator; it is total on nullable operands and needs no narrowing.
>
> **Boolean operators.** `and`, `or`, `not` ([§17.2](#sec-notes-on-operators)) operate only on `bool`; a `bool?` operand is a compile error. There is no three-valued (`bool?`) logic. Code that combines `bool?` values handles the `null` case explicitly, with `??` or a narrowing check.
>
> **User-defined operators.** A user-declared `operator` on a record or class type ([§25](#sec-operator-overloading)) does not extend to its nullable form; a nullable operand at a user-defined-operator position is a compile error, with the same remedies (rationale [§6.4](Bezel_Design_Rationale.md#sec-null-propagation-is-spelled)).
>
> **Why.** Propagation in Bezel is **spelled, never inferred**: every construct that routes an absent value carries `?` in its token — `?.` ([§7.5](#sec-null-conditional-access)), `?(` ([§7.6](#sec-null-conditional-call-args)), `??` and `??=` ([§7.7](#sec-null-coalescing-and-=), [§6.6](#sec-compound-assignments)). An operator that silently produced a `null` result, or a `false` ordering, would route an absent value under a token that says nothing about absence, and would defer the confrontation with `null` past the point where the program can still name what the absence means. The confrontation is at the operand, in the source, once.

**§6.6**'s *Nullable value-typed targets* paragraph is replaced ▲:

> ▲ **Nullable value-typed targets.** A read-modify-write compound assignment whose left-hand side's static type, or whose right operand's static type, is a nullable value type is a **compile error** — the underlying operator is not defined on a nullable value type ([§7.11](#sec-lifted-operators)) — naming the same remedies: a [§7.4](#sec-member-access-on-nullable-values) check, `??`, or `!` (`b := (b ?? 0) + 1`). A nullable **reference**-typed target is rejected by the same rule. `??=` is unaffected: it is not a read-modify-write (below), and its purpose is the null case. The same reading governs the `++`/`--` forms ([§17.6](#sec-the-increment-and-decrement-operators)).

The rest of §6.6 is untouched, including the narrow-integer write-back, the events carve-out, the operator-overload paragraph, and the example block (which contains no nullable target). The prose instance of the old rule inside the same section — *"`b += 100` on `b: byte?` raises `OverflowException` when `b` is present … and assigns `null` when `b` is null"* — is deleted with the paragraph.

**Appendix C.1** gains one bullet ▲, beside the existing *"A nullable value may not be dereferenced without a null check or `!`"*:

> ▲ *"A nullable value type is not an operand of an arithmetic, bitwise, shift, or relational operator, in expression or compound-assignment position ([§7.11](#sec-lifted-operators), [§6.6](#sec-compound-assignments))."*

**§19.3 and §16.6** gain D5's paragraph. **§4.6** (spec:622) loses its trailing clause *"and §7.11's relational lifting is a nullable-*value*-type rule that does not extend here"* — the distinction it draws stops existing, and §4.6's own rule is unchanged. **§9.2** (spec:1332) and **§19.3**'s constant-slot sentence (spec:4050) drop the `/[§7.11]` half of their equality citations (editorial, D5).

**rationale §6.4** is new — the entry #3294 is half about. Its content is D4's principle, with D1's operator set, D2's equality reason, D5's pattern ruling, and the three declined alternatives of this proposal's §3, each with its reason. §6 currently ends at §6.3, so the number is free.

**`DECLINED.md`** gains two decline-index rows (process §9's fifth checklist item): one for lifted arithmetic/bitwise/shift/relational over nullable value types, verdict class `rejected`, authoritative record rationale §6.4 + spec §7.11 + changelog v0.NNN, with the reopening bar of this proposal's §8; one for user-defined-operator lifting (D8), verdict class `rejected`, same records.

**stdlib: zero edits.** No signature changes; the `*OrNull` tier is untouched.

---

## 3. What is **not** proposed, and why

### 3.1 A runtime fault at a lifted operation (#3294 option 4) — declined as **incoherent**, not as expensive

The owner's alternative was a runtime null-value fault instead of a compile error. It is declined, and the reason is coherence rather than cost: it would need **no new exception type** — `NullReferenceException` is already declared at §31.4 and already glossed as *"null encountered where non-null required"*, with `!` as its producer, so §31.4's admission principle is satisfied and the stdlib cost is zero. That is the one place option 4 is cheap, and it is not where the objection lives.

**It is this proposal with the diagnosis moved to production.** If `a + b` faults when either operand is null, it can never *yield* null — so its result type is `T`, not `T?`, and `var x: int? := a + b;` becomes a §5.1 widening rather than a lifted result. Every program option 4 accepts, D1 accepts or rejects at build; every program option 4 faults on, D1 rejects with a compiler message naming the remedy.

**It contradicts three landed postures.**

1. **Appendix C.1** states *"A nullable value may not be dereferenced without a null check or `!`."* Option 4 adds a third dereferencing form — an arithmetic operator — with neither. That invariant would have to be *amended*, in an appendix whose charter is *"a checklist for implementers and a reference for programmers wanting to know which classes of error are caught at compile time."*
2. **§7.8** spells the assertion: *"`x!` asserts non-nullness. If `x` is null at runtime, `NullReferenceException` is raised."* Option 4 makes `a + b` mean `a! + b!` with no token at either site — an unspelled `!` in a language that argued about which *side* of the operand the assertion goes on (rationale §6.2).
3. **§5.4 and §6.5 both take static rejection wherever it is available and fault only on the residual.** §5.4: an integer-to-enum cast is *"a **compile error at the cast site**"* for a constant operand and `OverflowException` otherwise. §6.5: an out-of-domain shift count is *"a **compile error** when the count is a compile-time constant … and `OverflowException` at runtime otherwise."* In both, the fault is what is left over when static rejection cannot reach. For nullability there is no residual: it is a **type**, not a value range, and the compiler knows it at every operand. Option 4 takes the fault where static rejection is available everywhere — the precise inversion of the pattern.

**It is also strictly larger than D1 in type-system surface.** D1 deletes forms and changes no type. Option 4 re-types every lifted expression from `T?` to `T`, silently re-typing everything downstream, and makes currently-total operators throwable: `&`, `|`, `^`, `~` and an in-domain constant shift have no fault today, and every nullable instance would become a throw site that §28.8.1's throwable-expression analysis must carry.

### 3.2 Rejecting compound assignment alone (#3294 option 2) — declined; it does not remove the trap

**It contradicts a recorded decision.** rationale §11.6 (the v0.254 paragraph, rationale:1356): *"Rejecting sugar whose normative expansion is legal would carve §6.6's equivalence for no semantic gain."* Option 2 is exactly that shape — reject `x op= e` while `x := x op e` stays legal.

**And its premise fails independently.** The sticky accumulator is reproduced verbatim by the desugaring, in the same loop:

| | `total += p;` | `total := total + p;` | trap removed? |
|---|---|---|---|
| Status quo | legal, sticky | legal, sticky | no |
| **Option 2** | **error** | **legal, sticky** | **no** |
| **This proposal** | error | error | **yes** |

A speed bump in front of an unlocked door. Option 2 also makes `counter++` an error while `counter := counter + 1` stays legal — the sugar and its expansion disagreeing, which is the carve §11.6 declined. Under D6 they agree: `counter++` is rejected *because* its expansion is.

### 3.3 The F# alternative: keep the semantics, spell them `?+?` / `?<?` — declined **with a reopening bar**

There is a fifth option #3294 does not list, and it is the best-fitting of the alternatives: keep propagation exactly as §7.11 defines it, but under a marked token. F# reached it independently. Its `FSharp.Core/Nullable.fs` defines a parallel operator family in which `?` marks *which operand may be absent* — `?+?`, `?+`, `+?`, `?<?`, `?<`, `?=?` — with **C#'s semantics exactly**: `?+?` propagates, `?<?` is `false` on null, `?=?` treats null-equals-null as true. Only the spelling differs, and it differs enough that it cannot be written by accident. F# also quarantined the family in `Microsoft.FSharp.Linq` — the SQL-facing corner — rather than putting it in the language.

**It is a real design, and it fits D4's convention better than anything else on the table**: it would make propagation marked, which is the principle this proposal enforces. It is declined on **surface cost and evidence of demand**, not on principle:

- Two operator families — eleven-plus new tokens across binary arithmetic, bitwise, shift, and relational, in both `?op?` and one-sided `?op`/`op?` forms — is a real Appendix A addition, a real precedence-table addition, and a real overload-resolution question, for a language that has been *pruning* surface (the `event` member kind, object-initializer blocks, extension-specificity rules 2 and 3, `ReadOnlyAsciiByteSpan`, four kernel axioms).
- The survey found **no** language reporting that it misses these operators, and found the explicit `??`-and-narrow form to be five characters in Kotlin and Swift — the same five characters Bezel already charges.
- The demand is unmeasured: the corpus contains zero programs that would use them (this proposal's §4), so the family would ship ahead of its first consumer.

**Reopening bar** (recorded in `DECLINED.md` and rationale §6.4): *a real Bezel program — not a translated one — in which the `??`-and-narrow form is materially worse than a marked propagating operator, together with a proposed token set that does not collide with `??`, `?.`, `?(`, or the nullable type suffix, and an account of how the marked relational operator avoids re-introducing the `not (a ?<? b)` collapse this proposal's §1.1 rejects.* That last clause matters: F#'s `?<?` returns `bool`, not `bool?`, so a marked relational operator would carry §1.1's negation defect under a better name. A reopening that keeps F#'s relational semantics does not clear the bar.

### 3.4 Everything else that is explicitly untouched

- **Any change to §8.4, `??`, `??=`, `?.`, `?(…)`, or `!`** (D2, D3).
- **Any change to nullable *reference* types.** §4.6 already rejects `string?` ordering; equality on them is §8.4's; `?.` is unchanged.
- **Any change to the `bool?` posture.** §7.11 already rejects `and`/`or`/`not` over `bool?` and §21.2 already rejects a `bool?` condition; both are restated, not amended.
- **Any stdlib addition or signature change**, in particular any `Option.map`-shaped combinator API (D3's reason).
- **A warn-first ramp.** A ramp exists to give existing users time to migrate; there are none, and process §2 is explicit that the Ratchet class *"exists only for a tightening a prior revision already ruled and scheduled"* — nothing scheduled this. Straight rejection, one revision. Said here so the absence is a decision, not an omission.
- **A staged landing** (expressions first, compound second, or bitwise first). Any split leaves an intermediate revision in which `total += p` and `total := total + p` disagree — the incoherent state §3.2 declines to make permanent.
- **§16.4's step diagnostics, §19.6's exhaustiveness, §9.2's auto-hashability, §17.14's constant sublanguage.** None moves; see this proposal's §5.

---

## 4. Costs, counted at v0.266

Every figure below was re-derived against HEAD `1107b1e`, not carried from the investigation memos. The method is stated per row because two of the three counts are scanner upper bounds.

### 4a. Normative documents and the conformance corpus — **zero**

| Artifact | Sites requiring migration | Method |
|---|---|---|
| `Bezel_Language_Spec.md` code blocks | **0** | every fenced block scanned for a nullable-value-typed name adjacent to an arithmetic, relational, or compound operator |
| `Bezel_Standard_Library.md` code blocks | **0** | same scan |
| all six guides, code blocks | **0** | same scan |
| `tests/conformance/cases/*.bzl` (**571** files) | **0** | same scan; the 12 files that declare a nullable value type were then read individually |

The scan's only nullable-value-typed declarations in doc code blocks are sixteen lines, every one of which is a `?.` chain, a nullable-to-nullable cast, an interpolation example, or a stdlib signature: Guide 02:33 (`var len := maybe?.Length;`), spec:764 (`var y: long? := long?(x);`), spec:1062 (`var length: int? := user?.Name?.Length;`), spec:3863 (`var x: double? := null;` — §18.5's rendering example), and twelve stdlib `*OrNull`/diagnostic-property signatures. The 12 corpus files are the extracted twins of the same material plus stdlib signature listings; **none performs lifted arithmetic, lifted comparison, or a compound assignment on a nullable target.**

**§7.11 has no code example at all** — there is no fenced block between its heading and the section break. §6.6's example block is entirely non-nullable (`b: byte`, `n: int`, `s: short`, `c: char`, `acc: byte`). §22.16's own `invariant` example is over two non-nullable `decimal` fields (spec:5396). **The rule this proposal removes has never been *shown* in any normative document, only stated.**

The consequence cuts both ways and both halves belong in the record: migration is free, **and** the corpus provides no evidence that lifting is load-bearing. Process §6's *"A deletion-shaped proposal must account for the conformance corpus"* is satisfied by the count above; no corpus case is deleted or converted.

### 4b. Prose edits

Seven spec edit sites, one section rewritten: §7.11 (replaced whole), §6.6's *Nullable value-typed targets* paragraph (replaced) plus the `b += 100` prose it contains, §19.3 and §16.6 (D5's paragraph), §4.6:622 (trailing clause dropped), §9.2:1332 and §19.3:4050 (citation trims, editorial), Appendix C.1 (one bullet). Plus: one new rationale entry (§6.4), one new rationale record for D8, one changelog entry with its Migration note, two `DECLINED.md` decline-index rows, and a guide revalidation pin move with **zero example edits** (0 affected examples, §4a).

Verified cross-reference inventory, so nothing dangles: `sec-lifted-operators` is referenced from exactly five spec sites plus its own anchor — 622 (§4.6), 911 and 919 (§6.6), 1332 (§9.2), 4050 (§19.3) — and from **zero** sites in the stdlib, the rationale, and all six guides.

### 4c. Compiler — a net deletion

| Site | Effect |
|---|---|
| `src/sema/ExprTypecheck_Exprs.cpp:5134–5210` (the §7.11 arith/bitwise/shift typing block, ending `res->nullable = !bothPresent;` at :5206) | the lifting branch becomes a diagnostic; **the presence predicate it already computes is reused unchanged** |
| `src/sema/ExprTypecheck_Exprs.cpp:4608–4616` (relational falls through to the generic `bool` rule) | gains the nullable-operand rejection, beside the existing `bool`/`string?` rejections at the same site |
| `src/sema/ExprTypecheck.cpp:~12180–12420` (§6.6 nullable compound admission, #2153) | collapses to one rejection; `reportNullableRhs` at `:12276–12290` is the wording model (D7) |
| `src/codegen/LlvmBackend_Exprs.cpp:7444–7858` (the whole `lift.both`/`lift.rel`/`lift.op`/`lift.end` lowering, including the `decimal?` and mixed-width paths) | **~415 lines deleted** |
| `src/codegen/LlvmBackend_Exprs2.cpp:8990–9030` (`nv.relop.match`, `nv.range.match`) | **untouched** — D5's path, independent of the operator (this proposal's §2, D5 reason 4) |
| `src/sema/ExprTypecheck_Exprs.cpp:4441–4482` and `:4154–4165` (user-operator gates) | reworded "deferred" → "declined"; **behavior unchanged** (D8) |

Two facts make this the cheapest of the four options to implement. The rejection path, its diagnostic, and its migration idiom **already exist** (§1.3); this widens an existing gate from user-declared types to primitives. And the analysis it needs is already run: `:5206`'s `bothPresent` is exactly "is every nullable operand proved present here?" — D1 changes the other branch of a decision the front end already makes. Options 2 and 4 both *keep* the whole lowering (option 4 adds a fault branch and re-types the result), so neither is cheaper.

### 4d. Tests — the whole real cost, and it is still growing

| | This proposal breaks | Method |
|---|---|---|
| lit `.bzl` files asserting **acceptance** | **18** (exact) | `NODIAG`-carrying files: `sema_lifted_ops_486__*` is 25 files / 16 accepts, less one non-nullable control (`int_bitand_ok_6_5`) = 15, of which 4 are relational; plus `sema_bool_ordering_1574__unary_plus_nullable_int_lifts_spec_7_11`, `sema_unary_minus_1588__unary_minus_nullable_int_lifts_spec_7_11`, and `compound_nullable_operand_2153__lifted_writeback_accepted` |
| lit `.bzl` files already asserting **rejection** whose FileCheck text changes | ~15 | the `compound_nullable_operand_2153__*` rejects (15 of 16) and `compound_bitwise_target_kind_2163__*` (4) |
| gtest blocks | **~143** (upper bound) | `BEZEL_TEST` blocks containing a nullable-primitive declaration used adjacent to an arithmetic (71), relational (28), or compound (46) operator; union 143. Regex adjacency, run without a compiler — a genuine upper bound |
| conformance cases | **0** | §4a |

Concentration is high, which makes the work tractable: `tests/codegen/lifted_operators_486_test.cpp` alone is 54 of the 143 and is *entirely* about §7.11; four compound-assignment files carry another 39 (`nullable_value_compound_1964` 14, `nullable_decimal_int_compound_2149` 11, `nullable_decimal_compound_2141` 9, `nullable_property_compound_2888` 5), with two cross-module files at 3. **`tests/codegen/case_relop_nullable_1074_test.cpp` (12 blocks) is *not* in the count — D5 preserves every one of them.**

Most of these are not deleted but **inverted**: an accept-and-check-the-value behavior test becomes a lit rejection test asserting the new diagnostic, which is the cheaper artifact. Several whole issue-regression files (#840, #1964, #2141, #2149, #2883, #2888, #2937) become moot; the landing should state per file whether it is inverted or deleted, so the push is unambiguous (this proposal's §9, Q4).

**The honest form of "affordable now and never again."** It is *not* "there are no users" — true, and cheap. It is that the implementation of this rule is **still being actively extended**: #2153 landed 16 lit files onto this exact surface at v0.265, three weeks before #3294 was filed, and #2158's ruling in the same revision fixed the typing while inheriting the semantics. Every revision that hardens lifted compound assignment increases the cost of removing it.

### 4e. The two costs that are not counts

**(i) A relational clause over a *mutable nullable field* in an `invariant` — the one shape with no free remedy.** §7.4 narrows *"local variables and `readonly` fields"* and *"never narrows mutable fields or properties"*, so:

```bezel
public class Listener
  private _port: int?;
  invariant(_port >= 0 and _port <= 65535);       // legal today; rejected under D1
end Listener;
```

has no narrowing-based remedy. Two remedies exist, both verified:

```bezel
  invariant(_port <> null and _port! >= 0 and _port! <= 65535);   // (a)
```

which is **total** — §17.2: *"`and` and `or` short-circuit: the right operand is not evaluated if the result is determined by the left"* — and **behavior-preserving**: today a null `_port` makes the clause `false and false` → `false` → `ContractViolationException`; with the rewrite `_port <> null` is `false` → clause `false` → the same exception, from the same clause. Or (b) declare the field `readonly` where the design allows, restoring narrowing. What the remedy must **not** be is a sentinel (`(_port ?? -1) >= 0`), which forces the author to invent a value outside the valid range and reads as a trick.

Cost: one conjunct and two `!` tokens, in the one position the language cannot narrow. Incidence: **zero** in the spec, the stdlib, the guides, and the corpus. This was the strongest single argument for retaining relational lifting, it is recorded here rather than in a footnote, and it is answered by a two-line remedy rather than dismissed.

**(ii) The deliberately-propagating accumulator becomes verbose.** A program that genuinely wants "the total is unknown if any input is unknown" writes four lines instead of one. That is the design, not an oversight: every language surveyed makes the *rarer, more surprising* behavior the more expensive one to write — Rust distinguishes `let t: Option<i32> = …sum()` from `let t: i32 = …flatten().sum()` by a type annotation and defaults to neither; Kotlin needs an explicit fold; SQL, the archetypal propagating language, **carves aggregates out of propagation by standard**. Bezel today makes the poisoning form free, unmarked, and shorter than the skipping form, which is the inversion this proposal corrects.

---

## 5. Interactions

**§22.16 contract clauses — no tier moves, and the v0.263 purity win is untouched.** Two spellings of a range test over a nullable exist and they behave differently, so both are stated:

- `requires(port in 0..65535)` over a nullable `port` is **already a compile error at v0.266**, by a rule that has nothing to do with §7.11: §17.5 states *"If the left operand is nullable, it must be checked before use"*, and stdlib §9.8 declares `public pure method Contains(value: T): bool` over a non-nullable `T` (§16.3 restricting `T` to the integer types and `char`). **v0.263's `Range<T>.Contains` purity ruling never enabled a nullable range test**; it enabled `requires(port in 0..65535)` on a *non-nullable* `port`, which nothing here touches.
- `requires(port >= 0 and port <= 65535)` over a nullable **parameter** is rejected under D1, with a clean, behavior-preserving remedy: `requires(port <> null and port >= 0 and port <= 65535)`, because §22.16 states *"Flow narrowing works inside clauses unchanged (`requires(x <> null and x.Count > 0)` narrows per [§7.4])."* Same outcome on a null argument (clause `false` → `ContractViolationException`), same clause, same message shape. For a **mutable nullable field** in an `invariant`, see this proposal's §4e(i).

Arithmetic in clauses (`requires(offset + length <= _capacity)` over a nullable `offset`) is rejected with the same narrowing remedy; **zero such clauses exist** in the documents or the corpus. **Prover tier: nothing moves.** §22.16's prover floor is a clause folding to `true` as a §17.14 constant expression, and §17.14's constant sublanguage is literal-rooted with nothing nullable-typed in it except a bare `null`.

**§22.15 purity — no classification changes, in either direction.** Rule 3 admits *"All built-in operators on primitives, enums, and `string`"*; that set **shrinks** under D1 and nothing becomes impure. Some expressions become unwritable; none changes purity. The §22.15 classification-by-rule set (auto-property getters, record synthesized members) never involves a nullable operator. Contrast option 4, which would make currently-total operators throwable and pull §28.8.1's throwable-expression analysis in.

**Generics — the feared blast radius is empty, and no §3.6 rule-2 carve-out is needed.** Three landed rules close it independently:

1. §25.2: *"A generic type parameter declares no operators of its own; for an operand whose static type is a type parameter `T`, the candidate set is drawn from the **static-interface constraints** on `T`"* — so `a + b` in a generic body requires `where T: INumber<T>` or another static-operator interface.
2. §7.10: *"Inside the generic body, T is non-nullable under any type-bound constraint"*, and for `where T: value`, *"A nullable value type (e.g. `int?`) does not satisfy the constraint"*. `Sum<int?>` does not instantiate today, for reasons that predate this question.
3. §7.11 keys on *"a primitive"*, and the constraint-sourced operator set of §25.2/§11.8 is not the §3.5 intrinsic set.

So lifted arithmetic in a generic body is **already a compile error at the declaration**. D1 changes nothing generic, and it *erases* a latent under-specification: §7.11's per-instantiation predicate *"is a primitive"* is not routed through §3.6 rule 2 today, and after D1 there is no per-instantiation predicate left to route.

**`??=` — structurally excluded, not enumerated.** §6.6's own wording does the work (D6). And §7.4's `??=` narrowing clause (*"likewise after `x ??= expr;` with such an `expr`"*) makes the migration one added line rather than a rewritten loop.

**§19.3 / §16.6 patterns — preserved and made normative** (D5). **§9.2 auto-hashability — a false alarm, checked**: §9.2's admissible-component bullet cites §7.11 only for the *null-safe equality* of a `T?` component, which §8.4 owns outright and D2 retains; the citation trim is editorial and no type's hashability, equality, or serialization eligibility changes.

**§16.3 `Range<T>.Start`/`.End: T?`** — the only nullable-value surface in the language whose natural consumer does arithmetic (`r.End - r.Start`). Rejected under D1; remedy `r.End! - r.Start!` after an `IsOpenEnded` test. The cost is two tokens, not a shape change: properties never narrow (§7.4), so the `int?` result already required a `!` or `??` downstream. Zero instances in the documents or the corpus.

**§18.5 interpolation — unchanged, and it becomes the *only* remaining unmarked null exit**, by design: a null hole renders as the text `null` with no dispatch, which §18.5 defends as *"keeps absent values diagnosable in output"*. That is a rendering, not a value, and it is the one place the language deliberately makes an absence *visible* rather than routing it.

---

## 6. Cross-language evidence

Sources are primary artifacts; where this session's egress proxy blocked a host, a GitHub-hosted copy of the same artifact was quoted and the substitution recorded (the investigation's cross-language memo carries the full substitution list and the unsourced items).

| Language | Arithmetic lifts? | `<` with a null operand | Nullability enforced by |
|---|---|---|---|
| SQL | yes | `unknown`; `WHERE` drops the row | runtime marker in the data |
| C# | yes | **`false`** | `Nullable<T>` struct + compiler |
| F# `Nullable` | yes — **only under `?+?`-style tokens** | `false`, under `?<?` | library + distinct operators |
| **Bezel v0.266** | **yes** | **`false`** | flow narrowing (§7.4) |
| Kotlin | no — compile error | compile error | two type universes, flow |
| Swift | no | **compile error since Swift 3** | type system, flow |
| Dart (2.12+) | no — compile error | compile error | type system, flow |
| TypeScript (`strictNullChecks`) | no — compile error | compile error | union types + flow |
| Rust | no | defined: `None < Some(_)` | type system |
| Zig / Scala | no | not defined | type system |
| Ada | n/a (no scalar null) | undefined for access types | subtypes, `not null`, `Constraint_Error` |

**Eight of eight languages that enforce nullability by flow analysis reject lifted arithmetic and lifted ordering.** The only lifters are SQL, C#, the CLR languages downstream of it, and F#'s LINQ corner.

**The predictor is *mechanism*, not vintage.** The obvious hypothesis — designed-in languages reject, retrofitters lift — is **falsified by TypeScript**, which retrofitted null-safety onto the largest legacy codebase here and still rejects lifted arithmetic (diagnostics 2362/2363) and lifted ordering (`checker.ts` routes the four relational tokens through `checkNonNullType`, diagnostics 2531/18047) while permitting `==` in the other branch. The predictor that survives: **where nullability is a static property discharged by flow analysis, rejection is free** — the compiler already knows, at the operator, whether the operand may be absent, and saying so costs one diagnostic; **where the nullable is a runtime value wrapper operators can be defined over**, lifting is the path of least resistance. C# lifted in 2005 because it had `Nullable<T>` and no flow analysis. **Bezel has C#'s representation but not C#'s excuse**: §7.4 gives it the same flow machinery Kotlin, Swift, Dart and TypeScript use, and §7.11 declines to consult it.

Four findings carry most of the weight.

1. **C#'s own designers declined to repeat it.** Given a clean sheet for nullable *reference* types in C# 8, csharplang #1865 states the lifting alternative — *"For a NVT `S?` we lift all the members of `S` to `S?` … and return null if the receiver is null"* — and rejects it, in Mads Torgersen's words: *"Would lead to an implicit chaining of null checks (if null then null) rather than early exit on null, which would … **flag the problem 'at the end' rather than on the first undesired null**"* and *"**Doesn't really make good use of null tracking; just propagates nulls.**"* That is #3294's sticky accumulator, named by the designers of the language Bezel inherited lifting from, as a reason **not** to lift. C# 8 shipped flow analysis and warnings instead, with no lifted operators at all.
2. **Swift shipped lifted ordering and removed it in a breaking release.** SE-0121 (*Remove Optional Comparison Operators*, implemented Swift 3): *"the results of comparisons with Optional values are sometimes **surprising**, making it easy to write bugs"*, with the canonical example `peeps.filter { $0.pet?.age < 6 }` — *"if you don't own a pet, your non-existent pet is considered to be younger than any actual pet"*. **Swift's semantics were strictly better than Bezel's**: `.none < .some(_)` is a genuine total order, so trichotomy and `!(a<b) == (a>=b)` both survive, where Bezel's collapse-to-`false` keeps neither. The evidence applies *a fortiori*. SE-0121 also names the accelerant — *"The most common uses of these operators involve coercion or promotion from non-optional to optional types"* — and **Bezel has that promotion** (§5.1, blessed by §7.11's mixed-operand rule). Swift kept `==`/`!=` in the same proposal, *"still useful, and their results unsurprising"* (D2). Swift paid the migration with a shipped ecosystem, in the release notorious for source breakage; Bezel pays none of it.
3. **Bezel's `<` is worse than SQL's, not merely as bad.** With a null `x`, SQL's `NOT (x < 5)` is `NOT unknown` = `unknown`, and `WHERE` keeps only `true`, so the row is **dropped**. Bezel's `not (x < 5)` is `not false` = **`true`**, so the guarded branch **fires**. SQL's three-valued logic — the thing #3294 rightly wants to avoid — is precisely what protects SQL there; Bezel took C#'s two-valued collapse, which removes the protection while keeping the propagation. Bezel inherits neither of SQL's famous defects (§7.11 already rejects `bool?` logic, and §8.4's `null = null` is SQL's `IS NOT DISTINCT FROM`, not SQL's `=`) — it inherited the one part that has no defence.
4. **SQL itself declines propagation at accumulation.** PostgreSQL's own documentation of the aggregates: *"All these functions **ignore null values** in their aggregated input"*, with the null result reserved for *"when no rows are selected"*. `SUM(col)` over a column containing NULLs is not NULL. The canonical propagating language carves out exactly the construct #3294 identified as propagation's weakest point; Rust makes propagating-sum an opt-in type annotation. Neither defaults to poisoning.

**Two more, for the two decisions that need their own precedent.** *Ada made this proposal's exact equality/ordering split in 1983*: ordering operators are *"predefined for **scalar types**"* only (ARM 4.5(2)) — access types are not scalar — while equality on access values is defined and treats null as a value, *"or if both are equal to the null value of the access type"* (ARM 4.5.2), which is §8.4 exactly. And **nobody faults at an operator**: Ada is the only surveyed language that faults, and it faults at a *dereference* (ARM 4.1(13)) where static rejection is impossible for a general access type — then spent Ada 2005 moving the check *earlier* with `null_exclusion`, its own annotation recording that it wanted to go further and could not, *"too incompatible with Ada 95 code to require."* A language wishing it had been stricter, blocked by users it already had (this proposal's §3.1).

**A note on where Bezel would be alone, stated because it is the point a reviewer should press.** No surveyed language rejects arithmetic while *retaining* relational lifting — which was the shape the retain-relational variant would have produced, and one more reason D1 covers both. Rejecting both is the shape eight languages already ship.

---

## 7. Classifications

**Body-locality (proposal [0105](0105-body-locality-invariant.md) §1; process §9's v0.262 line): `local`.** The rule reads the static type of an operand at a program point inside the body being checked, plus that body's own §7.4 flow facts. It consumes no information from inside any other declaration's body, needs no per-body fact from elsewhere, and requires no per-use re-elaboration. No generic-instantiation carve-out arises — this proposal's §5 shows lifted arithmetic in a generic body is already impossible at the declaration, so there is no per-instantiation arm to confine, and D1 in fact *erases* §7.11's one un-routed per-instantiation predicate. This is 0105's easiest classification and the checklist line is satisfied in one sentence.

**Kernel (v0.198; process §9): no contact.** No rule of §28 or §30.5 is amended, extended, or read. The operators governed are the §3.5 intrinsics over primitive value types, which carry no ownership obligation; `??`'s ownership meet rule (§7.7, §28.3's producer table) is untouched by D3; nothing changes about disposal, borrows, transfer, freezing, or domain isolation. The kernel register stands unchanged, and no K row is derived from or cited by this proposal.

One prospective kernel note, recorded because D8 is what forecloses it: had §7.11's user-operator deferral ever been resolved *in favour of* lifting, a lifted binary operator over `owned`-nullable operands would have had to state what happens to the **present** operand when the other is null — dispose it, leak it, or move it into the null result — which is a §28 question with a kernel classification of its own. D8 closes that question rather than deferring it a fifth revision. Nothing about the current register changes either way.

---

## 8. What would change this decision

Recorded so the decision is falsifiable rather than merely argued. The first item is a live gap and is stated as one.

- **Evidence that Swift regretted SE-0121.** §6 finding 2 is the single strongest external datum: a peer language shipped lifted ordering with better semantics than Bezel's and removed it in a breaking release. Its durability was **not verified**. SE-0123's rejection rationale and the 2022 "Optional comparison revisited" thread are both on `forums.swift.org`, which this session's egress proxy blocked; SE-0123's `Status: Rejected` is readable from the proposal file in the swift-evolution repository, but the core team's stated reasons are not. If Swift has since reversed itself, finding 2 weakens substantially — it does not vanish (Kotlin, Dart, TypeScript, Zig, Scala and Ada are independent), but the "shipped it and took it back" evidence would become "shipped it, took it back, and put it back." **This is the one open gap in the record; closing it needs nothing more than reaching that host.**
- **A demonstration that the `??`-and-narrow idiom is materially worse than lifting for a real Bezel program.** The survey found the explicit forms push code toward a better shape — the identity element written at the accumulation site, the decision moved to the *first* null rather than the last — but that was assessed on other languages' corpora. Bezel's corpus contains no program that would exercise either form (this proposal's §4a), so this is evidence the project cannot generate from what it has. A numeric-parsing or telemetry pipeline where `??` at every operator is genuinely noisy is the shape to look for, and if it exists it is also the reopening evidence for the F# alternative (this proposal's §3.3).
- **A consumer for whom D5's companion ruling proves expensive.** D5 is a behavior-preserving transcription with zero implementation cost, so this is unlikely — but it is the decision most likely to interact badly with a future pattern feature, and it is where to look first if one does.
- **A future revision admitting nullable value types to static-interface constraints** — an `INumber<int?>` conformance, or a `where T: value?`. That would give this rule a per-instantiation arm and move it from `local` to `carve-out` under 0105 §1. Nothing suggests it is planned; it is the one structural change that would reopen the generics analysis.
- **Appendix C.1's dereference invariant turning out to be already false elsewhere** — an existing operator that faults on null without a spelled assertion. That would weaken §3.1's objection 1 to the runtime-fault option, which would then be joining a club rather than founding one. None was found; `!` (§7.8) and `Object.ReferenceEquals` boxing (§8.4) were the near-misses and neither dereferences.

**Also unsourced, listed honestly:** C# 2.0's LDM notes are not public, so C#'s *original* rationale for the relational `false`-on-null rule is inferred from the mechanism rather than quoted — ECMA-334 states the rule without motivation, and the only recorded SQL lineage in that standard is the note attached to `bool?` (§12.15.5), not to the relationals. VB.NET was not surveyed directly (same CLR `Nullable<T>`; lifting presumed identical). Scala's `Ordering.Option` and SQL `CHECK`/`UNKNOWN` polarity were not independently verified and carry no weight in any argument above. The Ada citations came from the ada-lang-io GitHub mirror, which self-declares "not verified"; clause numbers matched but should be re-checked against `ada-auth.org` before they are quoted normatively.

---

## 9. Items carried to graduation

The design is accepted; these are landing-time questions, not open design.

- **Q1 — D5's normative home.** The companion paragraph is written once and cited twice, or written in both §19.3 and §16.6. Recommendation: state it in **§19.3** (which owns pattern kinds) and cross-reference from §16.6's *Range patterns* sentence, so the rule has one home.
- **Q2 — D8, the only decision beyond the accepted scope.** Close the user-operator deferral as a decline in this revision, or leave §7.11's deferral sentence standing? Recommendation: close it (D8's reasons). Dropping it changes nothing else in this proposal.
- **Q3 — `??` and `!` inside contract clauses and pure bodies, confirmed explicitly.** §22.15 rule 3 admits *"All built-in operators on primitives, enums, and `string`"*, and §22.16's flow-narrowing sentence already puts nullable expressions in clauses — but no text says of `??` or `!` specifically that they are admitted. Since D7 makes them the primary remedy *inside clauses*, the landing should confirm this in one sentence rather than leave it to inference. (Related and pre-existing: §7.4's establishing-context list names `if`/`elif`/`while`/`when` and not contract clauses, while §22.16 states narrowing works there. An editorial issue worth filing whatever this revision does; the §4e/§5 remedies lean on §22.16's sentence.)
- **Q4 — inverted or deleted, per test file.** Seven issue-regression families become moot (#840, #1964, #2141, #2149, #2883, #2888, #2937). Project practice (`tests/README.md`'s spec-cited regression discipline) points to **inverting** them into rejection tests that preserve the issue number; the landing should state which, per file, so the push is unambiguous.
- **Q5 — same push, or a tracked spec-ahead-of-impl delta?** Process §5 item 3 requires a tracking issue for every spec-ahead-of-impl delta. Recommendation: land the spec and file the impl issue, since the compiler change is a net deletion plus one diagnostic and does not gate the documents.
- **Q6 — the residual advisory.** §7.11's current text carries an advisory note that lifted ordering *"deliberately breaks the usual `<=`-reflexivity-of-equality **on null**"*. Under D1 the advisory has nothing to advise about and is deleted with the paragraph. Confirming this is deliberate, since it is the one sentence in §7.11 that acknowledged the hazard.
