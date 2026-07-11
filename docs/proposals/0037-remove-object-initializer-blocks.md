# Proposal 0037 (RFC) — Remove object-initializer blocks: `with` means record update, only

| | |
|---|---|
| **Status** | **superseded — graduated in v0.188 (`ebfa7e0`)**: §22.11 tombstoned, §9.5 record-only `with` + class-source rejection, §22.9 analogy removed, A.7 comments rewritten (no production change). Open questions resolved as delegated rulings (recorded in the v0.188 changelog). *(prior: accepted (owner, 2026-07-12); draft (RFC))* |
| **Tracking issue** | [#1648](https://github.com/s8lap/bezel-lang/issues/1648)
| **Target module(s)** | Spec [§22.11](../Bezel_Language_Spec.md#sec-object-initializer-blocks) (deleted), [§9.5](../Bezel_Language_Spec.md#sec-non-destructive-update-with) (class-source paragraphs deleted; three-uses note shrinks to two), [§22.9](../Bezel_Language_Spec.md#sec-owned-fields-and-exception-safety) (one cross-reference restated standalone), Appendix [A.7](../Bezel_Language_Spec.md#sec-expressions) (two comment blocks rewritten; **no production changes** — §6 explains why none are available). **No stdlib change. No grammar-production change. No conformance-level change** (the construct is Core today and its deletion leaves Core smaller, not repartitioned). |
| **Written against** | spec/stdlib **v0.183**. *(Carried to v0.184: the cited sections are unchanged by the 2026-07-11 ownership-coherence revision; re-verify at landing.)* Every citation below was re-verified against the v0.183 text (section numbers *and* wording), and the whole document was adversarially re-verified a second time by an independent critique pass (2026-07-11) whose two blocking findings are incorporated here. |
| **Design character** | **RFC** — a pure-simplification question, not a defect fix. Object-initializer blocks are sound today. The recommendation is argued; the owner decides. |
| **Blast radius (verified)** | Zero uses in the stdlib document, zero in both guides, zero in all 36 proposals. The construct's entire documented life is §22.11's own example plus the cross-references listed above. Test corpus: 27 `new … with` sites across 7 `.cpp` test files (single-line grep; the counting rule excludes `.bzl` conformance cases — see §8), of which the class-source subset is **~23 (~85%)**; the ~4 record-source sites are untouched. Conformance corpus: 2 cases derived from §22.11 itself (`spec_0137`/`spec_0138`) and 2 A.7 grammar-embed cases whose comment text mentions the construct (`spec_0288`/`spec_0293`) — all handled in §8. |

## 0. Summary

`new T(args) with X := v, Y := w end` on a **class** type is, by the spec's own words, "Equivalent to" three statements: bind a local, assign `X`, assign `Y` ([§22.11](../Bezel_Language_Spec.md#sec-object-initializer-blocks)). It is pure sugar — no capability, no invariant, no lifetime shape exists only in the sugared form. What the construct *does* uniquely contribute is the language's one **type-kind-dispatched semantic pun**: the same syntax, `with … end`, means *copy-with-overrides* when the source is a record and *setter mutation on a fresh instance* when the source is a class. §22.11 states the dispatch plainly:

> The resolution is purely a function of the constructed type's kind, invisible at the call site beyond the choice of class vs. record.

This is proposal 0035's access-site-dependence argument one level up. 0035 attacked the one rule under which a member's *static type* depends on where the access is written; this RFC attacks the one rule under which an expression's *semantics* — copy or mutate — depends on a fact (the constructed type's kind) invisible in the expression itself. The spec has to say so explicitly ("the difference is semantic, not syntactic", [§9.5](../Bezel_Language_Spec.md#sec-non-destructive-update-with)); the rationale has already paid two reconciliation entries keeping the two meanings from contradicting each other (§2); and the two meanings have quietly grown **two different accessibility regimes** and **two different invariant stories** under one spelling (§1.2, items 4–5).

This proposal recommends **deleting the class-source meaning**: `with … end` requires a record-typed source; a class-typed source is a compile error naming the rule, with the fix-it the spec already prints (bind first, then assign — or use named/optional constructor arguments, [§22.5](../Bezel_Language_Spec.md#sec-optional-parameters-and-named-arguments), which run the constructor and respect its invariants). Record `with` — the load-bearing meaning, the only non-destructive update on immutable records — is **untouched**, including on a freshly constructed record (`new Point(1, 2) with Y := 9 end` remains legal record-`with`; the line is drawn at the source expression's *type kind*, not at the presence of `new`). `await for … with token` ([§30.7]) is likewise untouched; §9.5's "three uses of `with`" note shrinks to two.

Two things this proposal is **not**:

- **Not a bug fix.** The construct is specified coherently and implemented. The finding, adversarially verified this run (simplification-01, CONFIRMED with all blast-radius claims checked), is that it is *removable at near-zero cost*, not that it is broken.
- **Not a change to records.** Every sentence of §9.5 that speaks about record sources survives verbatim. The `with` PrimarySuffix and `InitAssign` production stay in the grammar.

## 1. The mechanism today (v0.183)

### 1.1 The construct and its own equivalence

[§22.11](../Bezel_Language_Spec.md#sec-object-initializer-blocks), in full effect:

```bezel
var t := new WaferTest(zUp := true) with
  SaveResults := true,
  NotifyUi := false
end;
```

> Equivalent to:
>
> ```bezel
> var t := new WaferTest(zUp := true);
> t.SaveResults := true;
> t.NotifyUi := false;
> ```

Only `Name := value` pairs; no statements, control flow, or method calls; targets must be public settable properties or public fields; assignments run in textual order after the constructor returns. The replacement idiom is therefore printed inside the rule being deleted — the migration template is the spec's own text.

Note what the example itself demonstrates: the constructor argument is already spelled `zUp := true` — a **named argument**. Bezel shipped named and optional constructor arguments from the start ([§22.5](../Bezel_Language_Spec.md#sec-optional-parameters-and-named-arguments)); C# built object initializers in 2007 partly because it lacked them for years. In Bezel, the type author's answer to "construct-then-configure" is an optional parameter — which runs through the constructor and its validation — and the sugar arrived redundant next to it.

### 1.2 The carve-outs it forces elsewhere (the ledger)

Per the 0035 discipline: the construct is not one rule; it is one rule plus every other rule that must know about it. As of v0.183:

1. **The semantic fork itself**, documented three times: §9.5 ("the difference is semantic, not syntactic … For a record source, `with` produces a copy … For a class source (always a freshly-constructed instance), `with` assigns through the named properties' setters, mutating the new instance"), §22.11's closing paragraph (the type-kind dispatch quoted in §0), and Appendix A.7's `PrimarySuffix` commentary ("The form is syntactically unified; the type checker resolves the semantic difference from the source expression's type") plus a second A.7 comment on `NewExpr` ("The object-initializer form … is parsed as `NewExpr` followed by a `with` PrimarySuffix; there is no separate ObjectInit production").
2. **A three-way keyword disambiguation note** (§9.5): "The `with` keyword has three uses across the language: record non-destructive update (this section), object initializer blocks on `new` expressions (§22.11), and cancellation in `await for … with token` (§30.7)" — with a paragraph explaining how the three are told apart. Deleting the class case removes the only member of the trio that is *not* syntactically self-evident (the other two are distinguished by grammar position; the record/class pair needs the type checker).
3. **A construct-specific disposal restatement plus a dedicated codegen guard for the unattached window.** §22.11's own text frames the disposal as *participation in* [§28.3](../Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)'s general discharge ("participates in the §28.3 exception-path discharge rule the same way any owned `new` result does"). What is genuinely construct-specific is the **unattached-owned state**: the instance is *unbound* during the setter sequence, so the spec must restate the discharge for a value the reader cannot see bound anywhere, and the compiler must carry a dedicated guard for that window (the class-path landing-pad handling in codegen, with its recorded bug-fix trail — item 7). In the two-statement form the instance is an ordinary owned local from the moment it exists, and §28.3 covers every path with no special text and no special codegen (§7 traces this).
4. **A second accessibility regime.** Record `with` targets follow the v0.180 rule: accessible at the use site under the member's own declared accessibility, setter irrelevant (§9.5). Class-source `with` targets must be "public settable properties or public fields" (§22.11) — a different rule, keyed on the same syntax by type kind. Two accessibility regimes on one spelling is exactly the shape of drift the v0.180 accessibility train had to reconcile once already; retiring the fork retires the standing obligation to keep them reconciled. *(This item rests on the document text alone. An earlier draft additionally claimed the reference compiler deviates from §22.11's public-only rule in the test corpus; adversarial re-verification found that claim **false** — every class-source `with` target in the corpus is declared `public`, underscore-named fields notwithstanding — and it is withdrawn.)*
5. **A second invariant story.** §9.5 warns that record `with` bypasses the constructor and recommends named methods for constrained updates — with immutability as the mitigating structure. Class-source `with` has the same bypass (setters run after validation) *without* the immutability excuse, and the spec never restates the warning on the class side. The replacement idiom (constructor arguments) is the only construct-then-configure form that *runs the validation*.
6. **§22.9's cross-reference**: the `this(...)`-chaining unwind rule is defined by analogy — "the same rule [§22.11] applies when an object-initializer setter throws" — so the exception-safety chapter of constructors currently depends on the object-initializer section for one of its cases.
7. **Compiler machinery**: the `WithExpr` node dispatches by source type-kind through sema (`ExprTypecheck_Exprs.cpp`'s §22.11 path), definite-assignment, static-init cycle analysis, ownership, and two codegen files; the class path carries its own landing-pad handling for the unattached-owned state (`LlvmBackend_Exprs2.cpp` `lowerWith`, ≈L1754–2089, with its own bug-fix trail: the #747-R43 exception-path fixes, the #429 `Object`-typed-field boxing interplay, and a dedicated `using var … with` disposal seam in `LlvmBackend_Setup.cpp` ≈L12365–12370). The record path survives; the class path retires — net-negative compiler code.

Each item is small. Their sum is the cost of the pun: every future rule that touches `with` — and the v0.180 accessibility train touched it — inherits the obligation to answer "for which of the two meanings?"

## 2. Recorded decisions engaged

Per the house rule, the recorded decisions adjacent to this change, and why none pre-decides it:

- **v0.58→v0.59 changelog** (rationale): replaced the flatly wrong sentence "`with` does not apply to classes" with the precise two-semantics statement now in §9.5. A *reconciliation* of the pun, not a defense of it.
- **v0.69→v0.70 changelog** (rationale): softened §22.11's wording because the v0.69 text read as if `new RecordType(...) with … end` were rejected, contradicting §9.5 — the type-checker resolves such expressions to record-`with`. A second reconciliation: the two meanings had drifted into contradiction and had to be re-aligned.
- **v0.73→v0.74 changelog** (rationale): moved `with` from a left-recursive standalone `WithExpr`/`ObjectInit` pair to the unified `PrimarySuffix`, "the §9.5 rule that 'the difference between record-with and class-new-with is semantic, not syntactic' is now reflected in the grammar itself." This is a *grammar-shape* decision (suffix placement) that this RFC preserves; it unified the parse, it did not rule on whether both semantics should exist.
- **v0.181 changelog** (rationale, field-initializers-once entry): §22.9 gained the chaining-body-throw case "the same rule §22.11 applies" — a use of the construct as a reference point, engaged in §7.
- **No recorded decision defends the class-source form.** The rationale's full `with`/object-initializer trail (verified: the four entries above are the only ones) contains reconciliations and a grammar relocation — never a considered-and-kept judgment that one syntax *should* carry two type-kind-dispatched semantics, and no entry weighing the sugar against §22.5's named/optional arguments. Like 0035's and 0036's subjects, the construct's presence is a **default inherited from C# familiarity, not a decision** — and unlike C#, Bezel never had the gap the C# feature filled.
- **DECLINED.md**: nothing adjacent. D-0002 (anonymous types) is about structural typing, not initializer syntax; no decline record touches `with`.

## 3. What replaces it

### 3.1 The idioms

For types you author — **named/optional constructor arguments** (§22.5), the strictly stronger tool (runs the constructor, respects invariants, works in expression position):

```bezel
public class WaferTest
  public constructor(zUp: bool := false, saveResults: bool := false, notifyUi: bool := true);
  // ...
end WaferTest;

var t := new WaferTest(zUp := true, saveResults := true, notifyUi := false);
```

One qualifier, stated here so nobody re-discovers it as a gap at landing: per §22.5 (v0.160), **the *named*-argument spelling is for plain-value arguments only** — a `ref`/`out`/`owned` argument is passed positionally. The replacement idiom is unaffected in substance (the construct-then-configure values this sugar carried were settable-property values, i.e. plain values; and where a constructor takes `owned` arguments they pass positionally exactly as they always have) — but "named/optional arguments" as the headline replacement carries this parenthetical.

For types you don't author — the **bind-first rewrite**, i.e. the spec's own "Equivalent to" snippet:

```bezel
var t := new WaferTest(zUp := true);
t.SaveResults := true;
t.NotifyUi := false;
```

This is not a new idiom being invented for the occasion: bind-first is already Bezel's pervasive answer wherever a temporary needs a name — §9.7's no-temporary-source rule for borrow-bearing records ("bind the source to a named local … and pass that"), §28.14's v0.180 scoped-argument rule (`new StreamReader(scoped new FileStream(...))` rejected; bind the stream first), §30.5's Pattern A named-source discipline. The deleted sugar was the one construct pulling in the *opposite* stylistic direction.

### 3.2 What dissolves into existing rules

| §22.11 rule (v0.183) | Becomes |
|---|---|
| Assignments in textual order, after the constructor returns | statement order — the rewrite *is* a statement sequence |
| Targets: public settable properties / public fields | ordinary member-access and accessor-visibility rules ([§13], [§23]) on ordinary assignment statements — one regime, no per-construct target rule |
| Setter throws → preceding assignments not rolled back | ordinary statement semantics |
| Setter throws → instance disposed (unattached-owned state, implicit `try`/`finally`) | [§28.3](../Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) exception-path discharge of the owned local `t` — the general rule, no construct-specific text (trace in §7) |
| §22.9's "the same rule §22.11 applies" for a chaining ctor's body throw | restated standalone: the fully-initialized instance is disposed through the normal generated-`Dispose()` path before the exception propagates — which is what §22.9 already says in its own words; only the analogy-citation is deleted |
| §9.5's class-source paragraphs and the record/class fork | deleted; §9.5 states one meaning and adds one rejection sentence: *a `with` expression whose source is class-typed is a compile error naming this rule; classes use constructor arguments (§22.5) or property assignment* |
| A.7's two "the type checker resolves the semantic difference" comments | rewritten to: the `with` suffix requires a record-typed source (§9.5); class-typed sources are rejected semantically — the same over-admit-and-reject-semantically pattern A.4 uses throughout |

## 4. The line, drawn precisely

**Untouched — record `with` expressions ([§9.5](../Bezel_Language_Spec.md#sec-non-destructive-update-with)), in every position they occupy today:**

- `p1 with Y := 99 end` on any record-typed expression — locals, parameters, fields, call results.
- `new Point(1, 2) with Y := 9 end` — a **fresh record construction is a record-typed source expression**; the type checker resolves this to record-`with` today (§22.11's closing paragraph, v0.70) and continues to. The presence of `new` is *not* the discriminator and never was; the constructed type's kind is. After this RFC the kind check has one outcome that errors (class) instead of one that changes meaning.
- The v0.180 accessibility rule, the no-setter-invocation semantics, the computed-property exclusion, the invariant caution — all verbatim.
- `await for … with token` ([§30.7]) — different grammar position, untouched. §9.5's three-uses note becomes a two-uses note (record update; `await for` cancellation), and loses the only pair that needed the type checker to tell apart.

**Deleted — the class-source meaning:** §22.11 wholesale; §9.5's class-source sentences; the A.7 comments' class half; §22.9's citation (restated); the compiler's class-source `WithExpr` path.

## 5. What is genuinely lost — expression-position construction, traced

The one-expression form `new T(...) with … end` can appear anywhere an expression can. The rewrite requires a statement context for the assignments. Where that bites, per position (this list is intended to be **exhaustive** over A.7's expression positions):

1. **Argument position** — `Render(new Style() with Bold := true end)`. Rewrite: bind first, then pass the local under the [§22.3] `owned` call-site marker where the parameter is `owned` — `var s := new Style(); s.Bold := true; Render(owned s);` — or as a plain (borrowing) argument otherwise. One extra line; identical semantics; matches the §3.1 bind-first family. This is the common case and the cheap one.
2. **Return position** — `return new T() with X := v end;`. Rewrite: bind, assign, `return t;` (owned-local return transfers, §28.3). One extra line.
3. **`using var` header** — `using var h := new C() with f := v end;`. Rewrite: `using var h := new C(); h.f := v;`. The disposal guarantee is unchanged (§7); the test corpus's dedicated `objinit_usingvar_disposal` seam retires with the construct.
4. **Expression-bodied members** — `public method Make(): owned T => new T() with X := 1 end;` must become block-bodied (three lines). Real, small, rare (zero occurrences anywhere in the corpus's documents; the corpus has no such site in tests either). Lambda bodies are the same shape with a cheaper remedy: `LambdaBody` has a `do … end` block alternative (A.7), so a lambda re-spells in place.
5. **Field-initializer position** — `private owned _cfg: Config := new Config() with Depth := 3 end;`. The honest worst case: a field initializer is a single expression, so the assignments cannot follow in place. Rewrites: (a) constructor-body assignment after the initializer (`_cfg := new Config();` in the initializer, `_cfg.Depth := 3;` in the ctor body — or both in the body); (b) a private static factory returning `owned Config` (§28.3's factory convention); (c) if `Config` is yours, a constructor parameter. For a **static** field initializer, remedy (a) does not apply (there is no per-instance ctor body to assign in); remedy (b) — the static factory — does, and §22.13's static-initialization ordering is unaffected (the initializer remains one expression; it now calls a factory). Three workable forms, none one-line. No such site exists in the corpus.
6. **Collection-literal elements** — `[new C() with X := 1 end, …]` (`CollectionLiteral`, A.7). Bind-first works: hoist each element to a named local before the literal, then build `[c1, c2, c3]` — which is already the stdlib's own idiom for owned-element literals (stdlib §4.5's `OwningArray<Connection>` example constructs `[c1, c2, c3]` from named locals with ownership transfer per element). One line per element.
7. **Nested-expression positions with no adjacent statement context** — an arm of a `when`/`case` expression, an operand of a conditional expression. Here the bind-first rewrite is **not local**: an arm has no statement context to host the assignments. The remedy is a **factory hoist** (a small private method returning `owned T`, per §28.3's factory convention) or restructuring the conditional into statements. Zero such sites exist anywhere in the corpus, documents, or tests.

The rewrite is **local and mechanical in every statement-adjacent position** (1–3, 6, and lambdas via `do … end`); in nested-expression positions (7) and the expression-bodied/initializer positions (4–5) it is a factory hoist or a block-bodied restructuring — still mechanical and behavior-preserving (§7 for the exception paths), but not one-line-local, and this RFC prices that honestly rather than claiming locality everywhere. What is permanently given up is a *spelling* — one expression instead of two-to-three statements, or a two-line factory in the worst positions — never a capability. That loss is real and is the whole price; §9 counts it against the ledger of §1.2.

## 6. Grammar: what changes and what cannot

**Recommended (and only viable) shape: grammar unchanged, semantics restricted.** The A.7 `PrimarySuffix` alternative `"with" InitAssign ("," InitAssign)* ","? "end"` and the `InitAssign` production stay exactly as they are — records need them. The restriction (`with` requires a record-typed source) is a type-checker rule, exactly like the many A.4 over-admit-and-reject-semantically notes (0036 §1's precedent list).

**The "cleaner grammar split" considered in this run's phase-1 memo is not actually available**, and this RFC records why so the idea stays refuted: `new Point(...) with … end` (record — legal, stays) and `new WaferTest(...) with … end` (class — newly rejected) are **syntactically identical**. No production can separate them; only the type checker can. There is consequently *no grammar deletion in this RFC at all* — the parser win is zero, and the RFC does not claim one. The wins are semantic: one meaning per syntax, the §1.2 ledger dissolved, and a construct-specific disposal window deleted. (A.7's two comment blocks are rewritten to describe the record-only semantics; comments are not productions.)

## 7. Exception safety: the construct-specific rule dissolves, verified path by path

§22.11's disposal paragraph exists because the sugar's instance is *unbound* during the setter sequence. The rewrite binds it immediately, and the general rules take over:

- **Setter throws, plain `var` binding.** `var t := new T(); t.X := f();` — if `f()` or the setter throws, `t` is a live owned local on the unwinding edge; §28.3's exception-path discharge ("the compiler treats every owned local … as if it were wrapped in an implicit `try`/`finally` whose `finally` disposes the value") disposes it. Same guarantee as §22.11's rule — disposed exactly once, before the exception leaves the binding's scope. Because the mechanical rewrite places the binding at the site the expression occupied, even try/catch nesting behaves identically.
- **Setter throws, `using var` binding.** `using var h := new C(); h.f := v;` — `using var`'s scope-exit disposal runs on the exception edge (§28.10/§28.3). Identical outcome.
- **Constructor throws.** Unchanged in both worlds — §22.9 covers construction; the block/statements never run.
- **§22.9's chaining case.** Currently defined by analogy to §22.11. Restated standalone with text §22.9 already contains: a throw in a `this(...)`-chaining constructor's body disposes the by-then fully-initialized instance through the normal generated-`Dispose()` path before the exception propagates. No semantic change; one dangling analogy removed.

Net: the *entire* §22.11 disposal paragraph — unattached-owned state, implicit cleanup, "leaks neither memory nor partially-attached resources" — becomes a consequence of rules that already exist, applied to a binding the reader can see. This is the same dissolution shape as 0035 §5.2's table: the special text was restating general rules for a construct that hid the binding.

## 8. Migration

- **Spec**: delete §22.11 (renumbering per the house convention for section removal — tombstone or renumber, editor's choice at landing; §22.12/§22.13 references are unaffected either way if a tombstone is used); §9.5 per §3.2/§4 (delete two class-source passages, shrink the three-uses note, add the rejection sentence with its fix-it); §22.9 per §7; A.7's two comment blocks per §6. The `with` keyword (Appendix B.1) is untouched — it keeps two live uses.
- **Rationale**: gains this decision's record (one entry; cite the two reconciliation entries as the pun's recorded cost). The historical changelog entries are append-only history and are not edited.
- **Stdlib**: none (zero uses, verified).
- **Guides**: none (zero uses, verified).
- **Proposals**: none (zero uses across all 36, verified).
- **Compiler**: retire the class-source `WithExpr` paths (§1.2 item 7); add the §9.5 rejection diagnostic (source is class-typed → error naming the rule, fix-it: "bind the instance to a local and assign the members, or use constructor arguments (§22.5)"). The record-source paths are untouched. Net-negative code; the retired path takes its bug-fix seams (#747-R43, #429-interplay, the `using var` disposal special case) with it.
- **Tests (`.cpp`)**: 27 `new … with` sites in 7 files (verified by corpus grep; single-line pattern, `.cpp` only). The class-source subset — **~23 sites (~85%)**: `with_init_test.cpp` (slice CG34, 12–13 sites), `objinit_usingvar_disposal_test.cpp` (7), `escaping_string_refcount_test.cpp` (2), and the class cases of `with_expression_target_validation_875_test.cpp` (2) — either re-spells to the two-statement form (where the test's subject is something else) or converts to a rejection test (where the test's subject was §22.11 itself). The ~4 record-source sites (`record_computed_property_test.cpp`, `serialization_synthesis_550_test.cpp`, and the two type-agnostic parser sites in `with_and_unit_value_test.cpp` — the grammar is unchanged, so parser tests survive) are untouched. New spec-derived tests: class-source `with` is a compile error naming the rule; fresh-record-`with` still resolves to record semantics.
- **Conformance corpus** (per #1380 the committed cases are hand-maintained and do **not** regenerate when the spec is edited — every item here is a hand edit): delete `tests/conformance/cases/spec_0137.bzl` (the §22.11 example verbatim) and `spec_0138.bzl` (its "Equivalent to" rewrite) together with **manifest rows 139–140** (`2211-Object-initializer-blocks`, both currently `skip:incomplete-fragment`) — or convert `spec_0137` into the new §9.5 rejection case, which is the recommended shape since it pins the new rule. Separately, `spec_0288.bzl` (manifest row 290, `skip:ebnf-grammar`) and `spec_0293.bzl` (case file present **without** a manifest row) embed the A.7 grammar text including the two comment blocks §6 rewrites; their drift is absorbed by the next A.7-touching regen or hand edit and needs no dedicated action beyond this note.
- **User code**: none exists (pre-users project).

## 9. Honest costs

Counted per-use against the ledger of §1.2:

1. **One to two extra lines** at every construct-then-configure site for types whose constructors don't take the values (§5 positions 1–3, 6); **three-ish lines or a small factory** in the rare positions (4–5, 7). Zero sites exist in the documents; ~23 class-source sites across 7 test files plus the two §22.11-derived conformance cases (§8).
2. **C# familiarity.** A C# reader recognizes object initializers; their absence is a (small) teaching item. Mitigation: the diagnostic's fix-it teaches the idiom at first contact, and the C# reader's *other* reflex — `with` on records — now has exactly the C# 9 meaning with no class-side surprise.
3. **The one-expression spelling is gone even where it read well** (argument-position configuration of a foreign type). Given up, not refuted. The counterweight is that the spelling was available *only* for public-settable surfaces and silently skipped constructor validation — the cases where it read best are the cases where a named-argument constructor is the better API anyway.
4. **A migration touch on ~7 test files + 2 conformance cases** and the compiler retirement work — bounded, listed in §8.

And the gains, symmetrically: the language's only type-kind-dispatched expression semantics deleted; `with` means one thing; a construct-specific disposal window and its codegen guard, a three-way keyword note, a second accessibility regime, a second invariant story, and §22.9's analogy dependence all dissolve into general rules; the compiler sheds a dual-dispatch seam with a recorded defect history; every future rule touching `with` (the v0.180 train will not be the last) answers one question instead of two.

## 10. Conformance

None at the *level* dimension. The construct is Core today by default; after deletion there is nothing to partition. No Appendix D edit. (Conformance-*corpus* file edits are migration work, listed in §8.)

## 11. Sizing

| Piece | Size |
|---|---|
| Spec edit | Small — one section deleted, two sections' paragraphs adjusted, two grammar comments rewritten, one changelog entry |
| Rationale edit | Small — one decision record |
| Stdlib / guides / proposals | None (verified zero uses) |
| Compiler | Small-to-moderate, net-negative — retire the class-source WithExpr path across sema/ownership/codegen; add one rejection diagnostic |
| Tests | ~7 `.cpp` files touched; class-source sites re-spelled or converted to rejection tests; 2 conformance cases deleted or converted + 2 manifest rows; new spec-derived rejection cases |

## 12. Owner questions

1. **Q1 — the ruling**: delete the class-source meaning? Recommended: **yes** — this is the 0035-shape trade (a real but small per-use ergonomic price for the deletion of a semantic fork and its carve-out family), with a blast radius verified at zero in all documents.
2. **Q2 — the §9.5 rejection sentence's fix-it**: recommend naming both remedies (bind-first *and* §22.5 constructor arguments) in the diagnostic, teaching the stronger idiom first.
3. **Q3 — §22.11's section number**: tombstone ("§22.11 — retired, vX; see changelog") vs renumber §22.12+. Recommended: tombstone — external citations of §22.12 (static members, heavily referenced) must not shift.
4. **Q4 — timing**: nothing sequences against this; it can ride any docs revision. The compiler retirement can lag the spec (the implemented-but-no-longer-specified window is the established spec-ahead-of-impl pattern, with the tracking issue filed at landing).

## Recommendation

**Adopt** — delete the class-source meaning of `with … end` (§22.11 and its tendrils), keep record `with` untouched, restrict by type-checker rule with no grammar change, and record the decision with the two reconciliation entries as its cost history.

**Confidence: high.** Every load-bearing claim is mechanically verified against v0.183 twice (author pass + independent adversarial critique; the critique's killer-counterexample hunt over every A.7 expression position found none — a rewrite exists everywhere), the replacement idioms are existing, recorded house patterns, and the loss is a spelling, priced honestly in §5/§9. The only judgment call is taste — whether the one-expression form's convenience outweighs the pun — and the owner's stated appetite for 0035-shaped removals is exactly this trade.

---

## Finalization record (2026-07-11, phase 3)

Adversarial critique verdict: **REVISE** (2 blocking, 5 advisory). Disposition:

- **B1 (blocking, fixed)** — the §1.2 item 4 claim that the reference compiler deviates from §22.11's public-only rule was **false** (every class-source `with` target in the test corpus is `public`; the underscore-named fields are public). The claim is deleted; item 4 now rests on the two-regimes document text alone, with the withdrawal noted inline so it is not re-added.
- **B2 (blocking, fixed)** — the migration section omitted the conformance corpus. §8 now lists `spec_0137`/`spec_0138` + manifest rows 139–140 (delete or convert; conversion recommended), notes `spec_0288`/`spec_0293`'s A.7 embeds (incl. that `spec_0293` has no manifest row), and states the #1380 hand-edit reality. The header's site count now states its counting rule (`.cpp`-only, single-line grep).
- **A1 (fixed)** — "roughly half class-source" corrected to ~23 of 27 (~85%) with the per-file breakdown in §8.
- **A2 (fixed)** — §5 gained collection-literal elements (position 6, with the stdlib §4.5 idiom), `when`/`case`-arm nested positions (position 7, factory hoist — explicitly *not* local), static field initializers (folded into position 5), and the lambda `do … end` remedy (position 4); the blanket "local in every position" claim is softened accordingly.
- **A3 (fixed)** — §1.2 item 3 reworded from "bespoke exception-path disposal rule" to "construct-specific restatement plus a dedicated codegen guard for the unattached window", matching §22.11's own framing.
- **A4 (fixed)** — §3.1 carries the §22.5 v0.160 qualifier (named form is plain-value-only; `owned` arguments positional).
- **A5 (fixed)** — §5 position 1 shows the §22.3 `owned` call-site marker.

No claims other than B1's parenthetical were withdrawn; no substantive position changed.
