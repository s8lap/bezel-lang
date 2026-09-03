// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Diagnostic-track pin for the UNARY half of §22.15 rule 6's operator arm
// (v0.286), ported from tests/module/pure_operator_rule6_3610_test.cpp's
// `local_impure_unary_operator_rejected`. `R::operator ~` is not
// `pure`-declared; `P` is `pure` and calls it via `~a`. `ExprTC::typeUnary`
// records the resolved overload on the UnaryExpr node exactly as the binary
// path does (setExprSymbol), so `checkOperatorOverloadPurity` reaches it via
// the same `callTargetIsPure` authority — no separate predicate.
//
// CHECK-NOT: internal:

module M;
public record R(V: int)
  public static method operator ~(a: R): int => a.V + 1;
end R;
public class Program
  public static pure method P(a: R): int
    return ~a;
  end P;
end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: call to `operator ~` is not admitted in a pure body
// CHECK-SAME: (§22.15)
