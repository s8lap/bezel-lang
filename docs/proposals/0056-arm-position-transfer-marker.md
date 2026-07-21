# Proposal 0056 — The arm-position transfer marker: `owned x` in `if`- and `case`-expression arms

| | |
|---|---|
| **Status** | superseded — graduated v0.209 (as narrowed; `when`-arm results admitted for parity at the composition gate — mutually-exclusive expression arms, same K7 meet). Impl delta tracked at the landing issue |
| **Tracking issue** | [#2076](https://github.com/s8lap/bezel-lang/issues/2076) |
| **Target** | Spec: §28.3 (K1.5 producers table — one new row; K3.4's marked-transfer family — two new positions; axiom register — A7 *narrowed*, not retired; the "equalize the branches" worked examples gain the expression form), §17 (the if-expression section), §35 (expression-position `case` arm results), Appendix A.6 (the marked-identifier atom + the unification with the v0.180 binding-slot marker); rationale §5.26 (A7 bullet updated). No stdlib changes. |
| **Written against** | spec v0.206 |
| **Origin** | The 2026-07-21 ownership-kernel review, candidate C7. Axiom A7 is the register's recorded-liftable bound; this proposal is the syntax design it called for — narrowed after critique to the positions that lift *without new kernel rules*. |
| **Depends on** | Nothing. Extends the v0.180 marked-transfer family (K3.4: `var y := owned x;`, `y := owned x;`, `using var y := owned x;`; v0.200 added `case owned x of`). |

*Reference convention: unprefixed §-references are language-spec references; K/A tags are spec §28.3's kernel ids.*

---

## 1. Motivation

Under A7, a named owned binding written as a conditional-expression arm is an **unmarked read — a borrow arm** (K3.13's no-silent-consume, kept uniform by *inexpressibility*: no marker spelling exists in those positions). The consequence is the **equalization ceremony**: selecting one of two named owned locals as one owned result cannot be written as an expression. The spec's own worked Pattern 2 (§28.3, *Ownership state of locals*) spends a declared-then-assigned statement `if` on what the author means as one line, and Guide 01 hedges "a named owned arm is a borrow, never a silent consume" — a subtlety that exists only because the marker is missing there.

```bezel
// Today (spec §28.3 Pattern 2 — the ceremony):
var x: Stream;
if useFresh then
  x := new Stream();
else
  x := owned cached;         // marked transfer — statement position has a marker
end;

// This proposal:
var x := new Stream() if useFresh else owned cached;
```

The asymmetry being repaired is real: the marker `owned x` already exists in *binding and assignment* RHS position, in *argument* position, and (v0.200) in *scrutinee* position. This proposal adds the two value-producing positions where the lift is a **pure derivation** — the mutually-exclusive arms of `if`- and `case`-expressions. It deliberately does **not** lift `??` operands (§8: a `??` evaluates its left operand unconditionally, so a marked *nullable* left operand poses a binding-post-state question — what is `a` after `owned a` evaluated while `a` held `null`? — that no existing kernel rule answers; the coalesce meaning is fully expressible with the `if` form and flow narrowing, at zero new rules).

## 2. The design

### 2.1 Grammar: one marker form, unified

A.6 gains a **marked-identifier atom**:

```
MarkedTransfer ::= "owned" Identifier
```

parsed as a primary-level atom (it binds tighter than every operator: `owned a if c else b` is `(owned a) if c else b`). The atom is *syntactically* admitted wherever a primary expression is; **position admission is enforced semantically**, and the admitted positions are exactly:

1. the entire right-hand side of a binding or assignment (`var y := owned x;`, `y := owned x;`, `using var y := owned x;`) — this **is** the v0.180 marker, re-derived: A.6's binding-slot alternative (`":=" "owned"? Expression`) and the assignment-slot twin are deleted, replaced by the RHS-root instance of the atom, with identical meaning for every existing program;
2. either arm of an **if-expression** (`A if c else B`);
3. the result expression of an **expression-position `case`/`when` arm**.

Everywhere else — `??` operands, operator operands, argument subexpressions (`f(owned x + 1)`), guards — the atom is a compile error naming this rule and, where applicable, the deferral (§8). The call-site argument marker (`M(owned x)`) keeps its own ArgList production, unchanged. The identifier must name a **local binding or `owned` parameter** — the same source class K3.4's binding-form marker accepts today; field paths are not admitted (v1; §8).

Because `owned` is a reserved keyword that begins no expression today, the atom is additive: every program that parses today parses identically, and the F2-class collision between a binding-slot marker and an arm marker cannot arise — there is only one marker, and `var x := owned cached if c else new Stream();` has exactly one reading: an if-expression whose true arm is the marked atom. (The degenerate whole-RHS case `var y := owned x;` is the same atom at RHS-root, which is the v0.180 semantics verbatim.)

### 2.2 Semantics: consume-if-selected (derivation only)

- A marked arm is an **owned producer** for the construct's K7 meet (a new row in K1.5's producers table: "marked arm transfer — owned; derives K3.4"). It meets cleanly with fresh producers and other marked arms: the arms of an `if`- or `case`-expression are **mutually exclusive** — exactly one is evaluated — so exactly one obligation exists (K7.1's reasoning, applying to precisely the constructs where its premise holds).
- On the path where the marked arm is **evaluated**, the named binding is consumed — left moved-from, exactly as `var y := owned x;` leaves `x`. On paths through other arms the binding is untouched. The post-construct join is K7.4's existing **maybe-consumed** machinery — unreadable after the construct, scope-end disposal per-path. No conditional-consume rule is invented: arm selection is ordinary control flow, and K7.4 was built for exactly this shape (it is the same join a statement-position `if c then Sink(owned x); end;` produces today).
- **The freeze applies unchanged** (K5.1: a marked transfer under a live local borrow of the binding is a compile error). Two worked consequences: *(a)* condition-side reads do **not** freeze the arms — in `owned candidate if candidate.Score() > incumbent.Score() else owned incumbent`, the condition's borrows complete before any arm runs, and K5.5's liveness is backward dataflow to a *later* use, so the example is legal. *(b)* `case a of … => owned a … end` — an arm result marking the **scrutinee's own binding** — is **always frozen**: the scrutinee borrow is live through the selected arm's completion (K8.3), so the consume is rejected; move the transfer after the construct, or restructure with a statement `case`.
- Marked arms and **loops**: a marked arm inside a loop body leaves the binding maybe-consumed at the join, and K7.4 bars maybe-consumed from flowing around a back edge into a use — so a *loop-carried* marked read of the same binding is rejected unless the body re-establishes the binding to owned on every path before the back edge (typically by reassignment, which is the existing accumulator discipline). The feature is a **one-shot select**, not a loop accumulator; the statement forms remain the loop idiom.
- **Owner-side sinks compose**: `using var r := owned a if c else new Stream();` (owned meet → owned binding), `return owned a if c else owned b;` under an `owned` return, an `owned` argument `M(owned a if c else owned b)` — each is the existing sink rule over an owned expression.
- **Disposal of the losing arm**: unlike a fresh producer (never created when unselected), a *named* unselected arm's binding still holds its value and is auto-dropped at its scope end per its path (K3.14) — the coalesce's loser is disposed, not leaked and not silently kept. Programs wanting the loser retained simply don't mark it.

**Kernel classification (the v0.198 obligation).** A **derivation**: K3.4's marked-transfer family gains two positions; the join is K7.1/K7.4 verbatim; the freeze is K5.1/K8.3 verbatim. **Axiom A7 is narrowed, not retired**: its live content shrinks to the still-deferred positions — `??` operands and operator operands — and the register row is updated to record the narrowing and this proposal as the lifting revision for arms. The register stays at thirteen live axioms; the win is a smaller axiom, honestly counted.

## 3. What changes for programs

```bezel
// 1. The coalesce, via narrowing — zero new rules (fast: Connection?):
public method Best(owned fast: Connection?, owned slow: Connection): owned Connection
  return owned fast if fast <> null else owned slow;
end Best;
// true path: fast narrowed non-null (§7.4), transferred; slow untouched there.
// After the construct both are maybe-consumed; the loser auto-drops per its
// path at scope end (K7.4 + K3.14).

// 2. The one-shot select (A5: `using var` is non-reassignable — this is a
//    selection, not a loop accumulator; loops keep the statement form):
using var best := owned candidate if candidate.Score() > incumbent.Score() else owned incumbent;

// 3. Mixed arms stay governed by the existing meet:
var s := owned cached if c else new Stream();     // owned ⊓ owned — fine
var t := owned cached if c else borrowedView;     // owned ⊓ borrowed — K7.3 rejection, unchanged
```

Diagnostics improve at the rejection sites too: today two unmarked owned arms silently merge as a **borrow** (both arms unmarked reads), surfacing later as a lifetime error; with the marker in the grammar, that later diagnostic can name the remedy — "did you mean `owned a if … else owned b`?"

## 4. What this deletes

- The §28.3 equalization Pattern 2 ceremony as the *only* spelling (the worked example gains the one-line form; the statement form remains legal and remains the loop idiom).
- The guide's hedge collapses to one rule with a spelling: *unmarked reads never consume; write `owned x` where you mean transfer* — now true in every mutually-exclusive value-producing position.
- Most of axiom A7's content (the arm half); the register row shrinks to the deferred operand positions.

## 5. Costs, honestly

- **One more marker position to teach** — the same marker with the same meaning, but two genuinely new notes ride it: the always-frozen scrutinee-arm case (§2.2), and the losing-arm disposal note (a named loser is dropped at scope end; a fresh loser was never created). The net teaching delta is judged favorable, not "free."
- **Maybe-consumed becomes more common** in everyday code. Its rules are unchanged; the review's C2 analysis (decline dropping the state) is re-confirmed as a dependency here.
- **Parser/sema surface**: the atom, the RHS-root re-derivation of the v0.180 marker, two admitted positions, freeze hookup, the join — all existing machinery; sized S/M.

## 6. Alternatives declined

1. **Status quo** — the ceremony stands and the register carries a liftable bound it was built to lift.
2. **Implicit consume in arms** — violates K3.13's no-silent-consume. Non-starter.
3. **Whole-expression marker** (`owned (a if c else b)`) — marks an expression rather than a binding; *what* is consumed is no longer named at the consume site; composes wrongly with mixed arms.
4. **Lifting `??` operands in v1** — declined after critique (§8): requires a new null-evaluated-transfer rule and collided with the binding-slot marker before the §2.1 unification; deferred, not rejected — the door is the recorded §8 question.
5. **Field paths in v1** (`owned x.f` arms) — drags K9 restoration analysis into per-arm paths; deferred with the same recorded posture.

## 7. Open questions for the owner

- **Q1 — the deferred positions.** `??` operands need one new rule: the post-state of a marked *nullable* binding whose value was `null` at evaluation (the natural answer — the read of `null` transfers nothing and leaves the binding in its owned-currently-null state — is coherent but is genuinely *new*, so it must land as a recorded ruling, not smuggled as a derivation). Operator operands remain unmotivated. Both stay in A7's narrowed row. Lift later, on evidence?
- **Q2 — `case` arm results only, or also statement-position arms?** Statement-position arms are statements — the existing K3.4 markers already work there. Recommended: expression-position only.
- **Q3 — diagnostic investment.** Ship the "did you mean `owned … if … else owned …`?" remedy note with the feature (recommended).

## 8. Critique record (2026-07-21)

An adversarial critique (designer→critic per `spec-change-process.md` §4) found two blocking defects in this proposal's first draft, both confined to its `??`-operand half: **(F1)** the null-LHS semantics required a new kernel rule (a marked transfer evaluated over a runtime-`null` binding) that the draft mis-derived from K7.5 — a syntactic null-*literal* rule — while simultaneously claiming K3.4-verbatim unconditional consumption; **(F2)** the flagship `var r := owned a ?? owned b;` was lexically ambiguous against the v0.180 binding-slot marker (`":=" "owned"? Expression`), leaving the central example unparseable as specified. Resolution: v1 is **narrowed to `if`/`case` arms** — where mutual exclusivity makes consume-if-selected a pure K7.4 derivation and where the §2.1 atom unification removes the marker collision for every admitted position — and the `??` half is deferred into A7's narrowed row with its open rule named (Q1). Substantive findings also resolved: A7 is narrowed rather than tombstoned (the operand half stays live; the register count claim was corrected), the "accumulator" caption was corrected to one-shot select with the back-edge analysis added, the scrutinee-arm freeze and losing-arm disposal notes were added, and the grammar was specified as productions rather than prose. The critique confirmed: condition-side reads do not freeze the arms; the same-binding-both-arms case is sound (moved ⊓ moved); the guard exclusion matches §35.5; and the atom is additive over today's grammar.
