# Proposal 0088 — Concrete-typed static selectors: name the remedy now, announce the classification later

| | |
|---|---|
| **Status** | accepted — stage 1 graduated v0.243 (owner-ruled 2026-08-11; stage 2 stays on the shelf, gated on the first real selector want per Q2) |
| **Tracking issue** | [#3054](https://github.com/s8lap/bezel-lang/issues/3054) |
| **Target module(s)** | **Stage 1** (cheap, immediate): spec §28.3 (*Factory methods and sourceless returns* — diagnostic text only), rationale §5.7 (the cliff recorded). **Stage 2** (owner-gated): spec §28.3 (a `sourced` return announcement: caller-side classification row, callee-side admission), §22.1/§25 return-slot syntax, Appendix A (the return-slot productions), Appendix C.3; rationale §5.7 (the lift recorded). No stdlib signature changes in either stage. |
| **Written against** | spec/stdlib v0.241 |
| **Currency note** | Re-verified clean at v0.306 (2026-09-17 proposals currency sweep): §28.3's *Factory methods and sourceless returns* heading and its producers-table rows 4 and 6 still carry the declaration-site rejection stage 1 amends; rationale §5.7 is still *Factory method ownership*; and the six `(":" "owned"? Type)` return-slot productions stage 2 amends are all present in Appendix A.4/A.5 (the v0.241 line numbers cited in §2.1 are a pinned reading aid and have since moved — the v0.287/v0.288 shared-`Member` production merge did not touch the return-slot family). No tier-1 respell, no tier-2 flag. Written-against pin unchanged. |
| **Origin** | Elegance review 2026-08-11, finding 7: `static method Pick(a: Res, b: Res): Res` is rejected at the declaration while the semantically identical `static method Pick<T>(a: T, b: T): T` compiles under the bare-type-parameter sourced-borrow rule — so the remedy for a concrete helper is to make it generic for no generic reason. Rationale §5.7 records the machinery thoroughly and the two-owners argument for the rejection is sound, but the record never confronts the resulting cliff, and the generify remedy is written down nowhere — not even in the diagnostic. |

*Reference convention: unprefixed §-references are language-spec references; K-tags are §28.3's kernel ids.*

---

## 0. The cliff, precisely

Two facts of the v0.241 text, each individually right:

1. **The concrete form is unwritable.** A static method's non-`owned` class-typed return must announce its classification (`owned`, `Task`-family, iterable convention, or a sourced bare type parameter), else the *signature* is rejected (§28.3 *Factory methods and sourceless returns*); and callee-side, *"a borrowed local or parameter with no source that outlives the call — e.g. a static method returning one of its parameters, `static method Pick(a: Res, b: Res): Res return a;` — may not be returned"* (§28.3 *Borrow lifetime rules*, which names this exact example twice). The recorded reason is the **two-owners hazard**: an unannounced concrete static signature is indistinguishable from a factory, so callers would classify the call as a fresh owned mint — a second owner for a value one of the caller's own arguments still owns, "the exactly-once invariant broken through the argument binding" (rationale §5.7, v0.194).
2. **The generic form is fine.** `static method Pick<T>(a: T, b: T): T` compiles under *Bare-type-parameter returns*: the declared bare-`T` return **is** the announcement of an argument-sourced borrow; every caller re-establishes the source set {a, b} against its own actuals (K2.4); the callee-side admission accepts parameter-sourced returns. `Math.Max(a, b)` works — §5.7's own summary clause (v0.194 paragraph, verbatim): *"bare `T` with T-reaching inputs is a **selector** and borrows (`Min`/`Max`/`Clamp`)"*; the same record states the callee side as *"classifies such calls as borrows carrying the union of the receiver's and the T-reaching arguments' source-liveness constraints."*

The cliff: the signature grammar can announce "selector" **only in generic vocabulary**. A user with a concrete two-argument chooser must either instantiate a gratuitous type parameter or restructure. Neither the rejection diagnostic nor any document states the generify remedy.

## 1. Stage 1 — the diagnostic names the remedy, and the cliff is recorded

**No rule changes.** The §28.3 declaration-site rejection gains remedy text, selected by a shape test the compiler can make locally — *does the rejected signature have at least one plain parameter whose declared type (nullability stripped) is, or derives from, the declared return type?* If yes, the selector remedy leads; otherwise the factory remedies lead:

> *"A static method's class-typed return must announce its ownership classification (§28.3). If `Pick` returns one of its arguments (a selector), make it generic — `static method Pick<T>(a: T, b: T): T where T: <constraint>` — whose bare-`T` return announces the argument-sourced borrow; if it produces a fresh value (a factory), declare `owned Res`; if it re-exposes stored state, use a static property (§28.6)."*

Rationale §5.7 gains one recorded paragraph: the concrete-selector cliff exists, the generify remedy is the sanctioned workaround, and the announcement gap (stage 2) is the recorded liftable. This stage is deliberately shippable alone: it converts an unexplained rejection into a taught one at zero rule cost.

## 2. Stage 2 — the `sourced` return announcement (designed for real, owner-gated)

### 2.1 The spelling

A **return-position marker**, parallel to `owned` in the same slot:

```bezel
public static method Pick(a: Res, b: Res): sourced Res
  return (if a.Score() >= b.Score() then a else b);
end Pick;
```

`sourced` is a **contextual keyword**, not reserved: it is recognized only immediately after the `:` of a method/operator return slot, a position where no expression or user type name is parseable ambiguously (the token is followed by a Type; a user type actually named `sourced` would be pathological but remains expressible everywhere else — the same latitude as compiler-recognized declaration clauses, the rationale §18 narrow-modifier family that `serialized` and the `when true` clause already inhabit). Alternatives to the spelling are Q3.

**Appendix A delta:** every return-slot production currently reading `(":" "owned"? Type)` — the MethodDecl family at A.4 (v0.241 lines 10261, 10300, 10304, 10310, 10353) and OperatorDecl (line 10453) — becomes `(":" ("owned" | "sourced")? Type)`, with the commentary noting `sourced` is admitted semantically only on `static` methods and operators (below). Function types (A.5's `"->" "owned"? Type`, line 10617) deliberately do **not** gain the marker in v1 — a `sourced` slot in a function-type's identity raises the same slot-classification questions §28.3's producers table resolves per-kind for concrete slots, and no motivating inhabitant exists; recorded liftable.

### 2.2 The checking rule — the bare-type-parameter rule at a concrete type

The design principle: **`sourced` makes the concrete signature mean exactly what the bare-`T` signature already means**, so both sides of the rule are transcriptions, not inventions.

**The source set.** A `sourced Res` declaration's **potential source set** is the set of **plain (non-`owned`) parameters whose declared type — nullability stripped — is `Res` or derives from it, or mentions `Res` as a type argument** (the positional analogue of the bare-`T` rule's "plain parameters whose declared type mentions the returned type parameter"; the mentions-clause admits `a: Array<Res>` as a source, matching the bare rule's treatment and the callee-side "a borrow reached through it"). `owned` parameters are never sources (consumed into the callee). A `sourced` declaration whose derived source set is **empty** is a compile error at the declaration — the marker would announce a borrow no caller could re-establish (the empty-set arm of the bare-`T` rule, which sends that shape to `owned`).

**Caller side** (a new producers-table row, transcribing the bare-`T` caller side): a call to a `sourced`-returning static produces a **borrow carrying the union of the source-liveness constraints of every argument bound to a source parameter** — usable only while every contributing source is live; a borrowed argument contributes its carried constraints transitively; an unbound-owned-temporary argument bounds the result to the same full expression (escape rejected, reject-not-extend); null-literal arguments contribute nothing; the consume-site freeze, announced-kill boundary, and every borrow-lifetime rule compose unchanged. No factory classification, no warning — the declared marker is the announcement.

**Callee side** (transcribing the bare-`T` non-empty-set admission): every return path must supply `null` (nullable return) or a borrow whose sources are drawn from the declared source set — a source parameter directly, a borrow reached through one, or static storage (process-lived; admissible here for the same reason as in the bare-`T` rule: the caller classifies the result as a borrow, so no second owner can arise). A fresh owned value, or a borrow of an owned local / `using var` / temporary, is rejected at the return site — a body producing fresh values is a factory and spells `owned`.

**Instance and extension methods:** `sourced` is rejected there in v1 — the receiver-borrow default already classifies their plain returns, and a receiver-plus-parameters source set is a widening with its own questions (the bare-`T` rule does include the receiver; the concrete instance case has had zero demand). Recorded liftable, Q4.

### 2.3 Why the two-owners hazard stays guarded

The factory rule's guard was never "concrete statics cannot lend" — it was "an **unannounced** concrete static is indistinguishable from a factory, so admitting it either mints a second owner (caller treats it owned) or requires a heuristic (the retired v0.60 posture)." `sourced` is the announcement that dissolves the indistinguishability:

- the caller-side classification is read off the declared marker with no heuristic — the factory rule's own success criterion (*"every admissible static signature is announced"*) now with one more announced kind;
- the callee-side admission rejects every fresh production, so nothing a caller could double-own can flow out;
- the unannounced concrete signature **stays rejected exactly as today** — the cliff is closed by adding a rung, not by softening the wall;
- signature evolution is inert: adding or removing unrelated parameters never flips the classification (it is marker-keyed, not shape-keyed), which is the refactor-stability property the v0.201 retirement of the v0.60 heuristic bought (§5.7) and which auto-detection would re-break (§4).

### 2.4 Worked examples, traced

```bezel
// The selector, concrete (stage 2):
public static method Pick(a: Res, b: Res): sourced Res
  return (if a.Score() >= b.Score() then a else b);   // both arms parameter-sourced borrows — admitted
end Pick;

using var x := new Res(1);
using var y := new Res(2);
var best := Pick(x, y);        // borrow, sources {x, y}: valid while both live
best.Report();
// x.Dispose();                // explicit dispose stays the announced kill w/ checked-mode backstop,
                               //   exactly as for Math.Max's result (§28.3 caller side)

Pick(new Res(1), y).Report();  // temporary-sourced: borrow confined to this statement (K8), then freed

var kept := Pick(new Res(1), y);   // ERROR: result borrow escapes its temporary source's
                                   //   full expression — bind the source first (reject-not-extend)

public static method Wrong(a: Res, b: Res): sourced Res
  return new Res(0);           // ERROR at the return: fresh owned value under a sourced
end Wrong;                     //   return — declare `owned Res` (the factory spelling)
```

Each trace was checked against the bare-`T` rule's corresponding clause; none required a rule the transcription does not carry.

## 3. Corpus evidence

The evidence for this proposal is structurally an **absence**, and is reported as such: the v0.241 rule rejects the concrete-selector *signature*, so no corpus site can exhibit the want directly — `grep -rE 'static method [A-Z][A-Za-z]*\(' ` over stdlib finds every class-typed static return announced (`owned`, `Task`-family, iterable, or bare-`T`), and the concrete selectors that exist (`Math.Max`/`Min`/`Clamp`, stdlib §2.2) are all generic — which is the finding: **the generic spelling is the only selector spelling, and the stdlib's selectors are generic whether or not genericity is meaningful to them.** The spec itself carries the unwritable shape as a worked rejection in two places (§28.3's borrow-lifetime bullet and the bare-`T` callee side, both quoting `Pick(a: Res, b: Res): Res`). No user corpus exists yet to measure demand; that is precisely why stage 2 is gated (Q2) rather than shipped — stage 1's remedy text costs nothing and stage 2's design is on the shelf for the first real want.

## 4. Alternatives considered

1. **Status quo + diagnostic only (stage 1 alone).** Viable — and it is this proposal's own recommendation *until the gate fires* (Q2). Declined as the *terminal* state: the generify remedy is real ceremony (a type parameter, a constraint to keep the marker meaningful, per-instantiation machinery) purchased to express a fact — "returns one of these arguments" — the language can plainly announce; and "make it generic for no generic reason" is a wart the rationale would otherwise carry forever.
2. **Auto-detection without announcement** — classify any concrete static whose return type matches a plain parameter's type as a selector. Declined by the announcement-by-declaration doctrine, on the record's own grounds: it reintroduces a heuristic where §28.3's design sentence is *"the caller-side classification of a static call is read off the declared return type with no heuristic"*; it makes classification **shape-fragile** (adding an unrelated `Res`-typed parameter to a factory silently flips it toward selector — the exact instance↔static-refactor-hazard class the v0.201 retirement closed); and it misclassifies real factories (`Clone(template: Res): Res` intends a fresh value; auto-detection would reject its body mysteriously instead of rejecting its signature teachably).
3. **Widen the bare-`T` rule to "any parameter type"** (let `Pick(a: Res, b: Res): Res` be classified by analogy without a marker). This *is* alternative 2 in different clothes — same doctrine violation, same fragility.
4. **A parameter-side spelling** (`static method Pick(source a: Res, source b: Res): Res`). More precise (names each source), but heavier at every declaration, novel in the grammar (a new ParamMod), and redundant in practice — the return-slot marker plus the derived source set reproduces it for every shape the bare-`T` precedent handles; the precision gap (excluding one same-typed parameter from the source set) has no known client. Recorded, declined for v1; revisitable if stage 2's derived-set rule (Q4) proves too coarse.
5. **A keyword other than a return marker** (method modifier `selector`). Declined: the classification is a property of the *return*, and the language's announcement grammar for returns is the pre-Type marker slot (`owned` there already); a modifier would put the fact in the wrong position and cost a modifier-order row.

## 5. Migration & compatibility (changelog house categories)

- **Stage 1 — doc-only + diagnostics:** no rule changes; the rejection's remedy text and the §5.7 record. Nothing newly rejected/accepted/warned.
- **Stage 2 — newly defined:** the `sourced` return marker (concrete static methods and operators), its caller-side borrow classification, its callee-side admission. **Newly rejected:** nothing that compiles today (the marker is additive; unannounced signatures were already rejected; `sourced` misuse — empty source set, instance methods, fresh-value returns — is new-code-only). **Grammar:** the A.4/A.5 return-slot productions per §2.1. **Stdlib:** no signatures change; whether `Math.Max`-style members *also* gain concrete `sourced` overloads is explicitly a non-goal (the generic forms are correct and stay).
- **Compatibility note:** a later widening of `sourced` to instance/extension returns or function-type slots is additive over this design (the liftables of §2.1–§2.2).

## 6. Implementation sketch

- **Stage 1:** diagnostic text at the factory-rule rejection site (`src/ownership` — the declaration-site static-return check; the shape test of §1 reads the signature only). Size: **XS**.
- **Stage 2:** parser (return-slot marker; `src/parser`), sema/ownership (`src/ownership`: source-set derivation from the signature; caller-side classification row — the bare-`T` caller machinery parameterized by a concrete source set; callee-side return-path admission — likewise a transcription of the existing bare-`T` check with the set supplied). The heavy machinery (union-of-source-liveness, freeze composition, temporary confinement) **exists** — it is the bare-`T` rule's, which is the design's point. Size: **M**.
- **Tests shipped with stage 2** (spec-cited; fail-without/pass-with): acceptance + liveness of the `Pick` shapes in §2.4 (including the temporary-confinement rejection and the freeze composition against consuming a source); declaration rejections (empty source set; instance `sourced`; fresh return under `sourced`); the unannounced signature still rejected with stage 1's remedy text; an operator-declaration `sourced` case; a `sourced` + nullable-return `null` path.

## 7. Open questions

- **Q1 — ship stage 1 now?** Recommendation: yes, in the next diagnostics-bearing train; it is remedy text and a rationale paragraph.
- **Q2 — stage 2's gate.** What unlocks it? Recommendation: the first concrete selector want that a real API surface files (stdlib wave or user corpus) — the same evidence-gated posture as 0051 Part B; until then the design sits here, and the §5.7 record points at it.
- **Q3 — the spelling.** `sourced` (recommended — it names the classification, matching §5.7's own vocabulary: *"bare `T` with T-reaching inputs is a **selector** and borrows"*, under the same record's *"announcement-by-declared-type doctrine"* — two clauses of the v0.194 paragraph, quoted separately), vs `borrows` (reads as a verb in a declaration slot), vs the parameter-side form (alternative 4)?
- **Q4 — the source-set derivation.** All plain parameters whose type is/derives-from/mentions the return type (recommended — the exact bare-`T` analogue), or same-type-only (tighter, excludes `Array<Res>` sources), or explicit parameter-side marking (alternative 4's precision)?
- **Q5 — operators.** Confirm `sourced` admissible on `operator` declarations (they are static; the same cliff exists for concrete operator selectors) — recommendation: yes, it falls out of the shared production.

## 8. Interactions

- **0090 (transfer-marker study):** `sourced` is a **declaration-position** announcement — under 0090's reading-model split (declaration-state vs action verb) it lands cleanly on the declaration side next to `owned`-the-state, whichever way the owner rules 0090; the two proposals were checked for collision and have none (0090 never touches return-slot markers).
- **0087:** no contact.
- **0048 / rationale §5.7 (explicit factory ownership):** this proposal is that lineage's next chapter — v0.201 made every static signature announced; v0.194 gave generics the selector announcement; stage 2 completes the announcement vocabulary at concrete types. The §5.7 record is amended, never contradicted: the two-owners argument is preserved verbatim as §2.3's ground.
- **§11.8 static interface members:** a `sourced` static interface member is admissible by the shared production; no special rule (the classification inherits through implementation exactly as the bare-`T` rule's does). Noted so the graduation text states it.
