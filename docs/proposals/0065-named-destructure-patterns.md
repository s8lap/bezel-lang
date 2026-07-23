# Proposal 0065 — Named-destructure patterns: `T(Prop = Pattern, …)`

| | |
|---|---|
| **Status** | draft — RFC for owner review (unconfirmed, owner-gated), 2026-07-24. The *direction* is already accepted: the delegated, critic-gated ruling recorded on [#1409](https://github.com/s8lap/bezel-lang/issues/1409) (2026-07-24, against spec v0.220) accepted the named-destructure form with the `=` spelling and fixed the v1 restrictions; this document writes that acceptance up in full, per the ruling's own "proposal needed" verdict. It does not reopen the settled forks (§3). |
| **Tracking issue** | [#1409](https://github.com/s8lap/bezel-lang/issues/1409) (split from RFC [#812](https://github.com/s8lap/bezel-lang/issues/812) item 3) |
| **Target** | Spec [§19.3](../Bezel_Language_Spec.md#sec-patterns) (the named form joins the pattern catalog: syntax, name resolution, binding), [§19.6](../Bezel_Language_Spec.md#sec-exhaustiveness) (one coverage clause), [§35.5](../Bezel_Language_Spec.md#sec-case-patterns) (*Case patterns*: the named form over payload fields), Appendix **A.7** (the destructuring production — delta in §8), Appendix C.7 (one invariant bullet); rationale (new record alongside §3.8: the `=` spelling, the declined braces/`:=`); Guide 03 gains an example at graduation (revalidation pin). **No stdlib change. No new tokens.** |
| **Written against** | spec v0.220 |
| **Origin** | Owner comments on #1409 (2026-07-03): the C#-style brace sketch withdrawn under rationale §5.16's recorded no-`{}` posture; then the `:=` spelling withdrawn for `=`. Delegated ruling 2026-07-24 (full record on the issue thread). |
| **Depends on** | Nothing. Implementation is sequenced **after** the R0 bug-drain and the #2079 v0.207+ syntax catch-up family (§7) — this proposal's acceptance schedules nothing. |

*Reference convention: unprefixed §-references are language-spec references.*

---

## 1. Motivation

**Classes have no destructuring surface, deliberately — but they do have a match surface.** §19.3 (v0.220) confines positional destructuring to records and choice cases: primary-constructor parameters are not promoted, positional shape is not part of a class's pattern-match surface, and rationale §5.17 declines class *deconstruction* outright because extraction and disposal are opposite operations on the same slots. None of that reasoning touches *interrogation*: a class's **public readable properties** are exactly its declared, encapsulation-respecting match surface, and reading them transfers nothing. Today the only way to match on them is bind-then-access:

```bezel
// current syntax (v0.220) — excerpt of a probe-validated program (sealed
// hierarchy Shape with leaves Rect{Width,Height} and Circle{Radius})
var kind := (case s of
  var r: Rect when r.Width = r.Height => $"square {r.Width}",
  var r: Rect => $"rect {r.Width}x{r.Height}",
  var c: Circle => $"circle {c.Radius}");
```

Every arm binds the whole object to say something about one or two properties; nested conditions become guard chains; the property names — the actual subject of the match — sit in the guard, away from the pattern. The named-destructure form puts the interrogation in the pattern:

```bezel
// proposed
var kind := (case s of
  Rect(Width = var w, Height = var h) when w = h => $"square {w}",
  Rect(Width = var w, Height = var h) => $"rect {w}x{h}",
  Circle(Radius = var r) => $"circle {r}");
```

**The construction/matching duality carries the semantics.** Call sites construct by name with `:=` (`new Rect(Width := 3)`, §22.5); a pattern *interrogates* — "is `Width` 3?" — which is precisely what `=` means in Bezel (§8.4). The `:=`/`=` asymmetry between putting a value in and testing a value is deliberate and load-bearing: for the constant case the pattern reads literally as the equality test it is:

```bezel
// proposed — record scrutinee; constant slots are `=` tests, per-slot alternatives compose
record Header(Version: int, Body: string);

case h of
  Header(Version = 1, Body = var b)     => HandleV1(b),
  Header(Version = 2 | 3, Body = var b) => HandleV2(b),
  _                                     => HandleUnknown()
end;
```

**Borrow-only sidesteps the §5.17 unsoundness by construction.** Pattern bindings in Bezel are non-destructive borrows sourced from the scrutinee (§19.3); a named slot is a property *read* — a §28.3 producers-table borrow — so nothing is extracted, no disposal interleaving arises, and the feature needs no new ownership rule (§4).

The form is the minimal completion of the existing grammar: the same parentheses, zero new tokens, one new list shape inside them, composing with per-slot `|` alternatives (v0.167) and nesting (`Rect(Size = Size(W = var w))`) for free.

## 2. The form

### 2.1 Syntax and parse commitment

```
Pattern          ::= … | QualifiedName "(" (PositionalPatternList | NamedPatternList) ")"
NamedPatternList ::= NamedSlot ("," NamedSlot)* ","?
NamedSlot        ::= Identifier "=" AltPattern
```

A destructure's slot list is **either** positional **or** named — the no-mixing rule (v1, liftable) is structural, not a semantic check. At least one slot is required (as today).

Collision analysis (from the owner's recorded check, re-verified against v0.220):

- **`=` is not a pattern relop.** `RelOpPattern` admits only `<` `>` `<=` `>=` (A.7 — a bare constant already means equals), so `=` has no reading inside a pattern today.
- **`=` cannot occur inside a slot's embedded expressions.** Pattern-embedded expressions — relational operands and range bounds — parse at **Shift** level or tighter (A.7 `PatternBound`, `RelOpPattern Shift`); equality sits at level 11, six levels looser (§17.1). So no pattern-interior expression can swallow an `=`.
- **Two-token commit.** After `QualifiedName "("`, an `Identifier` followed by `=` commits to `NamedPatternList`; every other continuation is positional. No current continuation of an identifier slot is `=`: a complete pattern is followed only by `,`, `)`, or `|`, and `Identifier <` continues a generic type-test, not a named slot. A *qualified* name before `=` (`Rect(Color.Red = …)`) does not parse — named slots are plain identifiers.
- **The guard `=` sits after the pattern.** `Rect(Width = var w) when w = h => …`: the guard's `=` is inside the `when` expression, outside the parentheses; no interaction.

### 2.2 Name resolution

A named slot's `Identifier` resolves against the pattern's type, by kind:

- **Record** — the positional fields' auto-generated properties (§9.1) and the body's auto-properties (§9.2). Computed (expression-bodied) record properties are **not** named-slot targets in v1 (storage-only, the same set `with` targets, §9.5; liftable — §5).
- **Choice case** — the case's payload field names (A.3 `CaseField`; §35.5). The pattern's head names the case (with the §19.8/§35.5 prefix omission), not the choice type.
- **Class** — the class's **public readable instance properties**, computed or stored, declared or inherited (§23). This is the semantic point of the feature: the named form is the by-name match surface positional destructuring deliberately never gave classes (§19.3).

Errors at the pattern, each naming its rule: an unknown name; a duplicate name in one slot list; a write-only property (§23.3); an indexer (§23.4); a static member; a field or method (properties only, on classes). Enums and primitives have no named-destructure form (they have no slot surface; their patterns are unchanged). An **interface**-typed pattern head is not admitted in v1 (§5, liftable) — the ruling's resolution list is records, choice cases, classes.

Visibility is the ruling's rule as fixed: the class resolution set is the *public* readable properties. Whether privileged sites (inside the declaring class) may name `private`/`protected` properties is recorded as an open question (§5), not silently decided.

### 2.3 Matching semantics

Matching `T(N1 = p1, …, Nk = pk)` proceeds:

1. **Type test first**, identical to the positional form: the scrutinee is tested against `T` (subtype matching for class hierarchies; case-tag test for a choice case). A named pattern therefore **never matches `null`** (§19.5) and no slot is read on a failed type test.
2. **Per-slot reads in textual order**, short-circuiting on the first refuted slot. Each read is the ordinary member read: a record field copy/borrow, a choice payload read, or a **class property getter invocation**. Getters *run* during matching; §19.4's first-match arm order makes the evaluation order deterministic and observable. Getters are expected to be cheap and effect-free, but this is a convention, not a checked rule (a `pure` getter is checked as always, §22.15); a getter that throws propagates out of the `case` construct from the arm being tried. Contract-bearing getters keep their normal §22.16 semantics.
3. **Slot patterns are full patterns** (`AltPattern`): bindings (`var w`), `_`, constants, `null`, ranges, relationals, per-slot `|` alternatives among binding-free members (v0.167 rules unchanged), type tests, and nested destructurings — named or positional — recursively.
4. **A constant slot is the equality test** `scrutinee.N = c` under the slot's declared type's `=` (§8.4, null-safe; lifted per §7.11 for nullable slots). A constant slot whose declared type does not bear `=` (e.g. a non-auto-equality-bearing record, §9.2) is a compile error at the pattern — the same use-site discipline as writing the comparison out.
5. Guard evaluation and arm selection are unchanged (§19.4).

One deliberate reading in v1: a **naked identifier on a named slot's right-hand side is *not* a binding.** `Rect(Width = w)` resolves `w` at the top-level-pattern rule — a constant reference or a type test by name resolution (§19.3/A.7), so it is an equality test when `w` names a `const`. The positional shorthand ("a naked identifier binds") is scoped to positional slots, where there is no `=` to misread; directly after `=` — the interrogation operator — a silent binding would invert the reading. Bindings in named slots are spelled `var w`. (F#/OCaml spell record-pattern bindings without `var`; the divergence is deliberate and recorded — §3. Flagged for owner confirmation, §5.)

### 2.4 Bindings and ownership

§19.3's pattern-binding ownership rule applies **verbatim**: every named-slot binding is a **borrow**, non-destructive on the scrutinee — for a class property, a borrow whose source is the scrutinee (§28.3 producers, the instance-getter row); for a record/choice payload of value type, a copy; for `string`/runtime-managed, a shared reference. No move ever happens through a pattern; getters may not return fresh owned values in the first place (§28.6), so no slot read can mint an obligation. The consume-site freeze and the scrutinee rules of §28.3/§35.5 are untouched.

In an **owning match** (§35.5), named slots do **not** transfer: transfer into arm bindings is the positional form's announced behavior over `owned` payload fields, and this proposal's bindings are borrows everywhere. An owned payload matched by a named slot behaves, for the disposal trace, exactly as a discarded slot: borrowed during the arm, disposed at construct completion by the standard §35.5 machinery. A program that wants the payload transferred writes the positional destructure. (Ruling-silent point, resolved conservatively from the fixed borrow-only restriction; recorded liftable — §5.)

The v0.209 arm-position transfer marker (`owned x` as an arm *result*) is orthogonal and unchanged: it applies to named owned bindings, and a pattern binding is a borrow, never markable.

### 2.5 Exhaustiveness

The §19.6 contribution rule, as fixed by the ruling:

> An unguarded arm whose pattern is `T(N1 = p1, …, Nk = pk)` contributes to static exhaustiveness **exactly where an unguarded arm covering `T` by the existing rules would** — a type pattern for a sealed-hierarchy leaf (including a determined generic instantiation, v0.196) — or, for a choice case, where the v0.200 all-slots-irrefutable positional destructure would — **if and only if every named slot is irrefutable** (a binding or `_`). Any refutable slot (a constant, `null`, range, relational, alternative list, type test, or nested destructuring) makes the arm contribute **nothing**.

This is the same one-level-deep posture as the v0.200 choice bullet: coverage is counted per type/case name; there is **no per-property value-space analysis** in v1 (`Rect(Width = 1..100)` arms never sum to coverage of `Rect`, whatever the property's type). Guarded arms contribute nothing, as everywhere (v0.182). The motivating payoff: the §1 example is exhaustive with no `_` arm — the second `Rect` arm and the `Circle` arm are unguarded with all-irrefutable slots, covering both leaves of the sealed hierarchy, which today only bare type patterns could do.

### 2.6 Diagnostics

Per the teach-the-fix convention, sketches (editor latitude at landing):

```
error: `Rect` has no readable property `Widht`
  named-destructure slots resolve against the type's public readable
  properties (records: stored fields/auto-properties; choice cases:
  payload field names). Did you mean `Width`? (§19.3)

error: positional and named slots may not be mixed in one pattern
  `Rect(3, Height = var h)` — write both slots by name:
  `Rect(Width = 3, Height = var h)`. (§19.3, v1)

error: property `Cache` of type `Buffer` has no `=` in constant position
  a constant slot is the equality test `scrutinee.Cache = c`; `Buffer`
  does not bear `=` (§8.4, §9.2). Bind the slot (`Cache = var b`) and
  test in a `when` guard instead.
```

## 3. Alternatives considered and declined

1. **C#-style brace property patterns** — `Rect { Width: var w }`, the original #812 sketch. **Withdrawn by owner constraint** (2026-07-03): `{}` never becomes a bracket kind in Bezel — rationale §5.16's recorded position confines it to string interpolation, and the cost it names (a permanent fourth bracket category for one construct) applies squarely here. Braces would also introduce a *second* destructure surface where the existing parentheses suffice.
2. **The `:=` spelling** — `Rect(Width := var w)`, mirroring named arguments exactly. **Withdrawn by owner refinement** (2026-07-03): assignment reads false in interrogation position. A pattern's data flow is the reverse of construction — `new Rect(Width := 3)` puts a value in; `Rect(Width = 3)` asks whether `Width` is 3, which is what `=` means (§8.4), and the `:=`/`=` asymmetry *is* the construction/matching distinction rather than a break in symmetry. ML-family precedent (F# `{ X = x }`, OCaml record patterns) spells patterns with `=`. The collision check that makes `=` safe is §2.1.
3. **Positional + named mixing in one pattern** (`Rect(3, Height = var h)`). Declined for v1, structurally: mixing complicates arity/coverage analysis and reads ambiguously against the record positional order; call sites do mix (positional-then-named, §22.5), so the lift has precedent if evidence arrives. Liftable.
4. **Naked-identifier bindings in named slots** (`Rect(Width = w)` binding `w`, the F#/OCaml reading). Declined for v1: directly after the equality operator, a silent binding inverts the interrogation reading, and a name that happens to resolve to a `const` would silently flip a binding into an equality test (or vice versa as constants are added) — exactly the trap the explicit `var` form removes. The positional shorthand survives unchanged where it is unambiguous.
5. **Decline the feature entirely.** Declined: it would contradict the owner's recorded, actively-refined direction on #1409; the gap is real (class matching forces bind-then-access; nested property conditions force guard chains) and the cost is near-zero (no new tokens, no new ownership rule, one grammar alternative).
6. **Full accept with immediate implementation.** Declined on roadmap grounds — R0 drain and R1 stdlib first — and because the remaining surface (A.7 wiring, §19.6 wording, class-property resolution taxonomy, getter-evaluation guarantees) deserves this proposal + critique cycle rather than a direct spec edit.

## 4. Interactions

- **§19.3 pattern-binding ownership** — inherited verbatim; §2.4. **No new kernel rule**: bindings are K2 borrows sourced from the scrutinee; no K3 consuming position is added (the ruling's K-derivation: not a kernel item).
- **§19.4 guards / evaluation order** — getters run during matching in first-match arm order and textual slot order; observable effects/faults are thereby deterministic (§2.3). Guarded arms contribute nothing to exhaustiveness, unchanged.
- **§19.5 null** — type test first; a named pattern never matches `null`; no getter runs on a null scrutinee.
- **§19.6 + the v0.196 determination rule** — a named pattern covers exactly the instantiation its head names, so determined-leaf coverage composes unchanged (`Ok<int>(Value = var v)` covers the determined `Ok<int>`; an undetermined leaf stays statically unattainable). The all-slots-irrefutable clause is §2.5; the cross-module leaf-addition discipline is untouched.
- **§35.5 choice matching** — named slots resolve payload field names; the borrowing match is unchanged; the owning match does not transfer through named slots (§2.4). Payload-carrying case patterns remain inadmissible in `|` alternative lists, named form included (they are destructurings, not constants).
- **§8.4 / §9.2** — constant slots are `=` tests; the non-`=`-bearing-type error rides the existing use-site discipline.
- **§28.3 / §28.6** — property reads are the producers-table getter row (borrow, source: scrutinee); getters cannot return fresh owned values, so no slot read carries an obligation. The consume-site freeze (K5/K8.3) sees named-slot borrows as any other live borrows of the scrutinee.
- **§22.15 / §22.16** — `pure` getters and contract-bearing getters keep their own rules; matching adds no purity requirement (recorded as convention, §2.3).
- **§27.4 lambda-parameter record destructuring (v0.183, #1408)** — *not* extended in v1: lambda parameters keep the positional form only. The named form's natural admission there (all-irrefutable slots) is recorded as liftable (§5).
- **§13 visibility** — resolution honors ordinary member visibility; v1 fixes the class set to *public* readable properties (§2.2, open question on privileged sites in §5).
- **Appendix C.7** — one new invariant bullet: a destructure pattern's slot list is positional or named, never mixed; named slots resolve per §2.2's taxonomy.
- **Conformance** — Core, alongside §19 where the pattern grammar lives; no Appendix D level change.

## 5. v1 restrictions (fixed) and liftables / open questions

Fixed by the ruling (part of the accept — re-litigating needs new evidence):

1. No positional+named mixing (liftable).
2. Resolution sets per §2.2 (records: storage; choice cases: payload names; classes: public readable properties). Unknown/duplicate/write-only/indexer at the pattern are errors.
3. Bindings are borrows; getters run during matching; §19.4 order governs.
4. Exhaustiveness by the all-slots-irrefutable rule; no per-property value-space analysis.
5. Constant slots are `=` tests; non-`=`-bearing slot type in constant position is an error.

Open questions for the owner (drafting-level, not re-openings):

- **Q1 — naked-identifier RHS.** Confirm §2.3's reading (constant-or-type by resolution, bindings spelled `var`); the ruling fixed the slot RHS as "a full `Pattern`" without addressing the positional naked-binding shorthand. Recommended: as drafted (decline #4 in §3).
- **Q2 — owning-match named slots.** Confirm the borrow-only, no-transfer reading (§2.4) — ruling-silent; the alternative (rejecting named patterns under an owning-match scrutinee outright) is stricter but forbids harmless peeking. Recommended: as drafted; a transfer-through-named-slots form is liftable later.
- **Q3 — privileged-site visibility.** Should a pattern inside the declaring class resolve `private`/`protected` readable properties (as an explicit read `obj.P` would)? v1 as ruled: public only; widening is compatible.
- **Q4 — interface-typed pattern heads.** Excluded in v1 (§2.2); interfaces have readable properties, so the widening is coherent if wanted. Liftable.
- **Q5 — record computed properties.** v1 targets storage only (mirrors `with`); admitting public computed record properties would align records with classes. Liftable.
- **Q6 — lambda-parameter named destructuring.** Liftable extension of §27.4 once this lands.

## 6. Test surface

Spec-derived, at graduation (each test cites §19.3/§19.6 and this proposal): records (positional-field and body auto-properties; constant, range, alternative, nested slots); classes (sealed-hierarchy exhaustive matches with no `_`; inherited properties; getter-evaluation order and faulting getters; unknown/duplicate/write-only/indexer/static errors); choices (payload names; borrowing match; owning match non-transfer trace); nesting (named-in-named, named-in-positional and vice versa); per-slot alternatives; guard interaction; the no-mixing error; `null` scrutinees; determined/undetermined generic leaves; the naked-identifier-RHS resolution rule.

## 7. Staging and migration

1. **This proposal** → owner review → `accepted`.
2. **Graduation** is a ruling-class landing (spec-change-process §2/§6): §19.3/§19.6/§35.5/A.7/C.7 edits, rationale record, changelog entry with Migration note, `check_docs.sh`, adversarial critique.
3. **Implementation tracker** filed at graduation — "impl: named-destructure patterns `T(Prop = Pattern)` (§19.3/§19.6, proposal 0065)" — queued **after** the R0 drain and the #2079 v0.207+ syntax catch-up family (it touches the same pattern grammar those migrations are stabilizing). Nothing starts now.
4. **Migration: none.** Purely additive syntax — `=` in a destructure slot is a parse error today, so no existing program changes meaning; corpus impact none until landing; test pins flipped: none.

## 8. Grammar delta (Appendix A.7)

One production amended, two added. (The ruling record's skeleton cited "A.8" for this wiring; the destructuring production lives in **A.7** — A.8 is constants and literals, untouched. Drafting correction, no design content.)

```
/* current (v0.220) */
Pattern         ::= … | QualifiedName "(" PositionalPatternList ")"   /* destructuring */

/* proposed */
Pattern         ::= … | QualifiedName "(" (PositionalPatternList | NamedPatternList) ")"
                                                                      /* destructuring */
NamedPatternList ::= NamedSlot ("," NamedSlot)* ","?
NamedSlot        ::= Identifier "=" AltPattern
                              /* Named-destructure slots (proposal 0065): the
                                 slot list is positional OR named, never mixed
                                 (v1). After `QualifiedName "("`, Identifier
                                 followed by "=" commits the named reading —
                                 "=" is not a pattern relop (RelOpPattern) and
                                 cannot occur in pattern-embedded expressions,
                                 which parse at Shift level or tighter (§17.1).
                                 The slot RHS is a full AltPattern (per-slot
                                 alternatives, nesting); a naked identifier RHS
                                 resolves as at top level (constant or type
                                 test), never as a binding — bindings are
                                 spelled `var x` (§19.3). Name resolution and
                                 the borrow-only binding rule: §19.3; coverage:
                                 §19.6. */
```

`Constant` (A.8), `CaseArm`, `AltPattern`, `PositionalPatternList`, and the guard grammar are unchanged.
