// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter: the compile is EXPECTED to fail, and
// wrapping it in `not` lets its diagnostics flow through lit's pipe to
// FileCheck — see tests/lit/lit.cfg.py and README.md for the full rationale.)
//
// Pure-SEMA diagnostic reject, ported from
//   tests/sema/local_const_eval_fault_1688_test.cpp
//   (BEZEL_TEST div_by_zero_rejected).
//
// Spec §17.14 (v0.184) "Evaluation": an evaluation FAULT — integer/decimal
// overflow, or division/mod by zero — in a constant expression is a COMPILE
// error at the constant expression, never a deferred runtime fault.  The
// spec's own example is `const D := 5 / 0;`.
//
// This test also demonstrates FileCheck's LOCATION-PRECISE matching: the
// `[[@LINE-1]]` numeric variable pins the diagnostic to the exact source line
// of the offending `const`, and `{{[0-9]+}}` accepts any column.  The coarse
// conformance manifest (an expectation column of compiles/fails/program) cannot
// express "the error must point HERE, at this line and a column".
module M;
class C
  public static method Main(): int
    const D := 5 / 0;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: const `D` initializer contains a division or modulo by zero
    // CHECK-SAME: (§17.14)
    return 0;
  end Main;
end C;
