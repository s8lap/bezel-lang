// §7.4 / §22.16 / #2212: in `A and B`, `B` is checked under `A`'s true-facts.
// Spec §7.4 (v0.226): "in `A and B`, `B` (and the true branch) is checked under
// `A`'s true-facts … so `x <> null and x.Length > 0` is well-typed." Spec §22.16
// (v0.226): "Flow narrowing works inside clauses unchanged
// (`requires(x <> null and x.Count > 0)` narrows per §7.4)."
//
// Root cause: narrowing was established only by collectNarrowings, which the
// if/elif/while visitors run AFTER the condition is fully visited — never
// inside visitExpr. So the right operand of `and` was visited with NO narrowing
// from the left, and a nullable parameter's `.Length` in the right operand was
// wrongly rejected. The fix in ExprTypecheck_Exprs.cpp visitChildren, between
// visiting the left and right of `and`, seeds the three narrowing sets from
// the left operand (mirroring checkOutPostcondAtReturn's save/restore).
//
// Three accepts:
//  (a) the #2212 repro — `requires(s <> null and s.Length >= 0)` on `string?`;
//  (b) the `ensures` variant — same shape on a postcondition clause;
//  (c) the latent `if (s <> null and s.Length > 0) then …` body case (same root
//      cause, spec-correct per §7.4 — blessed, not scope creep).
// The companion reject cell is param_narrowing_2188__contract_clause_body_fact_
// rejected.bzl (unguarded `requires(s.Length >= 0)` on `string?`); the clause-
// boundary clear there (ExprTypecheck.cpp:7358-7360) is untouched by this fix.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

class C
    // (a) §22.16 / §7.4: requires clause, `and` flows left's null-narrowing
    // into the right operand so `.Length` is admissible on a `string?` param.
    public static method Req(s: string?): int
        requires(s <> null and s.Length >= 0)
        return 0;
    end Req;

    // (b) §22.16 / §7.4: ensures variant — same `and` composition in a
    // postcondition clause (checked at each return edge, body-final facts
    // cleared per the #2188 boundary, so the clause must narrow on its own).
    public static method Ens(s: string?): int
        ensures(s <> null and s.Length >= 0)
        return 0;
    end Ens;

    // (c) §7.4: the latent if-condition case — `s <> null and s.Length > 0`.
    // The right operand narrows under the left; the then-branch additionally
    // inherits the whole condition's true-branch narrowing.
    public static method IfCond(s: string?): int
        if s <> null and s.Length > 0 then return s.Length; end;
        return 0;
    end IfCond;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
