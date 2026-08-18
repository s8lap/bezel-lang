# Proposal 0061 — Expression-form studies: the binding atom and `Result` propagation

| | |
|---|---|
| **Status** | graduated-v0.216 — §1 **superseded — graduated v0.216** (owner-activated 2026-07-22; spec §17.15, grammar A.7 `BindExpr`, kernel K8.5); §2 remains a recorded, evidence-gated study |
| **Tracking issue** | — (assign if §2 activates) |
| **Written against** | spec v0.214 (§1's pin re-verified against v0.215 at graduation; §17.15's landed text is the current rule) |

*The admission test both were judged by (and which future atom candidates should meet): a new expression atom earns its place only if it (1) produces a value whose computation is inexpressible or ceremony-heavy in expression position today, (2) rides existing kernel machinery — K7 meets, K8 boundaries, zero new ownership rules — and (3) does not make a doctrine-declined path more ergonomic. The two declines this review produced under test (3) and (1) are in `DECLINED.md` (try-expression; comprehensions).*

## 1. The binding atom — `(var x := E; body)` *(deferred by owner 2026-07-21: "probably not now, worth recording"; activated by owner direction and **graduated v0.216** — spec §17.15; the graduation-time decisions the sketch below left open are recorded in the v0.216 changelog entry)*

Names a subexpression within expression position: `(` + `var` is a unique two-token signal; `;` separates (no expression contains a top-level `;`, so the extent is trivially unambiguous — `in` collides with membership, `=>` with lambdas); bindings chain; the body is one expression. It is *binding*, not assignment-in-expression: single-assignment by construction, no mutation, unspellable by accident. Ownership rides K8 verbatim — an owned initializer is a named unbound temporary, disposed at the atom's boundary. It does **not** reopen the value-returning-`do` decline (that decline is about statement *sequencing*; this admits no statements).

**The motivating case the owner flagged as interesting:** contract clauses (§22.16) are expression-only and pure, so a clause using a subresult twice must recompute it — `ensures(var n := result.Count; n >= 0 and n <= _capacity)` names it once, harmlessly. Expression-position `case`/`when` arm bodies are the second beneficiary (today's remedy is helper extraction, which stands as doctrine meanwhile).

**Activation evidence to look for:** clause-heavy code (post-#2053) recomputing subexpressions; arm bodies driving helper-extraction churn in review.

## 2. `Result<T, E>` propagation *(evidence-gated)*

Bezel cannot propagate an `Err` from expression position (`return` is a statement; every call site consuming a `Result` pays a statement-position `case`). The Rust-`?` capability is genuinely unique under test (1) — but it is control flow hidden in an expression, against the announced-flow personality, and `Result` (v0.200) has no adoption evidence yet. Gate: revisit when real `Result`-using code exists and the per-call ceremony is observed, not predicted. Any design must also answer the ownership question (an early return is a K3.2 consume of the propagated payload) and should be weighed against the deferred ownership-polymorphic function parameters (stdlib §2.20's recorded blocker for `Result` combinators — `map`/`andThen` may relieve the same pain without new control flow).

## 3. Evidence log (for §2's gate)

*2026-07-21 (writer-seat sweep, retry-library program — the first real `Result`-dense Bezel written):* observed exactly the gated ceremony — statement-position `case` per propagating call, and a second, related friction: full generic arguments restated on every `Result` construction (`Result<T, RetryError>.Ok(v)` nine times in two combinators) where the declared return type fixes both parameters. **Return-position target-typing of case constructions** is hereby added to this study as a sibling candidate: it is *not* the argument-inference §14.5/§35.3 forbid, but the counter-argument is strong (the uniform never-infer posture exists to prevent some-sites-infer inconsistency) — weigh both together when the gate opens. Also noted: stdlib §2.20's `Result` combinators remain blocked on the deferred ownership-polymorphic function parameters; relieving that deferral may relieve most of the propagation pain without new control flow. One discoverability fix applied at this logging: rationale-§18-style deferral scans miss this studies file; cross-referenced from 0062's deferred register.

