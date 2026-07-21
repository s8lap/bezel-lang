# Proposal 0061 — Expression-form studies: the binding atom and `Result` propagation (recorded, not proposed)

| | |
|---|---|
| **Status** | recorded — two deferred directions from the 2026-07-21 expression-forms review; neither is proposed for adoption at this writing |
| **Tracking issue** | — (assign if either activates) |
| **Written against** | spec v0.214 |

*The admission test both were judged by (and which future atom candidates should meet): a new expression atom earns its place only if it (1) produces a value whose computation is inexpressible or ceremony-heavy in expression position today, (2) rides existing kernel machinery — K7 meets, K8 boundaries, zero new ownership rules — and (3) does not make a doctrine-declined path more ergonomic. The two declines this review produced under test (3) and (1) are in `DECLINED.md` (try-expression; comprehensions).*

## 1. The binding atom — `(var x := E; body)` *(deferred by owner, 2026-07-21: "probably not now, worth recording")*

Names a subexpression within expression position: `(` + `var` is a unique two-token signal; `;` separates (no expression contains a top-level `;`, so the extent is trivially unambiguous — `in` collides with membership, `=>` with lambdas); bindings chain; the body is one expression. It is *binding*, not assignment-in-expression: single-assignment by construction, no mutation, unspellable by accident. Ownership rides K8 verbatim — an owned initializer is a named unbound temporary, disposed at the atom's boundary. It does **not** reopen the value-returning-`do` decline (that decline is about statement *sequencing*; this admits no statements).

**The motivating case the owner flagged as interesting:** contract clauses (§22.16) are expression-only and pure, so a clause using a subresult twice must recompute it — `ensures(var n := result.Count; n >= 0 and n <= _capacity)` names it once, harmlessly. Expression-position `case`/`when` arm bodies are the second beneficiary (today's remedy is helper extraction, which stands as doctrine meanwhile).

**Activation evidence to look for:** clause-heavy code (post-#2053) recomputing subexpressions; arm bodies driving helper-extraction churn in review.

## 2. `Result<T, E>` propagation *(evidence-gated)*

Bezel cannot propagate an `Err` from expression position (`return` is a statement; every call site consuming a `Result` pays a statement-position `case`). The Rust-`?` capability is genuinely unique under test (1) — but it is control flow hidden in an expression, against the announced-flow personality, and `Result` (v0.200) has no adoption evidence yet. Gate: revisit when real `Result`-using code exists and the per-call ceremony is observed, not predicted. Any design must also answer the ownership question (an early return is a K3.2 consume of the propagated payload) and should be weighed against the deferred ownership-polymorphic function parameters (stdlib §2.20's recorded blocker for `Result` combinators — `map`/`andThen` may relieve the same pain without new control flow).
