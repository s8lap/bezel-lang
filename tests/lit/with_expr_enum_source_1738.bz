// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter: the compile is EXPECTED to fail — see
// tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Issue #1738: the with-expression source-type gate (§9.5) did not reject an
// ENUM-typed source. §9.5 states plainly: "The source expression must be
// record-typed." The sema gate in ExprTypecheck_Exprs.cpp (WithExpr handling)
// enforced this for class-/interface-typed sources (#1735) but carved out
// Enum, so `someEnumValue with X := 1 end` fell through sema uncaught and
// reached codegen, which — since #1735's lowerWith hardening — hit an
// internal-error diagnostic ("with on a non-record source reached codegen
// unexpectedly") instead of a proper user-facing §9.5 sema reject. Per §10.5,
// an enum may not declare instance fields or backing-storage properties (its
// runtime representation is the underlying integer), so an enum source has no
// possible valid `with` target — this test locks in the corrected sema-level
// rejection with a diagnostic naming both §9.5 and §10.5.
module M;
enum Color
  Red, Green, Blue;
end Color;
class C
  public static method Main(): int
    var c := Color.Red;
    var d := c with X := 1 end;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `with` requires a record-typed source; this source is enum-typed (§9.5)
    // CHECK-SAME: (§10.5)
    return 0;
  end Main;
end C;
