# Proposal 0089 — Does §19.6's generic-leaves determination rule earn its weight, post-choices? (study)

| | |
|---|---|
| **Status** | under-review — drafted 2026-08-11 (owner-directed, from the elegance review) |
| **Tracking issue** | [#3055](https://github.com/s8lap/bezel-lang/issues/3055) |
| **Target module(s)** | question-shaped — if ruled *retire*: **spec** §19.6 (the "Generic leaves: the determination rule" subsection deletes; the sealed-hierarchy bullet's generic clause shrinks to the replacement text in §5), §21.7 (one consequence sentence), Appendix C.7; **rationale** §8.2 (post-choices annotation) + a new decision record; **compiler** `src/sema/ExprTypecheck.cpp` (the determination machinery deletes); **tests** (the rule's family inverts to pin the new warning). If ruled *keep*: **rationale** only (§8.2 gains the re-affirmation; the study is the record). |
| **Written against** | spec/stdlib v0.241 |
| **Origin** | The 2026-08-11 elegance review (coordinator Tier 3 item 13; back-half critic finding), owner-accepted for study. The trigger: rationale §9.6 (v0.200) itself describes the rule as "a page of machinery that exists only because class sums encode the discriminant in the runtime *type*," and rationale §8.2 — the rule's only rationale anchor — predates choices. |

*Reference convention: unprefixed §-references are language spec references; rationale references are marked.*

---

## 0. The question

§19.6's sealed-hierarchy exhaustiveness bullet needs, for generic hierarchies, the **determination rule** (§19.6, "Generic leaves"): per leaf, compute the root instantiation by composing base-clause substitutions, solve for the leaf parameters against the scrutinee type under exact invariant identity, filter by constraint satisfaction, classify three ways (exempt / determined / undetermined) — with three-valued identity against rigid enclosing parameters, the §7.10 `?`-pre-image split, constraint decidability from type-parameter bounds, and a census prohibition. It is roughly two pages of the hardest text in §19 (v0.196, #1816), plus its own worked examples and two recorded rejected readings.

Since v0.200, the language has a second closed-sum kind — **choices** (§35) — whose exhaustiveness needs none of this: "Generic choices need no determination machinery … a choice's discriminant is a name, and the problem does not arise" (§35.6). The stdlib's own `Result<T, E>` is a choice (stdlib §2.20). The question the owner accepted for study: does the determination rule still earn its spec and implementation weight, or should generic-leaf matches simply require an irrefutable arm, with choices as the steered spelling for value-shaped generic sums?

## 1. What the rule buys

Exhaustive `case` — no `_` arm — over **generic** sealed class hierarchies, in the determined shape:

```bezel
public sealed abstract class Result<T> ... end Result;      // §19.6's own example
public sealed class Ok<T>(value: T): Result<T> ... end Ok;
public sealed class Err<T>(error: string): Result<T> ... end Err;

var label := (case r of                     // r: Result<int>
  var o: Ok<int> => $"ok: {o.Value}",
  var e: Err<int> => $"err: {e.Error}");    // exhaustive, no _ — the rule's proof
```

Concretely that buys three things: (a) the missing-arm warning stays precise when a leaf is added upstream (the §19.6 cross-module story); (b) expression-position matches carry the no-`MatchFailureException` guarantee without a `_`; (c) statement-position matches can receive §21.7's totality grant (a `case` statement that returns on every arm and is proven exhaustive discharges the method's return-path obligation — pinned by `tests/lit/sema/sema_ret_path__exhaustive_generic_case_stmt_no_trailing_return_total_spec_21_7_11_7.bzl`). It also buys the *soundness* half: the rule exists because the two cheap readings fail — declaration-coverage is unsound (claims a proof while `Ok<string>` escapes at scrutinee `Result`), and §19.6 records blanket-never-exhaustive as "over-broad: it regresses the determined shape below, the canonical sealed-match idiom."

**That last recorded rejection is the crux, and its premise is dated.** At v0.196, `Ok<T>`/`Err<T>` class hierarchies *were* the canonical sealed-match idiom — the language had no other closed sum. At v0.200 choices landed, rationale §9.6 explicitly re-drew the line ("hierarchies for behavior-carrying, identity-bearing sums, choices for value-shaped outcomes"), and the canonical Result moved to §2.20 as a choice. The determined shape's flagship inhabitant no longer exists as a class hierarchy anywhere in the tree.

## 2. What the rule costs

**Spec.** The generic-leaves subsection: ~42 dense lines plus examples (§19.6 lines 4016–4057 at v0.241) — the determination computation, rigid-parameter three-valued identity, the `?`-pre-image split, constraint satisfaction from bounds, the census prohibition, two rejected readings, the cross-module composition paragraph, and a recorded existential-pattern non-reading. Every future type-system change must answer "how does determination see this?" — the subsection already had to integrate aliases (§27.8), nullability idempotence (§7.10), enclosing-generic rigidity, and per-bound constraint decidability, and each future constraint kind or type form inherits the same obligation.

