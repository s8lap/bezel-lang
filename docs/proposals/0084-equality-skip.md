# Proposal 0084 — `equality skip`: per-member exclusion from record auto-equality, mirroring `serialized skip`

| | |
|---|---|
| **Status** | superseded — graduated v0.244 (owner-ruled 2026-08-11, all questions as recommended) |
| **Tracking issue** | [#3050](https://github.com/s8lap/bezel-lang/issues/3050) |
| **Target module(s)** | **spec** — §9.2 (the membership computation gains the clause; the disqualifier list gains the rescue), §9.3 (combined form: positional fields), §9.8 (one sentence stating the two packages' clauses are independent and both scope the shared membership computation), §35.7 (choice payload fields, per Q2), §3.6 (the auto-equality-bearing row's note; rule 3's breaking-change reach), Appendix A.3/A.4 (the `EqualityClause` production beside `SerializedClause`), Appendix B.2 (`equality` as a contextual keyword), Appendix C.1 (invariant row). **rationale** — §18's deferred entry resolves (pointer to the decision record); a new decision-record subsection. **guides** — Guide 3 gains two lines (the cache-field idiom). |
| **Written against** | spec/stdlib v0.241 |
| **Origin** | The 2026-08-11 elegance review (front-half critic, finding 4, confidence high; coordinator Tier 2 item 7), owner-accepted. The finding: rationale §18's deferral ("Asymmetric record auto-equality customization … Currently it's all-or-nothing") predates `serialized skip` (§9.8, v0.117), which changed the fix's cost from "design a mechanism" to "reuse one." |

*Reference convention: unprefixed §-references are language spec references; rationale references are marked.*

---

## 0. Thesis

Body-record auto-equality is all-or-nothing (§9.2): add one explicit field to an otherwise-structural record and `=`, `<>`, `Equals`, `GetHashCode` all become use-site compile errors. The error is the right *kind* of failure — loud, never silent reference equality — but the road back is five member declarations (`operator =`, `operator <>`, `Equals(Object?)`, `Equals(T)`, `GetHashCode` — §9.2's remedy plus §25.2's co-declaration rule), all restating "structural equality over everything except this one field":

```bezel
record Interval
  public readonly property Lo: int;
  public readonly property Hi: int;
  private _cachedLen: int;    // one cache field → the whole package gone (§9.2 disqualifier)
end Interval;
// today: Interval = Interval, .Equals, .GetHashCode are use-site compile errors (§9.2)
```

Meanwhile the *sibling* synthesis package, built on the same §9.2 membership rule, already has exactly the right affordance: `serialized skip` (§9.8) excludes one member from serialization, and even interacts with eligibility by **removing its component from consideration** — "a field that would otherwise make the type ineligible … does not disqualify the type, because it is no longer a serialized component." The language invented the per-member escape and applied it to one of the two packages.

**Proposed:** mirror it. A member may carry **`equality skip`**: it is excluded from the auto-equality membership computation — from `=`/`<>`, `Equals`, and `GetHashCode` — and, exactly like its serialization sibling, a skipped member cannot disqualify the record:

```bezel
record Interval
  public readonly property Lo: int;
  public readonly property Hi: int;
  private equality skip _cachedLen: int;    // PROPOSED — excluded; the package survives
end Interval;
// Interval(1,5) with any cache state = Interval(1,5) with any other: true.
```

## 1. The exact grammar (mirroring `serialized skip`'s placement)

§9.8's clause is grammatically `SerializedClause ::= "serialized" ( "skip" | SerializedName )`, attached at (Appendix A, v0.241): `PositionalField` (A.3), the auto-property alternative of `PropertyDecl` (A.4, the `SerializedClause?` slot), `EnumConstant` (A.3), `ChoiceCase` and `CaseField` (A.3). Proposed, beside it:

```
EqualityClause  ::= "equality" "skip"
```

attached at:

- **`PositionalField`** (A.3): `Identifier ":" Type SerializedClause? EqualityClause?` — a positional field excluded from equality but present, constructed, and rendered.
- **the auto-property alternative** (A.4): the same slot that carries `SerializedClause?`, in the fixed order `SerializedClause? EqualityClause?` (an arbitrary but stated order, keeping the grammar LL — both clauses optional, each at most once).
- **`FieldDecl`** (A.4): the production gains the clause between the modifiers and the identifier (`Visibility "static"? … "readonly"? EqualityClause? Identifier ":" Type …`) — **this is the point of the proposal**: an explicit field is §9.2's disqualifier, and the clause is what un-disqualifies it. (`serialized skip` never needed a `FieldDecl` position because explicit fields are never serialized components; `equality skip` needs it because explicit fields are exactly what breaks the package.)
- **`CaseField`** (A.3), per Q2: a choice payload field excluded from the §35.7 generated equality.
- **Not** on `EnumConstant` (an enum value is not a component), **not** on `ChoiceCase` (a case is the discriminant — skipping it is incoherent), **not** on static members (§9.2 already ignores them), **not** on computed properties or methods (they never participate; a clause there is a compile error, mirroring §9.8's inert-annotation rule: "silently ignoring it would be the kind of inert-looking-but-load-bearing annotation §18 warns against").

`equality` joins Appendix B.2 as a contextual keyword (recognized only in this clause position; an identifier everywhere else — exactly `serialized`'s row). Only the `skip` form exists: rename and `optional` have no equality analogue (there is nothing to rename and no read direction to tolerate), so the vocabulary is one word, closed.

## 2. Semantics

**The single normative statement (§9.2 amendment sketch):**

> A storage component marked `equality skip` is **removed from the auto-equality membership computation**: it does not participate in the generated `=`/`<>`, `Equals(T)`/`Equals(Object?)`, or `GetHashCode`, and it is exempt from the admissible-component-type requirement — a member whose type is inadmissible, **including an explicit field declaration**, does not disqualify the record when marked `equality skip`. Every other role of the member is untouched: an equality-skipped auto-property is still storage, still a `with` target (§9.5, the copy carrying its value), still rendered by the generated `ToString`, and still a serialized component when the type opts into §9.8 (the two packages' clauses are independent — below). The membership computation is one computation with per-package clauses: wherever this rule is reused — §9.8's body-record eligibility, §35.7's choice equality, §3.6's auto-equality-bearing property — the reuse sees the clause of *its own* package applied to **participation** (§9.8 applies `serialized skip`; this package applies `equality skip`), while the **eligibility gate** reads both packages' skips (consequence 2 below).

Worked consequences, each traced against the current text:

1. **The rescue works for the disqualifier list's first bullet only.** An explicit field with `equality skip` no longer disqualifies (that is the feature). The *second* disqualifier — an `override` of `Equals`/`GetHashCode` — is untouched and unrescuable: the author who takes over equality has taken over all of it; `equality skip` composes with the *generated* members only. A record with both an equality-skipped field and an `Equals` override is simply not auto-equality-bearing (the override governs), and the skip clause on it is **not** an error — it is meaningful to a reader and future-proof against the override's removal. (Contrast §9.8's inert-annotation error, which fires when the *type* is outside synthesis entirely; here the type is a record and the package is merely suppressed.)
2. **The eligibility gate reads both packages' skips; participation stays per-package.** §9.8's body-record serialization eligibility reuses §9.2's membership conditions; that reuse evaluates them with **both packages' skips removing their members from the structural member list** — a member skipped by *either* clause is not "a member outside the §9.2 list" for either package's eligibility check. Participation never crosses: `equality skip` does not hide a member from the wire, and `serialized skip` does not change what `=` compares — each package's participating set is governed by its own clause alone. One sentence in §9.8 states this. (The gate rule is Q1's sub-choice; the more-ceremonious alternative — demanding both clauses on a cache field — is weighed and declined in §3.)
3. **Hashing coherence cannot break (§8.3).** The generated `=` and the generated `GetHashCode` are computed over the *same* reduced member set. If `a = b`, every participating member compares equal; the hash combines exactly those members' hashes in declaration order (§8.3's order-sensitive combiner, unchanged); therefore `a.GetHashCode() = b.GetHashCode()`. The contract holds by construction — the skip removes a member from both sides of the implication at once, and there is no configuration in which a member is hashed but not compared or vice versa.
4. **All members skipped.** A record whose every storage component is equality-skipped has an empty participating set and takes §9.2's existing zero-auto-property semantics verbatim: every pair of values equal, a fixed implementation-defined hash, `ToString` unchanged (it renders the auto-properties, skipped or not). Defined, no diagnostic — §9.2's zero-property record draws none, and an all-skipped record is the same value semantically. (Q3 offers the warning if the owner wants one.)
5. **The recursive admissibility rule is unchanged in shape.** "Another record that is itself auto-equality-bearing" (§9.2) now evaluates the inner record *with its own skips applied* — which is not a new rule but the same reuse discipline: the property is §3.6-derived, computed per type from its declaration, and the outer record consumes the inner's verdict. An inner record rescued by `equality skip` is admissible as a component exactly as any auto-equality-bearing record is.
6. **`with` and construction.** `with` copies storage directly (§9.5) — a skipped member copies with the rest (a copied cache is at worst stale, which is the cache author's existing contract). Constructors are unaffected; a validating constructor remains fully compatible (§9.2).
7. **Choices (Q2).** §35.7 defines choice equality by §9.2's membership list; a `CaseField` marked `equality skip` is excluded from its case's payload comparison and hash, and an inadmissible payload so marked does not force the use-site error. Same rule, same reuse, one sentence in §35.7.

## 3. The serialization/equality asymmetry, argued

A member can now be equality-skipped but serialized, serialization-skipped but equality-bearing, both, or neither. Is that coherent? **Yes, and it is already the established design.** §9.8 states the two packages are "independent packages over the same auto-properties" and that "the `serialized` clause affects only serialization synthesis — it has no effect on auto-equality." The asymmetric configurations are each meaningful:

- *equality-skipped, serialized* — a `LastModified` timestamp: persisted, not identity. Round trip: `Deserialize` reads it normally (still a serialized component); equality ignores it. Coherent.
- *serialization-skipped, equality-bearing* — a derived-but-stored digest recomputable on load: identity, not wire format. §9.8's existing `default(T)`-reconstruction rule applies to it unchanged. Coherent (and possible today).
- *both* — the cache field: neither identity nor wire. The `Interval` case, spelled `private equality skip _cachedLen: int;` — note an explicit field needs no `serialized skip`, because explicit fields were never serialized components; the one clause suffices, and §2's eligibility-gate rule (consequence 2) is exactly what keeps the record serialization-eligible with the skipped explicit field present. (The alternative — demanding both clauses on a cache field — is offered in Q1's sub-choice and declined as ceremony that reads as meaning something it does not.)

The line that stays bright: **a clause never crosses packages' participation.** `equality skip` never hides a member from the wire; `serialized skip` never changes what `=` compares. Only the *eligibility gate* (can synthesis run at all) treats a skipped-by-either member as structurally absent, which is precisely what "skip" claims.

## 4. What the skip is not

- **Not custom equality logic.** Skip only *excludes*. A record that needs projection equality — 0012's `DateTimeOffset` comparing by instant — still writes the five members; that precedent (§25.2's sanctioned overload route) is untouched and remains the escape for logic, with `equality skip` covering the exclusion-only case that the rationale §18 entry recorded.
- **Not a mutability license.** §9.4's advice stands; a skipped *mutable* member is the expected use (a cache mutated behind `readonly` structure), but skipping does not make mutating *participating* members any safer.
- **Not available to classes.** Classes have no auto-equality (§8.3: reference equality); nothing to skip.

## 5. Corpus evidence

Counted at v0.241:

- **`serialized skip`: 31 occurrences** (docs + tests) — the mirrored mechanism is real, shipped, and exercised; its grammar, inert-annotation error, and eligibility interaction are all precedent this proposal copies rather than designs.
- **Records currently paying the five-member road: 0 in the stdlib** (the stdlib's records are clean structural records; the three `operator =` declarations in the stdlib doc are the compiler-intrinsic `IntPtr`/`UIntPtr`/`Ptr<T>` pointer types, §25.2-exempt, not rescues). **~28 `operator =(` sites in tests** — overwhelmingly §25.2/§9.2 rule tests (co-declaration enforcement, overload resolution), not cache-field workarounds. **0 doc examples** show the five-member rescue.
- So the corpus shows **no accumulated workaround debt** — the honest reading is that the all-or-nothing rule *deters* the cache-field shape rather than taxing it visibly (users simplify the record or move the cache out). The proposal's evidence is the recorded deferral (rationale §18 — the need was real enough to record) plus the mechanism's arrival (v0.117), not corpus pressure; stated plainly per the review's evidence discipline.
- **`equality skip` / `EqualityClause`: 0 occurrences** — fresh spelling, no collision; `equality` as an identifier appears nowhere in the corpus as a declared name (contextual-keyword safety check).

## 6. Migration and compatibility (changelog categories)

- **Newly defined:** the `EqualityClause` grammar production and its semantics (§2); the §9.8 eligibility-gate sentence (§2 consequence 2, weighed in §3); the §35.7 extension (Q2).
- **Newly rejected:** `equality skip` on computed properties, methods, statics, enum constants, choice cases (inert-annotation errors, mirroring §9.8's).
- **Nothing flips:** no existing program changes meaning — the clause is opt-in, absent everywhere today. Records currently disqualified stay disqualified until annotated.
- **§3.6 rule 3 note (public contract):** adding or removing `equality skip` on a public record's member is a *breaking change* — it flips comparison results and hash values for clients, and can flip the auto-equality-bearing/equality-bearing derived properties. One sentence in §3.6's rule-3 examples; the artifact interface (§2.8.2) carries the clause exactly as it carries `serialized` clauses (same delta classification path, §2.8.6).

## 7. Alternatives considered

1. **Status quo** (the five-member road, deferral stands). Declined: the deferral's recorded cost model is outdated — §9.8 shipped the mechanism, and the remaining cost of the fix is one grammar production plus reuse of an existing computation. The all-or-nothing rule's *virtue* (never silent) is fully preserved: an unannotated cache field still errors at use sites.
2. **A type-level exclusion list** (`equality(Lo, Hi)` header clause). Declined by §9.8's own recorded reasoning against all-or-nothing customization lists: "adding a component never silently drops it" holds only for per-member total clauses; a list must be kept synchronized (rationale §5.19's failure mode).
3. **Auto-exclude private fields** (equality over public storage only). Silent, visibility-keyed semantics — a `private` toggle silently changing `=` is exactly the implicit behavior the §9.2 error exists to prevent. Declined.
4. **`equality ignore` / `transient` / other spellings.** Declined for vocabulary economy: the sibling clause spells exclusion `skip`; one word for one concept across both packages (`serialized skip` / `equality skip`) is the point of mirroring.
5. **Full custom-equality clauses** (`equality by(...)` projections). Out of scope — that is 0012's five-member territory; this proposal deliberately takes the exclusion-only slice the rationale §18 entry describes ("exclude one field (cached hash, debug timestamp)").

## 8. Implementation sketch

- **parser** (`src/parser`): the `EqualityClause` production at the four attachment sites; `equality` contextual-keyword handling copied from `serialized`'s.
- **sema** (`src/sema/` — the §9.2 membership computation feeding auto-equality synthesis, the §9.8 eligibility check, and the §3.6 derived-property evaluation): thread the per-package skip sets through the existing membership walk; the inert-annotation errors at the rejected positions.
- **codegen**: the synthesized `=`/`Equals`/`GetHashCode` bodies iterate the participating set — already parameterized by the member list; no new lowering.
- **tests** (spec-cited per `tests/README.md`): the `Interval` rescue (declares fine, `=`/hash over `Lo`/`Hi` only — a behavior test asserting equal-with-different-cache and hash agreement); the disqualifier-override non-rescue; all-skipped semantics; the §9.8 asymmetric configurations round-tripping (equality-skipped-but-serialized and the reverse); the `with`-copies-skipped-member trace; Q2's choice payload case; the inert-annotation rejections; a §3.6 derived-property flip test (hashable constraint satisfied after rescue — `Set<Interval>` instantiates).
- **Size:** small — the mechanism is a set-difference on an existing computation plus grammar plumbing.

## 9. Questions for the owner

- **Q1** — adopt `equality skip` as specified (§1–§3)? Recommended. Sub-choice within it: the eligibility gate treats a member skipped by *either* package as structurally absent for both packages' eligibility (recommended, §2 consequence 2), or demand both clauses on a cache field for a serializable record (more explicit, more ceremony)?
- **Q2** — extend to choice payload fields (`CaseField`) in the same revision (recommended — §35.7 already defers to §9.2's list, so *not* extending creates a fresh asymmetry), or records-only v1?
- **Q3** — all-members-skipped: defined with no diagnostic per §9.2's zero-property precedent (recommended), or a warning ("every component skipped — every value compares equal")?
- **Q4** — confirm that `equality skip` does **not** remove the member from the generated `ToString` (recommended — one clause, one effect; renderings are not contracts), or bundle ToString exclusion?

## 10. Interactions

- **Proposal 0086 item 4 (#3052, `hashable`/equality-bearing vocabulary):** this proposal changes when the §3.6 property *holds*; item 4 changes what it is *called*. Textually adjacent (§3.6's table row); land in either order, cross-cite at graduation.
- **Proposal 0078 (#3044):** none (enums have no member equality).
- **§2.8.6 delta classification:** the clause's presence in the artifact interface follows `serialized`'s existing carriage; no new category.
- **Rationale §18:** the "Asymmetric record auto-equality customization" entry gains its shipped-pointer annotation at graduation (same-revision annotation rule, per the v0.186 lesson recorded in that list).
