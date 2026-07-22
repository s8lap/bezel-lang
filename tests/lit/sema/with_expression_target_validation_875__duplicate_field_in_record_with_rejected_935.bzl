// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, duplicate_field_in_record_with_rejected_935).
//
// REJECT: duplicate field name in a record `with`.

module M; record Point(X: int, Y: int); class C public static method Main(): int var p := new Point(1, 2); var p2 := p with(X := 5, X := 10); return p2.X; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: field `X` is assigned more than once in this `with` expression
// CHECK-SAME: (§9.5)
