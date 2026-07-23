# Proposal 0066 — Method-local `record` and `enum` declarations (`LocalTypeDecl`)

| | |
|---|---|
| **Status** | draft — RFC for owner review (unconfirmed, owner-gated), 2026-07-24. The *direction* is already accepted: the delegated, critic-gated ruling recorded on [#1405](https://github.com/s8lap/bezel-lang/issues/1405) (2026-07-24, against spec v0.220; post-critique revision) accepted method-local `record` and `enum` declarations with four scope rulings fixed. This document writes that acceptance up in full, per the ruling's own "proposal needed" verdict; the settled forks are not reopened (§3). |
| **Tracking issue** | [#1405](https://github.com/s8lap/bezel-lang/issues/1405) — the feature [DECLINED.md](DECLINED.md) **D-0002** names as the accepted remedy for anonymous types (owner-confirmed 2026-07-03) |
| **Target** | Spec Appendix **A.6** (`Statement` gains `LocalTypeDecl` — delta in §8), a new [§21](../Bezel_Language_Spec.md#21-statements) subsection (*Local type declarations*: scope, restrictions, semantics cross-references), one cross-reference sentence in [§11.6](../Bezel_Language_Spec.md#sec-nested-types)'s vicinity at the editor's discretion, Appendix C.1 (one type-system-register bullet); rationale (the D-0002 remedy record graduates from DECLINED.md prose into a rationale pointer). **No stdlib change.** §9 (records) and §10 (enums) are *referenced*, not amended — the local forms are ordinary nominal types (§2.4). |
| **Written against** | spec v0.220 |
| **Origin** | Owner conversation on data projections (2026-07-03) → #1405; D-0002 (anonymous types declined on three structural grounds, method-local records named the remedy); delegated ruling 2026-07-24 (full record on the issue thread, revision log included). |
| **Depends on** | Nothing. Backlog-sequenced: implementation is queued **after** the R0 bug-drain and the R1 stdlib waves; acceptance schedules nothing (§7). |

*Reference convention: unprefixed §-references are language-spec references.*

---

## 0. Summary

Allow `record` and `enum` declarations as **statements**, scoped to the enclosing body like a local variable. The local type is an ordinary nominal record/enum — §9.2 auto-equality, §9.5 `with`, §9.8 serialization eligibility, §10's whole enum surface — with exactly one relaxation: *where the declaration may appear*. Four scope rulings are fixed: **(a)** strictly body-scoped visibility, no signature positions, escape-via-boxing admitted only as far as §8.6 already admits it; **(b)** references to enclosing generic parameters rejected in v1 (liftable); **(c)** one grammar anchor — `LocalTypeDecl ::= RecordDecl | EnumDecl` as a `Statement` alternative with an empty visibility slot and no type shadowing; **(d)** `enum` included, `choice` excluded (liftable). No new kernel rule (§4).

## 1. Motivation

**D-0002's recorded remedy.** Anonymous types were declined on three structural grounds — structural typing inside a nominal system, shapes that cannot cross method boundaries, and a killer habitat (reflection) Bezel deliberately lacks — and the decline names its remedy: "method-local `record` declarations (tracked separately): the shape lives next to its single use, keeping the recorded records-over-tuples stance ('a name and a documented shape is a feature') while removing the distance cost." This proposal is that tracked remedy. Declining it would strip D-0002 of its remedy and reopen a settled decline without new evidence — the move DECLINED.md's preamble forbids.

**The distance cost is real and purely lexical.** A LINQ projection needs a named record; today that name must live at module or class level, arbitrarily far from its single use:

```bezel
// current syntax (v0.220) — excerpt of a probe-validated program (User: a
// class with Name/Age properties; import System.Collections.Linq)
record NameAge(Name: string, Age: int);      // module level: one line, wrong place

public static class Report
  public static method Render(users: Array<User>): string
    var rows := users.Select(lambda(u) => new NameAge(u.Name, u.Age)).ToArray();
    var sb := new StringBuilder();
    for var row in rows do
      sb.AppendLine($"{row.Name}: {row.Age}");
    end;
    return sb.ToString();
  end Render;
end Report;
```

Proposed — the shape moves next to its single use; nothing else changes:

```bezel
// proposed
public static method Render(users: Array<User>): string
  record NameAge(Name: string, Age: int);    // local: visible to end Render only
  var rows := users.Select(lambda(u) => new NameAge(u.Name, u.Age)).ToArray();
  …
end Render;
```

The same courtesy for small closed constant sets scoped to one algorithm:

```bezel
// proposed
public static method Advance(grid: Grid)
  enum Phase
    Scan, Mark, Sweep;
  end Phase;
  var phase := Phase.Scan;
  …
end Advance;
```

The feature adds **no typing discipline**: the local record keeps everything records provide — auto-equality (§9.2), `with` (§9.5), serialization eligibility by field types (§9.8), nominal identity, a name a reader can search for. Only the declaration's admissible position is relaxed.

## 2. Design

### 2.1 Grammar — one anchor, reused productions

Appendix A.6 `Statement` gains one alternative:

```
LocalTypeDecl   ::= RecordDecl | EnumDecl
```

reusing A.3's productions unchanged, with two stated restrictions checked semantically (the shared-production over-admit policy A.4 already uses):

- **The `Visibility?` slot must be empty.** A local type has no access surface — there is no "outside" that could hold a reference — so a visibility keyword is a compile error naming the rule. This deliberately *contrasts* with §11.6 nested types, where v0.181 makes the keyword **required**: a member has an access surface to declare; a local does not. Both rules are locally motivated; the new subsection states the contrast.
- **The `GenericParams?` slot must be empty (v1).** A local type declares no type parameters of its own; see §2.3 and the flag in §6 (this restriction is a drafting-level addition beyond the ruling's four lettered rulings, recorded liftable).

Parse commitment is token-one: `record` and `enum` are unconditionally reserved (§1.4, B.1) and begin no statement today, so the statement reading is unambiguous and no existing program changes meaning.

`LocalTypeDecl` is admitted wherever a `Statement` is — method, constructor, destructor, and accessor bodies, lambda `do`-bodies, `do … end` blocks, loop bodies, statement-position arm bodies. "Method-local" throughout this proposal reads as *body-local*.

### 2.2 Scoping and name discipline

- **Scope**: from the declaration statement to the end of the **innermost enclosing statement block** (the member body, or the `do … end`/loop/arm block containing it) — exactly a local variable's scope (§21, §21.6). Declaration must precede first use; local type declarations are therefore order-significant, and a local type's field types may name only types already in scope (module-visible types, the enclosing class's nested types, and *earlier* local types — no forward references, hence no mutually recursive local types).
- **Within its scope the name is an ordinary type reference** in *body* positions: local-variable ascriptions, `new`, generic arguments of calls and local declarations (`Array<NameAge>`), `typeof`/`nameof`/`default`, casts and type patterns, lambda parameter ascriptions, and nested lambdas (lexically inside the body).
- **No signature positions.** The name may not appear in any *member signature*: parameter types, return types, field/property/indexer types, generic constraints, or base/interface lists of non-local types. This is lexically near-automatic — the name is simply not in scope at any member signature — but the rule is stated so the diagnostic is the named one: *"a method-local type may not appear in a member signature."*
- **No type shadowing.** The declared name may not collide with any *type* name visible at the declaration point. The names-are-a-feature stance cuts against shadowing here: a local `NameAge` silently shadowing a module-level `NameAge` two scopes up is a trap, not a convenience; the collision is an error. It also may not collide with another declaration in the same body's declaration space — another local type or a local variable — the §11.3-style clash discipline applied to the body. Two *different* methods may each declare `record Row(…)`: the names never meet, and the types are distinct (§2.4).

### 2.3 Restrictions (v1)

1. **No references to enclosing generic parameters** — ruling (b), a compile error naming the rule. A local record mentioning an enclosing method's or class's `T` would be implicitly generic: its identity, §9.2 equality synthesis, §9.8 serialization synthesis, and boxed conformances all become per-instantiation, needing a story in the artifact interface's per-instantiation machinery (§2.8.2) and the cross-module generics posture (the #428 family's recorded decline-with-subset). Genuine design space, disproportionate to the motivating case (projections with concrete field types). Forbid→allow is a compatible future widening; the reverse is a breaking retraction — smallest sound ruling wins. **Liftable**, with the lifting cost named above. A welcome consequence of the restriction: a local type's identity is **instantiation-independent** — one nominal type per declaration even inside a generic body, so a public generic's carried body (§2.8.2 definition-site clause) carries it as ordinary body content with no per-instantiation identity questions.
2. **No own generic parameters (v1)** — the `GenericParams?` slot is empty (§2.1). Same synthesis-per-instantiation surface as (1) minus the artifact seam; the motivating case needs none of it. Liftable; flagged in §6 as a proposal-level addition.
3. **No local `choice`** — ruling (d). Choices carry payload-derived three-way classification (§35.4 value-like / borrow-bearing / **owning** via §15.3 move-only), announced-owning match obligations, and coverage machinery — a much heavier admission whose local-declaration interactions (an owning local choice's move-only value category inside one body, for instance) deserve their own study, and no projection-shaped motivating case exists. Recorded **liftable with evidence**.
4. **No local `class` or `interface`** — not asked and not wanted: classes carry ownership, destructors, and the §28 disposal machinery — a body-scoped class is real kernel surface for no recorded need. **Out of scope**; a future admission would need its own proposal and K-derivation. Interfaces are contracts for an outside that a local has none of.
5. **No visibility keyword** — §2.1.

### 2.4 Semantics — an ordinary nominal type, stated

The local type is an **ordinary nominal record or enum**; every §9/§10 rule applies unchanged:

- **Identity is per-declaration.** Two same-shaped local records in different methods are distinct types — nominal, deliberately (no structural unification; that is the anonymous-types discipline D-0002 declined).
- **Auto-equality** (§9.2), **`with`** (§9.5), **auto-`ToString`** (§8.5), **serialization synthesis** (§9.8, eligibility by field types, conformance declared as usual), interface implementation (§9.6), and the derived type properties (§3.6 — auto-equality-bearing, borrow-bearing per §9.7, sendable per §30.14, serialization-eligible) are all computed by their existing rules from the declaration. Static members are admitted as on any record/enum (§9.2's static-member exemption; a local type's `static readonly` state is program-lived under §22.13, like any other static).
- **Local enums** are §10 enums wholesale: constants, explicit values, underlying types, methods, interface conformances, `case` matching with prefix omission (§10.6) and enum-scrutinee exhaustiveness (§19.6) inside the scope.
- **Boxing and escape — the one subtle rule, stated exactly as far as §8.6 already goes.** A **non-borrow-bearing** local record (§9.7 — no borrow-carrying fields; `string`/runtime-managed fields don't count) implementing an interface may be **returned boxed through that interface**: the type identity escapes *unnameably*, precedent being a `private` nested type (§11.6) escaping through a public interface today; such a box is genuinely self-contained — records carry no disposal (§9.7: records cannot implement `IDisposable`) and a non-borrow-bearing record holds no references whose sources it could outlive. A **borrow-bearing** local record (§9.7: "Reference-typed fields in records are always borrows") boxes under §8.6 *Boxing of borrow-bearing records* **unchanged**: a short-lived, call-bounded box is admitted, and "storing a boxed borrow-bearing record in a field, returning it from a method, or capturing it in a closure that escapes the source scope" stays rejected by the same rules as the unboxed form (§9.7, §28.3). Locality of the declaration adds **no new admission**. The counterexample an unqualified admission would wrongly bless — recorded from the ruling's critique cycle as the case the rejection prevents: `record View(Cell: Widget)` declared over a `using var` widget and returned boxed through an interface — the widget is disposed at the method's end and the box dangles; §8.6's existing rejection is exactly what stops it, fully engaged for local records.

### 2.5 Diagnostics register

The named errors, one per rule (wording sketches; editor latitude at landing):

```
error: a method-local type may not appear in a member signature
  `NameAge` is declared inside `Render` and is visible only in its body
  (§21.x). Move the declaration to module or class level to use it in a
  signature.

error: reference to enclosing type parameter `T` in a method-local type
  a method-local record/enum may not mention the enclosing method's or
  class's type parameters (v1, §21.x). Use concrete field types, or move
  the declaration to module level as a generic type.

error: local type `NameAge` collides with a visible type name
  method-local types do not shadow: `NameAge` is already visible here
  (declared at Reports.NameAge). Rename the local type. (§21.x)

error: a method-local type takes no visibility keyword
  `public record NameAge(…)` — a local type has no access surface;
  remove `public`. (§21.x; contrast §11.6, where nested types require one)
```

## 3. Alternatives considered and declined

1. **Decline the feature.** Would reopen D-0002 by removing its recorded remedy; the projection-distance cost is real and this remedy is the nominal-typing-preserving one. Declined.
2. **Anonymous types instead** — already declined as D-0002 (structural typing in a nominal system; can't cross method boundaries; reflection habitat). Not reopened here; this proposal is its remedy, and the D-0002 reopening bar (tuples first, with evidence) is untouched.
3. **Defer with no ruling on the scope questions.** The four questions (visibility/escape, generic capture, grammar anchor, enum/choice inclusion) were answerable with small conservative rulings; leaving them open invites the silent-interpretation failure mode CLAUDE.md rule 4 forbids. Declined — they are fixed (§2).
4. **Allow enclosing generic-parameter capture in v1.** Per-instantiation identity, equality/serialization synthesis, boxed conformances, §2.8.2 artifact-interface machinery, #428 posture — disproportionate to concrete-field projections. Declined, **liftable** with the cost named (§2.3.1).
5. **Admit `choice` locally in v1.** Heavier classification/coverage/owning-match surface, no motivating case. Declined, **liftable with evidence** (§2.3.3).
6. **Type shadowing of outer type names.** A local type silently shadowing a module/class type is a readability trap; collision is an error instead (§2.2). Declined.
7. **Local `class` declarations.** Ownership, destructors, disposal machinery (§28) — genuinely new kernel-engaging surface for no recorded need. Out of scope; would need its own proposal and K-derivation. Declined.
8. **Unqualified escape-via-boxing** ("an escaped boxed local record is always self-contained"). Refuted in the ruling's critique cycle: it would admit the §8.6-rejected dangling box (`record View(Cell: Widget)` over a `using var`, returned boxed — a use-after-free). The admitted form is qualified to non-borrow-bearing records only; borrow-bearing local records keep §8.6's call-bounded-box-only rules (§2.4). Declined-as-amended.

## 4. Interactions

- **K-derivation: no new kernel rule** (deliberately *not* "no kernel contact"). Local records/enums are value types — records cannot own heap resources or implement `IDisposable` (§9.7), enums are constants — so no ownership, disposal, or borrow rule *changes* and the K1–K9 obligations are untouched. But a local record with class-typed fields is **borrow-bearing** (§9.7) and engages the existing §9.7/§8.6/§28.3 borrow machinery (K2 bounding) exactly as a module-level record does: the feature adds no rule to that machinery and lifts none of it. The v1 cut excludes `choice` and `class` precisely because those are the kinds that would add genuinely new kernel-engaging surface.
- **§8.6 / §9.7 / §28.3** — the boxing/escape rule of §2.4; nothing amended, one sentence of application stated in the new subsection.
- **§9.8 serialization** — eligibility by field types, unchanged; the synthesized wire shape is field-name-keyed, so same-named local records in different methods interoperate (or don't) exactly as any two same-shaped records do. A local type cannot appear in a `polymorphic` registry (the registry lives in a class body, where the name is not in scope) — automatic, no rule needed.
- **§30.14 sendability** — derived from field types as usual; a sendable local record crossing a domain boundary *by value* is fine (its identity travels with the value; no naming is needed).
- **§19 matching** — a local record destructures, a local enum matches with §19.6 enum exhaustiveness, inside the scope; nothing special.
- **§2.8.2 artifact interface** — a local type appears on no public surface (it can't: no signature positions), so it enters the interface only as carried *body content* of an exported generic (definition-site clause), where restriction (1) keeps its identity instantiation-independent (§2.3.1). No interface format change.
- **§21 / §21.6** — scope and block rules reused verbatim; `LocalTypeDecl` is one more statement kind with no control-flow behavior.
- **§11.6 nested types** — unchanged; the visibility-keyword contrast is stated in the new subsection (§2.1). A local record's *body* may itself contain §11.6 nested member types, which follow their own rules.
- **Compiler (impl note, not spec)** — symbol collection currently has no body-scoped type space; cost recorded as compiler-medium on the issue. The impl tracker is filed at graduation (§7).
- **Conformance** — Core (§21 statements); no Appendix D level change.

## 5. Non-goals

- **Anonymous types stay declined** (D-0002; reopening bar unchanged).
- **Tuples stay deferred** per the recorded rationale stance (records-over-tuples).
- **No inference of local shapes** — the declaration is explicit; nothing is synthesized from a projection expression.
- **No local `class`/`interface`/`choice`** (§2.3; the first two out of scope, the third liftable).

## 6. Open questions / liftables

- **L1 — enclosing generic-parameter capture** (ruling (b)): liftable; the lift must answer per-instantiation identity, §9.2/§9.8 synthesis, boxed conformances, and §2.8.2 carriage. Evidence bar: generic-context projections that cannot use concrete field types.
- **L2 — local `choice`** (ruling (d)): liftable with evidence; the lift must study an owning local choice's move-only value category and match obligations inside one body.
- **L3 — own generic parameters on the local type** (drafting-level addition, §2.1/§2.3.2): the ruling fixed (b) for *enclosing* parameters and was silent on the local type's own; this draft forbids both in v1 on the same smallest-sound-ruling grounds. **Flagged for owner confirmation** — admitting `record Pair<T>(…)` locally is coherent (module-level generic records exist) but drags in the same per-instantiation synthesis questions for zero motivating need.
- **Q1 — display of local type names** in diagnostics, `ToString`'s qualified-name forms (§8.5), and `Type` values observed through a boxed escape (§8.2): recommend an implementation-defined qualified rendering that names the enclosing member (e.g. `Reports.Render.NameAge`), normative only in that distinct declarations render distinctly. Owner may prefer to pin the format.
- **Q2 — statement-position reach.** This draft admits `LocalTypeDecl` in every statement context (§2.1, including single-statement arm bodies), which is the zero-carve grammar reading; the owner may prefer restricting to *block* contexts (member bodies and `do … end`) if arm-position declarations read as noise. Recommend: as drafted (no carve).

## 7. Staging and migration

1. **This proposal** → owner review → `accepted`.
2. **Graduation** is a ruling-class landing (spec-change-process §2/§6): A.6 delta, the new §21 subsection, Appendix C bullet, rationale pointer, changelog entry with Migration note, `check_docs.sh`, adversarial critique. Examples land in the v0.207+ syntax surface directly.
3. **Implementation tracker** filed at graduation, per the accepted-sequences-not-starts posture: *"impl: method-local record/enum declarations (`LocalTypeDecl`) — body-scoped type space in symbol collection, per proposal 0066."* Queued after the R0 drain and the R1 stdlib waves; #1405 remains the design tracker until then.
4. **Migration: none.** Purely additive — `record`/`enum` at statement position is a parse error today, so no existing program changes meaning. Test pins flipped: none (the ruling's grep of `tests/` and the conformance corpus found no pin asserting the rejection as a named rule). Corpus impact: none until landing. Cross-references: the other rfc-b items (#1402/#1445/#1449) are stdlib surfaces and do not interact.

## 8. Grammar delta (Appendix A.6)

```
/* current (v0.220) */
Statement       ::= LocalDecl | LocalConstDecl | AssignmentStmt | ExpressionStmt
                  | DiscardStmt
                  | IfStmt | WhileStmt | ForStmt | DoEndBlock
                  | CaseStmt | WhenStmt
                  | ReturnStmt | ThrowStmt | BreakStmt | ContinueStmt
                  | TryStmt | YieldStmt
                  | IncrementStmt

/* proposed: one added alternative … */
Statement       ::= LocalDecl | LocalConstDecl | AssignmentStmt | ExpressionStmt
                  | DiscardStmt
                  | IfStmt | WhileStmt | ForStmt | DoEndBlock
                  | CaseStmt | WhenStmt
                  | ReturnStmt | ThrowStmt | BreakStmt | ContinueStmt
                  | TryStmt | YieldStmt
                  | IncrementStmt
                  | LocalTypeDecl

/* … and one new production, reusing A.3 */
LocalTypeDecl   ::= RecordDecl | EnumDecl
                              /* Method-local type declaration (proposal 0066):
                                 `record`/`enum` are reserved and begin no other
                                 statement — token-one commit. The reused A.3
                                 productions are restricted semantically: the
                                 Visibility? slot must be empty (a local type
                                 has no access surface — contrast §11.6, where
                                 nested types REQUIRE the keyword) and the
                                 GenericParams? slot must be empty (v1). Scope:
                                 declaration to the innermost enclosing block's
                                 end, like a local variable; no signature
                                 positions; no shadowing of visible type names
                                 (§21.x). References to enclosing generic
                                 parameters are rejected (v1, §21.x). */
```

A.3 (`RecordDecl`, `EnumDecl`) is unchanged.
