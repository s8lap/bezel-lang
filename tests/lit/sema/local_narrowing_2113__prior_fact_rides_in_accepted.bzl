// §7.4 (v0.226, #2113): the OUT-side leak fix (companion reject cell:
// local_narrowing_2113__lambda_body_fact_does_not_leak_rejected.bzl) must not
// over-reach the IN boundary. Spec §7.4 establishes that a narrowing fact
// established BEFORE the lambda holds inside the body (capture): the closure
// sees the ambient fact state, so a nullable narrowed non-null before the
// lambda is usable inside the body. The fix restores the saved sets AFTER the
// body visit only — it does not clear them on the way IN — so the capture
// direction is preserved.
//
// This ACCEPT cell documents that boundary direction: `s` narrowed before the
// lambda is usable inside the body both before and after the fix.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

class C
    // `s` is narrowed non-null in the enclosing scope before the lambda; the
    // body captures that fact and `s.Length` is admissible inside it.
    public static method Cap(s: string?): int
        if s = null then return 0; end;
        var f := lambda() => do
            var n := s.Length;  // OK — capture: `s` narrowed before the lambda
        end;
        return 0;
    end Cap;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
