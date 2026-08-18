# Proposal 0080 — Require `Exception`-derived throwables

| | |
|---|---|
| **Status** | graduated-v0.243 — superseded — graduated v0.243 (owner-ruled 2026-08-11, all Qs as recommended) |
| **Tracking issue** | [#3046](https://github.com/s8lap/bezel-lang/issues/3046) |
| **Target module(s)** | **spec** — §31.1 (the throw operand; the flagged ruling resolved), §31.2.1 (multi-type binding rule simplified; generic arm bound tightened), §31.2.3 (catch-all restored), §31.2.5 (the asterisk deleted), §17.12/§31.5 (wording), §22.15 (pure-throw carve-out's operand sentence), Appendix C.5 (invariant wording). **stdlib** — none (§2.17's hierarchy already complies). **rationale** — §11.3 area gains the decision record (or a new §11 subsection). **guides** — Guide 02 wording only (no non-`Exception` teaching exists to delete). |
| **Written against** | spec/stdlib v0.241 |
| **Origin** | Elegance review 2026-08-11, finding 3 (§18–§35 report): "a spec-flagged wart that should be resolved in favor of deletion." The spec has carried its own flag since v0.182: *"whether the language should eventually **require** it is flagged for owner review, and until re-ruled the current ruling stands"* (§31.1). This proposal is the re-ruling, in favor of requiring. |

*Reference convention: unprefixed §-references are language-spec references.*

---

## 0. Thesis

`throw` today admits **any owned class instance** (§31.1, the #771 ruling codified at v0.182). The capability has no recorded motivating use, no reporting surface (`Message`/`StackTrace`/`CauseChain` are `Exception`-only, §31.4), and its entire cost is *rules*: a no-universal-catch-all caveat (§31.2.3), an `Object`-base carve in the multi-type binding rule (§31.2.1), an asterisk on the deliberate-suppression idiom (§31.2.5), and a hedged parenthetical in §31.1 itself. The v0.182 changelog already recorded the reason the ruling was suspect — *"#771 predates the v0.162 Exception-model completion, whose reporting surface … is written in `Exception` vocabulary."*

**Proposed rule:** the `throw` operand's static type must be a class type that is `Exception` or derives from it. Everything the permissive ruling forced the spec to say gets deleted; the property every C#/Java reader assumes — a final `Exception` arm catches everything — becomes true.

## 1. The rule

Replacement for §31.1's operand-type sentence (drop-in sketch; the ownership half of the paragraph is untouched — see §3):

> `throw expr` requires `expr` to be an **owned instance of an `Exception`-derived class type**: the operand's static type, nullability not admitted, must be `Exception` or a class deriving from it. Values of primitive, `string`, record, enum, inline-array, unit, or function type cannot be thrown — a compile error at the throw site. An `Object`- or interface-typed operand is likewise rejected (no interface derives from `Exception`, and an `Object` referent may be a boxed value); narrow to the exception class first. The operand is consumed as an owned value under the ownership rules below, so a *borrowed* class reference is not a valid operand: to re-raise the exception being handled, use the bare re-raise (§31.2.4); to propagate a caught exception's information, chain a fresh instance through the snapshot constructor (§31.4).

The flagged parenthetical after the catch-dispatch sentence (*"…a non-`Exception` throwable participates in `try`/`catch` … but offers none of that reporting surface. Deriving from `Exception` is therefore the strongly recommended default; whether the language should eventually require it is flagged for owner review…"*) is **deleted** — the requirement replaces the recommendation, and the flag is discharged by this ruling.

## 2. What this deletes, and what it re-states

Read against the v0.241 text in full, §31 carries exactly these permissive-ruling artifacts:

1. **§31.1, the catch-dispatch paragraph.** *"a thrown class that does not derive from `Exception` is caught by an arm naming its type or a base of its type, and propagates past a bare `Exception` arm"* — deleted with its whole parenthetical (the reporting-surface hedge and the owner-review flag). Replacement: one sentence stating that catch dispatch is purely by type (§31.2.1) and that every throwable carries the `Exception` surface of §31.4.

2. **§31.2.1, the multi-type binding carve.** The current rule needs a special case: a mixed list (`Dog | IOException`) has a nearest common base of `Object`, and the rule rejects the *binding* form there with a three-line justification. With every throwable `Exception`-derived and Bezel's single inheritance, **the nearest common base of any two throwable classes is itself `Exception`-derived or `Exception`** — the `Object` case is unreachable. Simplified drop-in text for the binding bullet's tail:

   > When binding, `name` has the static type of the **nearest common base** of the listed types — the most specific class that is a base of every listed type. Every listed type is `Exception`-derived (the arm-type rule below), so the nearest common base is always `Exception` or a class deriving from it; for alternatives sharing nothing more specific, it is `Exception`.

   The parenthetical justifying the `Object` rejection (four lines of v0.241 text) is deleted whole. The non-binding form needs no change (it was unrestricted; it stays unrestricted, now over `Exception`-derived types).

3. **§31.2.3's no-universal-catch-all caveat.** *"A thrown class that does not derive from `Exception` — which §31.1 admits — propagates past an `Exception` arm: there is no universal catch-all pattern; to catch a non-`Exception` class, name its type…"* — deleted. The preceding sentence loses its qualifier: a bare `Exception` arm at the end **is the catch-all**, full stop; the "for programs that throw only `Exception`-derived classes (the standard library and every built-in producer do)" hedge deletes with it.

4. **§31.2.5's suppression asterisk.** *"(a thrown non-`Exception` class propagates past it, §31.2.3 — there is no universal catch-all)"* — deleted; "to deliberately suppress all `Exception`-derived exceptions" simplifies to "to deliberately suppress all exceptions."

5. **§22.15's pure-throw carve-out sentence.** *"The carve-out follows §31.1's operand rule otherwise (any class, not only `Exception`-derived)."* — the parenthetical updates to the new operand rule. Nothing else in the carve-out changes: its two conditions (pure constructor; trivial-dispose operand class) are orthogonal to the base-class requirement, and the built-in exception constructors that enable the validation idiom are already `pure` (§31.4). `Exception` itself is trivial-dispose (its state is `string`s and a trivial-dispose `Array<ExceptionInfo>`; §31.4's note "all built-ins are trivial-dispose"), so an `Exception`-derived user class is trivial-dispose exactly when its own additions are — the requirement subtracts nothing from the carve-out's reachable set.

6. **A consequential tightening this proposal must state (not silently imply): catch-arm types.** Under the new rule, an arm pattern naming a class that is neither `Exception` nor derived from it is **provably dead**: by single inheritance, every base of an `Exception`-derived class is on its chain — `Object`, `Exception`, or an intermediate `Exception`-derived class — so no throwable instance can ever match a non-`Exception`-family type. Proposed: such an arm is a **compile error** naming this rule (the same dead-code posture as §31.2.1's existing rejection of non-class generic bounds). The generic-parameter arm rule tightens in step: the parameter must carry an **`Exception`-derived class bound** (`where TEx: Exception` — today the *idiomatic* form becomes the required one); a bound naming a non-`Exception` class is rejected at the generic declaration, exactly where the current class-bound check already sits. Q1 asks whether the dead-arm case should be a warning instead; the recommendation is error (see §7).

Everything else in §31 — arm ordering, guards, supersession, bare re-raise, exception ownership, dispose-after-arm and its two exemptions, the snapshot chaining constructor, `finally` semantics — is type-agnostic over throwables and survives verbatim.

## 3. The ownership seam, shown unchanged

`throw x` is K3.8 in the owned-consuming enumeration and a K4.2 **announced kill**: the keyword is the announcement, the dispose-after-arm sets the disposed flag, and the consume therefore joins no consume-site freeze list (§31.1, §28.3). Every clause of that machinery is keyed on the operand being an **owned class instance** — its K1 classification — and none is keyed on the operand's base class:

- K3.8/K4.2's announced-kill status depends on keyword-visibility plus the flag backstop; both are properties of the throw form and the runtime's dispose-after-arm, not of the type thrown.
- The post-throw **moved-from** state of a named operand binding (§28.2/K4.2) is the binding-state machinery, base-class-blind.
- The exception-ownership rules (§31.1 *Exception ownership*): transfer to the runtime, disposal after the matching arm, the two owner-owned exemptions, the borrowed-field residual inside thrown instances — all quantify over "the thrown instance," never over its base.
- The pure-throw carve-out's disposal-inertness argument (§22.15) rests on trivial-dispose, not on `Exception`-derivation (§2 item 5 above).

Requiring `Exception`-derivation is therefore a pure *typing* precondition at the throw site — one subtype check — with **zero contact with §28**. No kernel rule, axiom, or freeze-list entry changes; Appendix C.5's throw invariant rewords its type condition and nothing else.

## 4. Corpus evidence

Method: `grep -rhoE 'throw new [A-Za-z_][A-Za-z0-9_]*'` over `docs/` and `tests/`, plus a per-file script resolving each thrown class's in-file declaration against the `Exception` family (declared base named, transitively toward built-ins; hand-checked samples). Run 2026-08-11 at v0.241 HEAD.

**Docs: clean.** All **49** `throw new` sites across the ten normative documents (the spec, stdlib doc, rationale, changelog, and six guides — `docs/*.md` non-recursively; a recursive `docs/` sweep counts 70 by also picking up `docs/proposals/`, which are records, not normative examples) throw built-in exception types or doc-declared classes with explicit `Exception`-family bases (`AssertionException: Exception`, `ProcessException: IOException`, `JsonTypeException: JsonException`, `VerificationException: Exception`; the handful of undeclared illustrative names — `SomeException`, `UpstreamException`, `DataAccessException` — appear only in prose-adjacent snippets and read as `Exception`-derived by name and use). **No normative example throws a non-`Exception` class.**

**Tests: NOT clean — and the honest count matters.** The task's prior expectation ("expect zero") is **wrong for the test corpus**, and this proposal reports it rather than papering over it:

- 750 `throw new` occurrences total in `tests/`.
- **228 occurrences across 41 files** throw a class declared in-file with **no base at all** (`class E end E;`, `class Boom …`, `class AppError …` — full file list attached to [#3046](https://github.com/s8lap/bezel-lang/issues/3046)).
- Of these, exactly **two files are deliberate** exercises of the permissive rule: `tests/sema/catch_multitype_no_common_base_v182_test.cpp` (the `Dog | IOException` `Object`-base binding carve — the very rule §2 item 2 deletes) and `tests/lit/sema/sema_throw_operand_type__non_exception_class_ok_31_1.bzl` (the §31.1 any-class-ok pin). Both flip polarity under this proposal and become regression tests *for* the new rule.
- The remaining ~39 files are **incidental**: the tests are about disposal ordering, `finally` semantics, codegen, and ownership — the authors wrote `class E end E;` as the shortest throwable, because the permissive rule let them. The migration is the one-token `class E: Exception end E;` per declaration, mechanical and semantics-preserving for every one of these tests (their assertions are about cleanup, not about the exception's type identity).

The corpus finding **supports** the proposal's central claim in its strongest form: in ~750 throw sites, not one uses non-`Exception` throwability *for anything* — the only deliberate uses are the tests that pin the rule itself.

## 5. Alternatives considered

1. **Status quo.** The flag stays open, the four rule-artifacts stay in the normative text, and every reader must learn that `catch var ex: Exception` is not a catch-all — a trap with no compensating capability. Declined: the spec itself has been carrying the request for this re-ruling since v0.182.
2. **Require `Exception`-derivation as a warning, not an error.** Preserves the two deliberate tests unmodified and defers breakage. Declined: a warned-but-admitted non-`Exception` throwable keeps every §2 artifact alive (the catch-all caveat, the `Object` carve, the suppression asterisk must all stay, since the shape remains expressible) — the proposal's entire payoff is the deletions, which only a hard rule buys.
3. **Structural rather than nominal admission** (any class exposing `Message`/`StackTrace`). Declined: Bezel has no structural typing anywhere (§3), the reporting surface would still need the runtime's cooperation (trace capture at throw is `Exception` machinery, §31.4), and the nominal rule is one subtype check.
4. **Also retire the arm-side flexibility (catch arms restricted to `Exception`-derived) but keep the throw side permissive.** Incoherent: the throw side is the source of the artifacts; the arm side merely reflects it.

## 6. Migration & compatibility (changelog house categories)

- **Newly rejected:** `throw` of a class not deriving from `Exception` (remedy, named in the diagnostic: derive the class from `Exception` — one token at the declaration — or throw an `Exception`-derived wrapper); a catch arm naming a non-`Exception`-family class type (remedy: delete the dead arm or fix the type); a generic catch-arm parameter bounded by a non-`Exception` class (remedy: `where TEx: Exception` or a derived bound).
- **Newly defined:** the simplified §31.2.1 binding rule (§2 item 2) — for all currently-*accepted* programs it types identically (all-`Exception`-derived lists already resolved to their nearest common base); the `Object`-fallback rejection becomes unreachable rather than repealed-with-changed-behavior.
- **Doc-only:** §31.2.3/§31.2.5 caveat deletions; §22.15's parenthetical; Guide 02 needs no teaching change (it never taught the non-`Exception` case — verified, zero mentions).
- **Not migration, but stated:** no stdlib signature or type changes; every built-in producer already throws `Exception`-derived types (§31.4's own principle).

## 7. Implementation sketch

- **Compiler areas:** the throw-site type check lives beside the existing operand-type rejections (primitives/records/`Object`/interface — `src/sema`, wherever `sema_throw_operand_type__*` is enforced today); the arm-type and generic-bound checks beside §31.2.1's existing class-bound check. Size: **S** — each is one subtype test against a known base plus diagnostics.
- **Tests shipped with the flip** (each spec-cited, failing without the fix): rejection of a no-base-class throw (§31.1, the flipped `sema_throw_operand_type` lit test); rejection of a dead non-`Exception` arm and of a non-`Exception` generic bound (§31.2.1); acceptance + typing of an all-`Exception` multi-type binding (§31.2.1 simplified rule); a behavior test that a final `Exception` arm catches a user-defined derived throwable (§31.2.3 catch-all restored); the mechanical corpus sweep (41 files) lands as its own commit so the rule-change diff stays reviewable.

## 8. Open questions

- **Q1 — dead catch arms: error or warning?** Recommendation: **error**. The arm is provably unreachable (single inheritance, §2 item 6), matching the project's reject-dead-shapes posture (§31.2.1 already errors on the analogous generic-bound defect); a warning would leave `catch Dog =>` legal-but-inert, a new silent no-op. The cost is confined to the two deliberate test files, which flip anyway.
- **Q2 — migration staging.** Land the mechanical test sweep (`class E end E;` → `class E: Exception end E;`, 41 files) *ahead* of the rule flip as a no-op commit, so the flip's own diff is the two polarity-flipping tests plus new regression tests? Recommendation: yes — same pattern as 0077's Q4 (independent mechanical work ahead of the ruling).
- **Q3 — the rationale record.** The decision record (this proposal's §0–§3, condensed) belongs in rationale §11 beside §11.3's `catch`-is-`case` record; confirm placement. The v0.182 changelog entry is pinned history and is not edited; the new revision's entry records the re-ruling with the flag-discharge framing.

## 9. Interactions

- **0090 (transfer-marker study):** reads the same §31.1 announced-kill clause; 0090 leaves announced targets unmarked, so the two proposals compose with zero contact — stated in both.
- **0081 (abandonment lints):** no contact (different chapters, shared doctrine only).
- **Sibling front-half 0078 (checked-posture family, per the coordinator's index):** same doctrine — a language that faults loudly should not have a silent hole in its catch-all; the restored universal `Exception` arm is the posture win.
- **0076/v0.229 criterial rule:** the five-band failure-signal doctrine is written in `Exception` vocabulary throughout; this proposal makes that vocabulary total rather than partial. No band boundary moves.
