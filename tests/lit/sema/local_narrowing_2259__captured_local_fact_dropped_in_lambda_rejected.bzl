// §7.4 / §27.5 (v0.236, #2259): a flow-narrowing fact established on a LOCAL
// before a lambda does NOT ride into the lambda body. §27.5 is explicit that
// capture is by REFERENCE — "Modifications to a captured variable inside the
// lambda are visible outside, and vice versa" — so the enclosing scope may
// rebind a captured local's cell AFTER the closure's creation but BEFORE its
// invocation, invalidating a fact the body type-checked against. The closure
// then reads null → SIGSEGV (probe-confirmed on origin/main: "SEGV on unknown
// address 0x000000000008 … in lam$0", the `.Length` load off a null receiver).
//
// This cell REPLACES the former local_narrowing_2113__prior_fact_rides_in_
// accepted.bzl, which pinned the UNSOUND accept (the #2113 fix argued the
// local sets nullNarrowed_/narrowings_ "need NO in-side drop"; #2259 corrects
// that — the local sets are cleared on the way IN, matching what 817a498
// already did for fieldNarrowed_'s local-rooted facts and for the identical
// reason). The companion OUT-side reject
// (local_narrowing_2113__lambda_body_fact_does_not_leak_rejected.bzl) is
// unchanged. Re-narrow inside the body to recover a fact:
//   var f := lambda() => do if s <> null then return s.Length; end; … end;
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

class C
    // #2259 repro: `s` narrowed non-null before the lambda; the lambda captures
    // `s` (by reference, §27.5) and reads `s.Length` inside the body. The
    // pre-capture fact must NOT ride in — the body sees `s: string?`.
    public static method Cap(s: string?): int
        if s = null then return 0; end;
        var f := lambda() => do
            var n := s.Length;  // REJECTED — `s` is not proven non-null here
        end;
        return 0;
    end Cap;
end C;

// CHECK: member access `.Length` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
