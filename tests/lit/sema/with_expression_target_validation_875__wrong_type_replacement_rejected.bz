// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, wrong_type_replacement_rejected).
//
// §9.5/§5.1, #875: a wrong-type replacement value is a compile error.
// `p with X := "hi" end` where X: int must be rejected.

module M; record Point(X: int, Y: int); class C public static method Main(): int var p := new Point(1, 2); var q := p with X := "hi" end; return q.Y; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the replacement value for `X` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
