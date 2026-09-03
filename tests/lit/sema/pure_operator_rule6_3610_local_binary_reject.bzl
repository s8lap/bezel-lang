// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Diagnostic-track pin for the operator-invocation arm of §22.15 rule 6
// (v0.286), ported from tests/module/pure_operator_rule6_3610_test.cpp's
// `local_impure_binary_operator_rejected`. Rule 6: "A call — method,
// **operator**, accessor read, or constructor in the admitted positions —
// is permitted iff the member ... is `pure`-declared or pure-classified."
//
// `Vec::operator -` is not `pure`-declared; `P` is `pure` and calls it via
// `a - b`. Pre-`narrow #3610` (37f59d5) this compiled with exit 0 and the
// executable exited 3 — the impure operator body ran inside the pure body.
// Verified REFUTED-then-reconfirmed on the current tree by this lane
// (c7-b13, R12 cycle 4): the reject already fires; this file pins it against
// regression at the diagnostic-track layer, which had no #3610 coverage.
//
// CHECK-NOT: internal:

module M;
public class Vec
  public X: int;
  public constructor(x: int)
    X := x;
  end constructor;
  public static method operator -(a: Vec, b: Vec): int => a.X - b.X;
end Vec;
public class Program
  public static pure method P(a: Vec, b: Vec): int
    return a - b;
  end P;
end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: call to `operator -` is not admitted in a pure body
// CHECK-SAME: (§22.15)
