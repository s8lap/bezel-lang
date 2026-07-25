// §7.4 (v0.226, #2113): a flow-narrowing fact established INSIDE a lambda
// body must NOT leak to the enclosing scope — the body is deferred code that
// may run never or later, so the enclosing statement cannot inherit its
// facts. Spec §7.4 ("How narrowing is invalidated") frames a lambda body as
// its own callable boundary; the same boundary that (#2085/#2116) already
// restored `fieldNarrowed_` must also restore the two local-rooted sets
// (`nullNarrowed_` for null tests, `narrowings_` for §12.7 type tests).
//
// Root cause: the lambda visitor (ExprTypecheck_Exprs.cpp) saved/restored
// `fieldNarrowed_` across the body visit but left `nullNarrowed_` and
// `narrowings_` untouched, so a narrowing established at the lambda body's
// TOP level survived into the enclosing scope's fact set. The repro form is
// the EARLY-RETURN guard: `if s = null then return; end;` leaves `s` in
// `nullNarrowed_` at the body's top level (the after-if intersection is the
// single fall-through path, on which `s <> null`), and that fact then leaked
// out. (A plain `if s <> null then … end` does NOT leak — its then-branch
// fall-through narrows `s` but the implicit else falls through nullable, so
// the after-if intersection drops `s`; the early-return form is the minimal
// repro because it eliminates the nullable fall-through path.)
//
// This is the REJECT cell: with the fix the trailing unguarded `s.Length` is
// correctly rejected (the enclosing scope sees `s` as nullable). The
// companion ACCEPT cell (prior fact rides IN) is
// local_narrowing_2113__prior_fact_rides_in_accepted.bzl.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

class C
    // The #2113 repro: the lambda body's early-return guard narrows `s` at
    // the body's top level; without the fix that fact leaks, so the trailing
    // `s.Length` is wrongly accepted. With the fix it is rejected.
    public static method Leak(s: string?): int
        var f := lambda() => do
            if s = null then return; end;
            var n := s.Length;  // OK — narrowed INSIDE the lambda body
        end;
        return s.Length;  // REJECTED — enclosing scope sees `s: string?`
    end Leak;
end C;

// CHECK: member access `.Length` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
