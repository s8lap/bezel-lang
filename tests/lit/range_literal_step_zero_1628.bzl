// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail — see
// tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Pure-SEMA diagnostic reject, ported from
//   tests/sema/range_literal_step_zero_1628_test.cpp
//   (BEZEL_TEST for_loop_literal_step_zero_rejected_spec_16_4).
//
// Spec §16.4 (v0.188): a LITERAL range step of zero is a compile error; a
// COMPUTED (runtime) step of zero is admitted at construction and raises
// `ArgumentException` at `GetEnumerator()`.  Here `0..10 step 0` is a literal
// step-0 and must be rejected where written.
//
// FileCheck matches the diagnostic's CONTENT — the rule wording and the §16.4
// citation — not merely "compilation failed".  The escape guard below keeps
// the reject a clean, user-facing §16.4 diagnostic and not an
// internal-compiler-error escape. (#6620: that guard used to name the
// codegen-only `internal:` marker — codegen is gated on a clean front end, so
// on a row that rejects in the front end it named a needle that could never
// fire; it now names the front end's own ICE marker.)
module M;
class C
  public static method Main(): int
    var sum: int := 0;
    for var i in 0..10 step 0 do sum := sum + i; end;
    return sum;
  end Main;
end C;

// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// #7069: the front end's ICE marker is printed BEFORE any user-facing
// diagnostic, and a negative directive scans only the region between the
// surrounding matches — below the positive match this needle could never
// fire, so it is written above it, where it can.
// CHECK-NOT: internal compiler error
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a range step of zero is a compile error (§16.4)
// CHECK-SAME: compile-time constant
