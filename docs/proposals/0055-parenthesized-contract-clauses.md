# Proposal 0055 — Parenthesized contract clauses: `requires(E)` / `ensures(E)`

| | |
|---|---|
| **Status** | under-review (adversarial critique run 2026-07-21; all findings resolved — see §10) |
| **Tracking issue** | [#2074](https://github.com/s8lap/bezel-lang/issues/2074) |
| **Target** | Spec: §22.16 (all clause spellings and the body-less-closure rule), Appendix A.4 (`ContractClause`, `InvariantDecl`, `IfaceMethod`/`IfaceStaticMethod` body slots, the `MethodDecl` comment), Appendix B.2 (clause-region row wording), §22.4/§2.8.2 (the desugared `static ensures` spelling in examples), Appendix C.15; stdlib §2.17 (the one cross-reference row); guides: none (contracts are not yet taught there). Impl: the #2053 slice-1 parser + its tests (mechanical); slices 2+ (typing, lowering, carriage) are surface-independent and untouched. |
| **Written against** | spec v0.205 |
| **Origin** | Owner review of the v0.203 contract-clause surface (2026-07-21): the `; ;` body-less redundancy (#2070), the `; =>` expression-body seam, and the clauses-read-as-statements misreading. |
| **Depends on** | Nothing new. Amends proposal 0043's graduated surface (v0.203); 0043's semantics, tiers, inheritance, and carriage (0044, v0.204) are untouched. |
| **Design influences** | C++26 contracts (`pre(expr)` / `post(r: expr)`); D (`in(expr)` / `out(r; expr)`, the short forms since 2.081); Bezel's own keyword-paren family (`old(E)`, `nameof(…)`, `typeof(…)` — `old(E)` already lives *inside* every contract clause). |

*Reference convention: unprefixed §-references are language-spec references; "0043 §N" refers to proposal 0043's sections.*

---

## 1. Motivation: three surface warts, one root

v0.203's clause grammar gives every clause its own terminator: `ContractClause ::= "static"? ("requires" | "ensures") Expression ";"`. Three observed costs, all traceable to that one `;`:

1. **The body-less closure special case.** A clause-bearing abstract or interface method ends "at its last clause's `;`" — so the lone-`;` MethodBody alternative had to be declared *unreachable with clauses* (A.4's comment), an extra `;` had to be declared an error (0043 §3.2: "there is exactly one way to spell each form"), and the parser needs statement-starter lookahead to discriminate a body-less member from a block body. The implementation promptly drifted lenient (accepting `requires x > 0; ;`) and a test pinned the divergence — **#2070**. A rule that needs this much scaffolding to keep one spelling is fighting its own grammar.
2. **The `; =>` seam.** An expression-bodied member reads `requires x > 0; => x;` — a terminator immediately followed by an introducer. Well-defined, but the owner review's judgment (shared by this author) is that it reads as a wart, and no formatting rule fully rescues the one-line form.
3. **Clauses read as statements.** In a block body, `requires amount > 0m;` is visually identical to the first body statement — which invited the (reasonable, wrong) reading that clauses *are* statements, a design that would forfeit slot contracts on body-less members (the feature's strongest use case, §22.16's "intended home"). Syntax that misleads readers about the construct's category is a cost even when the grammar is unambiguous.

## 2. The change

```
ContractClause  ::= "static"? ("requires" | "ensures") "(" Expression ")"
InvariantDecl   ::= "static"? "invariant" "(" Expression ")" ";"
```

The clause's parentheses are **structural** — part of the clause syntax, exactly as a `while` condition's parentheses would be in a C-family language — not part of the expression. A clause is self-delimiting; it carries no terminator. `InvariantDecl` keeps its trailing `;` because it is a *member*, and members terminate with `;`.

Body slots revert to uniformity:

```
MethodDecl      ::= … GenericConstraints* ContractClause* MethodBody
                       /* MethodBody's ";" alternative is reachable for every
                          clause count — the body-less-closure rule is deleted */
IfaceMethod     ::= … ContractClause* ";"
IfaceStaticMethod ::= … ContractClause* ";"
                       /* uniform: every interface member ends with ";" */
```

### The four member forms

```bezel
// Expression body — no terminator-then-introducer seam:
public method Identity(x: int): int
  requires(x > 0)
  => x;

// Body-less (abstract / interface) — the lone ";" is the ordinary terminator;
// a redundant ";" cannot arise because clauses carry none:
method M(x: int): int
  requires(x > 0)
  ensures(result > 0);

// Block body — clauses now visually distinct from statements:
public method Withdraw(amount: decimal)
  requires(amount > 0m)
  requires(amount <= _balance)
  ensures(_balance = old(_balance) - amount)
  _balance := _balance - amount;
end Withdraw;

// Constructor and invariant member:
public constructor(limit: decimal)
  requires(limit >= 0m)
  _limit := limit;
end constructor;

invariant(_balance + _limit >= 0m);
```

All three §1 warts are dissolved at the root rather than patched: no `; ;` (unspellable), no `; =>` (the arrow follows a `)`), and the clause region is visually a distinct category from the statement list.

## 3. Disambiguation (normative analysis)

- **Region entry is unchanged.** The Appendix B.2 marker-wins rule stands verbatim: after `GenericConstraints*` (methods) / the parameter list (constructors), the tokens `requires`/`ensures` — or `static` followed by either — open the clause region, and the keyword reading wins there. The region is the maximal run of `ContractClause`s and ends at the first token that is not a clause opener.
- **Clause interior is trivially delimited.** After the marker, the parser matches `(` Expression `)`; inner parentheses nest by the ordinary expression grammar. There is no expression-continuation question at the closing `)` because the outer parens are clause syntax: in `requires(x > 0)(foo).Bar();` the clause is `x > 0` and `(foo).Bar();` is the first body statement — under v0.203's grammar the same text parsed as a *complete* clause whose expression was `(x > 0)(foo).Bar()` (a call of a call), consuming the final `;` as the clause terminator; the mistake surfaced only as a later type error, never at the token that went wrong.
- **The first-statement collision** (a body statement invoking a parameter or static named `requires`/`ensures`) is the same collision class as v0.203; what changes is the failure mode, and it differs by member kind. *Constructor:* `requires(x);` parses as the clause `requires(x)` followed by a stray `;` in statement position — an immediate parse error (Bezel has no empty statement). *Method:* the `;` after the clause matches the lone-`;` MethodBody alternative, so the member parses as **body-less** and is rejected semantically ("a body-less method must be declared `abstract`"), with any intended body statements then erroring in member position — loud, at the member, though not a parse error. The migration therefore adds the §22.16 remedy (parenthesize `(requires)(x);` or rename) as a note on the non-abstract-body-less diagnostic whenever the member carries clauses. *Abstract/interface members:* no collision exists — there is no body whose first statement could be a call. The comparison that matters: under v0.203, `requires (x);` with `x: bool` parses as a **legal clause** — the intended call silently becomes a precondition and the program compiles with changed meaning; under 0055 the same text is a hard error on every bodied member. The worst case flips from silent to loud.
- **No lambda ambiguity.** The bare-clause alternative (`requires E` with no delimiters, §8.2) founders on `requires (x) => x` — clause `(x)` plus expression body, or clause-expression lambda `(x) => x`? Structural parens make the question unaskable: the clause is whatever sits between the clause's own parens.

## 4. What this deletes

1. §22.16's body-less-closure rule and A.4's "reachable only with zero clauses" comment — the concept "a declaration ends at its last clause's `;`" leaves the spec.
2. The parser's statement-starter discrimination for clause-bearing members (`tokenLooksLikeStatementStarter` and the `clausesTerminateBodyless` logic) — the post-clause dispatch is exactly the pre-contracts body dispatch: `;` → abstract, `=>` → expression, else block.
3. **#2070 in its current form** — the divergence it reports becomes unspellable. The issue is folded into this proposal's migration (its regression-test guidance transfers: the new reject test is `requires(x > 0);` followed by a second `;`, which now fails as a stray member `;`… i.e. the ordinary error, needing no dedicated diagnostic).

## 5. What this does not touch

Everything semantic. Clause typing (§22.16: `bool`, pure, borrow-only), evaluation order, tiers and the `static` qualifier, `ContractViolationException`, inheritance/multi-root combination, the §22.4 `when`-clause identity (its desugared spelling becomes `static ensures(not result or p <> null)` — same declaration, same records), async `requires`, and the v0.204 contract records: **canonical clause text is the clause expression's token sequence** (§2.8.2), and the structural parens are not expression tokens, so *no record changes and no artifact-interface migration exists* — provided the migration preserves the expression token sequence: a v0.203 clause spelled with a fully parenthesized expression (`requires (E);`, whose expression tokens are `( E )`) must migrate to `requires((E))` to keep its record. Every clause in the current docs is unparenthesized, so in practice no record changes.

## 6. Migration

| Surface | Change | Size |
|---|---|---|
| Spec §22.16 | every example clause `requires E;` → `requires(E)`; the definitional prose spellings (`requires E;` / `ensures E;` / `invariant E;` at the section opening and the invariants subsection); the when-clause-identity subsection's **two** spelled-out desugarings; delete the closure prose | mechanical, ~20 snippets + prose |
| Appendix A.4 / B.2 / C.15 | the §2 productions; comment deletions; B.2 row wording unchanged in substance | small |
| §22.4 | its one spelled-out `static ensures …;` desugaring gains parens (§2.8.2 carries only the *phrase* "which are `static ensures` clauses" — no change; no §30.x site exists) | 1 site |
| Conformance corpus | **confirmed empty at this writing** — zero clause-bearing cases exist (grep verified; 4 prose false-positives); the row exists for the re-check at graduation time | none today |
| Parser (#2053 slice 1) | clause loop expects `(` Expression `)`; delete the closure lookahead | small; slices 2+ untouched |
| Parser tests | `contract_clauses_2053_test.cpp` re-spelled; the lenient-`;` test becomes the stray-`;` reject | mechanical |

**Coordination (the real constraint):** #2053 implementation slices are in flight on main. The syntax change should land as *one atomic step* — spec amendment + parser + test re-spell in the same landing window, before further clause-consuming slices — or immediately after the in-flight slice merges. With zero external users, the cost is a one-time mechanical migration; every week of delay adds fixture mass.

## 7. Costs, honestly

- Long clause expressions sit inside parens; multi-line clauses need the expression wrapped (ordinary continuation rules apply). The v0.203 spelling was marginally lighter for prose-like predicates.
- `requires(…)` is call-shaped. Mitigated by the marker-wins rule (it can never *be* a call in clause position) and by the existing keyword-paren family; C++26 and D chose the same shape for the same construct.
- Churn on a one-revision-old feature. This is also the argument for deciding *now*.

## 8. Alternatives declined

1. **Status quo + formatting rule** (each clause on its own line). Rescues the block and body-less forms but leaves the `; =>` seam and the closure machinery (#2070's class) in place; the owner review judged the seam a real cost.
2. **Bare unterminated clauses** (`requires E => x;`). Ambiguous — the `(x) => x` lambda collision and the expression/statement boundary (`requires f(x) (foo).Bar();` — `f(x)(foo)` is a valid call) force either maximal-munch heuristics or precedence carve-outs; declined as a grammar with unpredictable seams.
3. **Clauses as body statements.** Forfeits contracts on body-less members — the slot's contract written where the slot is declared, the feature's strongest case — or splits contracts across two homes; `ensures` is not sequentially executed and `old`/`result` have no statement semantics; the artifact interface carries declarations, not bodies. Declined in the 2026-07-21 review discussion; recorded here.
4. **Post-body clauses** (Eiffel's `ensure` placement). Splits the clause region by kind, reads preconditions after the code they govern, and still leaves `requires` before the body — the seam survives.
5. **Required trailing `;` after the clause list.** 0043 §3.2's original decline ("a double-semicolon with no reading") stands — this proposal removes the double-`;` question rather than re-answering it.

## 9. Open questions for the owner

- **Q1 — `invariant` parens.** Recommended as specified (`invariant(E);` — family-consistent, and the member `;` is untouched). The alternative (leave `invariant E;` unparenthesized) saves two characters and breaks the family; declined by recommendation.
- **Q2 — landing sequence.** Graduate spec-first with an impl tracker, or hold for an atomic spec+impl landing coordinated with the #2053 lane (recommended: atomic, per §6).

## 10. Critique record (2026-07-21)

An adversarial critique (designer→critic per `spec-change-process.md` §4) was run on the first draft. One blocking finding — the §3 collision analysis wrongly claimed a stray-`;` *parse* error for the method case (the actual behavior is a body-less-member *semantic* error, since the lone-`;` MethodBody alternative is reachable again by design) and contradicted §4's dispatch description — resolved by the per-member-kind analysis now in §3, including the honest worst-case comparison (v0.203's silent parse-as-clause of a `bool`-argument call vs 0055's loud rejection) and the migration note attaching the §22.16 remedy to the body-less diagnostic. Two substantive findings resolved: the v0.203 contrast now correctly describes a *successful* clause parse failing later as a type error (no `;` was missing), and the §6 migration table now targets the real desugaring sites (spec §22.4's one spelling and §22.16's when-identity subsection's two — the draft's §2.8.2/§30.x citations were a phrase-only site and a phantom) plus §22.16's definitional prose spellings. Two nits resolved: the record-identity claim is conditioned on expression-token preservation (`requires (E);` must migrate to `requires((E))`; every clause in the current docs is unparenthesized, so no record changes in practice), and the conformance-corpus row is recorded as confirmed-empty today. The critique confirmed the design core intact: the lambda ambiguity is structurally unreachable, the interface body slots compose and the #2070 double-`;` becomes unspellable, `GenericConstraints` cannot swallow the clause paren, the §22.4/§2.8.2 record identity is preserved for every existing clause, the guides need no revalidation, and the C++26/D precedent forms are described accurately.
