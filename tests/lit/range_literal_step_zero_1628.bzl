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
// citation — not merely "compilation failed".  The `CHECK-NOT: internal
// compiler error` guards that the reject is a clean, user-facing §16.4
// diagnostic and not an internal-compiler-error escape. (#6620: that needle
// used to be `internal:`, which is a CODEGEN-only marker — codegen is gated
// on a clean front end, so on a row that rejects in the front end it named a
// needle that could never fire.)
module M;
class C
  public static method Main(): int
    var sum: int := 0;
    for var i in 0..10 step 0 do sum := sum + i; end;
    return sum;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a range step of zero is a compile error (§16.4)
// CHECK-SAME: compile-time constant
// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// CHECK-NOT: internal compiler error
