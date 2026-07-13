// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/compound_assign_operator_result_851_test.cpp
//   (BEZEL_TEST compound_assign_operator_result_851, plus_returns_other_record_rejected).
//
// §25.1: an `operator +` returning a DIFFERENT, unrelated record is rejected.

module M; record Other(Z: int); record Point(X: int, Y: int) public static method operator +(a: Point, b: Point): Other => new Other(a.X); end Point; class C public static method Main(): int var p := new Point(3, 4); var q := new Point(5, 6); p += q; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` desugars to `lhs := lhs + rhs`, but `operator +` returns a value that is not assignable to the left-hand side's type — the operator's result type differs from the target
// CHECK-SAME: (§25.1)