**Implementation, measured.** The §19.6-tagged determination machinery in `src/sema/ExprTypecheck.cpp` (v0.241): the helper family at roughly lines 648–1450 (~800 lines — ground-type walk, tri-state component matching, the `?`-split solver, ground-solution constraint checking, obligation formatting; built across #1816 stages s1–s8, then #1862, #1874) plus the exhaustiveness-checker integration at roughly lines 2030–2260 (~230 lines). Call it **~1,000 lines of sema**, with its own bug history (#1862 determined-leaf recording, #1874 ancestor-walk) — for comparison, the *entire* choice exhaustiveness story (§35.6) rides the name-counting the checker already did for enums.

**Users.** Zero cost when unused — the rule imposes nothing on programs that don't match generic sealed hierarchies. The cost is carried by the spec's readers and the compiler's maintainers.

## 3. What the alternative costs users

If the rule retires, a `case` over a generic sealed hierarchy (root or any leaf generic) requires an irrefutable arm. The delta, precisely:

- **Runtime semantics change nowhere.** The proof was sound; deleting it changes no arm selection. A formerly-proven match still matches every value; the difference is a warning (expression position) and the lost §21.7 grant (statement position). Adding the demanded `_` arm is one line, and for the determined shape the arm is genuinely dead code.
- **The determined-shape idiom loses its polish**: `case r of var o: Ok<int> => …, var e: Err<int> => …, _ => throw Verify-style-unreachable` — the `_` arm is exactly Guide 3 §2's "silences the compiler's help" cost, at the sites the rule served. That teaching tension is real and is the honest price.
- **The steered spelling for new code is a choice** (§35), which is *better* along every axis the value-shaped case cares about (no allocation, destructuring, name-counted exhaustiveness, no determination) — rationale §9.6's line. What choices cannot replace: behavior-carrying, identity-bearing generic sums — a generic sealed hierarchy whose leaves carry methods and object identity. That residual class keeps `_`-arm matching, or dispatches through virtual methods instead of `case` (the OO spelling, which such hierarchies usually want anyway).

## 4. Corpus evidence

Greps at v0.241:

- **stdlib: zero** `sealed abstract` declarations at all — no generic (or non-generic) sealed class hierarchy exists in the library; the library's closed sums are choices (`Result<T, E>`) and enums. (Proposal 0019's `JsonValue` hierarchy — draft, unlanded — is non-generic.)
- **docs: one** generic sealed root outside pinned history — §19.6's own `Result<T>` teaching example (the neighbouring `Message` example's root is non-generic; its generic content is the `Tagged<U>` leaf of §19.6's undetermined worked case). The guides never show a generic sealed hierarchy.
- **tests: six files** contain generic sealed roots — `sema/case_exhaustiveness_test.cpp` (39 occurrences: the #1816/#1862/#1874 determination suite itself), `sema/case_type_test_instantiation_1913_test.cpp`, `codegen/generic_type_test_arg_1843_test.cpp`, `codegen/result_stdlib_2047_test.cpp` (a pre-choice Result-name collision test, not a determination user), and two lit tests (the `?`-split warning pin; the §21.7 totality pin).
- **Conclusion:** the rule is exercised **exclusively by its own spec-derived test family**. No organic in-tree code — stdlib, guide, example, or non-§19.6 test — matches a generic sealed hierarchy exhaustively.

## 5. The retirement path, if ruled

**Deletion, not deprecation.** There are no users (§4); a deprecation window would warn nobody. One revision:

- **§19.6 replacement text (sketch)** — the sealed-hierarchy bullet keeps everything non-generic; its generic clause and the whole "Generic leaves" subsection reduce to:

  > When the closed hierarchy's root or any leaf is generic, leaf coverage is **statically unattainable**: type patterns name single instantiations, and the compiler does not compute which instantiations inhabit the scrutinee type (the v0.196–v0.24x determination rule is retired — see the changelog and rationale §8.2). Such a `case` is completable only by an irrefutable arm — an unguarded `_` or untyped `var` — and draws the non-exhaustiveness warning without one; the warning names the generic hierarchy and prescribes the `_` arm, and, for value-shaped sums, a `choice` (§35), whose exhaustiveness is counted by case name (§35.6) and needs no `_`.

  This is the previously-rejected blanket reading, adopted **with its rejection's premise expired** (§1) — the decision record says so explicitly rather than silently contradicting the v0.196 reasoning.
- **§21.7:** one sentence — statement-`case` totality over a generic sealed hierarchy requires the irrefutable arm (which then carries the return/throw); the in-tree lit pin flips to demand it.
- **Compiler:** the ~1,000-line determination machinery deletes; the exhaustiveness checker treats generic roots/leaves as unattainable-with-remedy (a few lines). The warning text is the new surface.
- **Tests:** the determination suite inverts — the determined-shape acceptance tests become warning-expectation tests; the `?`-split and rigidity tests retire with the distinctions they test; each flip ships spec-cited per `tests/README.md`.
- **Recoverability:** the full design stays in git history, the v0.196 changelog entry (pinned), and the rationale decision record; if evidence ever arrives, the cheaper future lift is the **existential pattern** §19.6 already names as a possible extension (`var o: Ok` matching any instantiation — which would make generic-leaf coverage *expressible* without any determination computation), recorded as the preferred reopening shape rather than restoring determination.

## 6. Keep and retire, weighed

**Keep.** The rule is sound, finished, implemented, and tested — its design cost is sunk; only maintenance remains. It is the sole mechanism by which behavior-carrying generic sums get `_`-free matching, and deleting a working guarantee because today's corpus doesn't use it forecloses tomorrow's framework code (where generic node hierarchies are plausible). The maintenance burden is speculative until the next type-system feature actually lands. Retiring also *spends* effort — spec surgery, code deletion, test inversion — to remove something that costs unused programs nothing.

**Retire.** The complexity budget is the honest ledger (0046's discipline): ~2 spec pages of §19's hardest text plus ~1,000 sema lines is a standing tax on every reader and every future type-system change, purchased for a capability with zero organic uses whose value-shaped majority case the language deliberately re-homed to choices (v0.200) — and whose canonical example, `Result`, the stdlib itself moved. The rule's own recorded defense against the blanket reading no longer has an inhabitant. The residual served class (generic, behavior-carrying, exhaustively `case`-matched rather than virtually dispatched) is narrow enough that a one-line `_` arm — or the existential-pattern lift, if it ever matters — is the right price. A finished mechanism is not free: #1862 and #1874 were post-landing defects *in* it, and the next ones arrive with the next type-system feature.

**Recommendation: retire (delete), medium confidence.** The deciding weights: zero organic corpus usage after two years of the rule's availability and one year of choices; the spec-weight asymmetry (the hardest §19 text serving the emptiest §19 use case); and the existence of a strictly better steered spelling for the shape that motivated it. The confidence is medium, not high, because the keep-side's foreclosure argument is real — which is why §5 pins the existential-pattern lift as the recorded reopening path rather than leaving retirement as a dead end.

## 7. Migration and compatibility (changelog categories, if retired)

- **Newly warned:** exhaustive matches over generic sealed hierarchies (previously proven) draw the non-exhaustiveness warning absent an irrefutable arm; in-corpus, only the rule's own test family (§4).
- **Newly rejected:** nothing (no program becomes an error; the §21.7 grant's loss surfaces as the existing missing-return diagnostic with the `_`-arm remedy).
- **Runtime behavior:** unchanged everywhere (§3).
- **Doc-only:** rationale §8.2 annotation, the decision record, §19.6's shrinkage.

## 8. Questions for the owner

- **Q1** — retire the determination rule (recommended, medium confidence; §6), or keep it and re-affirm rationale §8.2 with this study as the record?
- **Q2** — if retired: confirm **deletion in one revision** (recommended; no users to serve a deprecation window) rather than a deprecation cycle?
- **Q3** — if retired: confirm the **existential pattern** as the recorded reopening shape (recommended — it is the cheaper mechanism for the same capability), so the decision record steers any future re-ask away from restoring determination?
- **Q4** — if kept: should the §19.6 subsection at least gain the §35.6 cross-pointer prominently ("for value-shaped generic sums, prefer a choice — none of this subsection applies"), making the steering explicit in normative text? (Recommended under either ruling; under *retire* it is subsumed by §5's replacement text.)

## 9. Interactions

- **Proposal 0086 item 3 (#3052, `closed` spelling):** both edit §19.6's sealed-hierarchy prose; sequence in one editing pass if both land.
- **Proposal 0078 (#3044):** complementary §19.6 work — 0078 makes the *enum* bullet's proof true at runtime; this study sizes the *class-hierarchy* bullet's generic machinery. A joint changelog entry could state §19.6's per-scrutinee guarantees once.
- **Proposal 0065 (named-destructure patterns, graduated v0.224):** §19.6's named-destructure paragraph cites the determination rule for generic leaves; retirement simplifies that paragraph's generic clause (covered by §5's replacement text).
- **Rationale §9.6 / §35.6:** both already carry the "determination exists only for class sums" framing this study relies on; they are the record that the retire option completes rather than contradicts.
