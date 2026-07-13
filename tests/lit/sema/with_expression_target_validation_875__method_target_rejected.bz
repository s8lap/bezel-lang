// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, method_target_rejected).
//
// §9.5, #877: a method name as a `with` target is a compile error.
// `p with Double := 99 end` where Double is a method must be rejected.

module M; record Point(X: int, Y: int) public method Double(): int => X * 2 + Y; end Point; class C public static method Main(): int var p := new Point(3, 4); var q := p with Double := 99 end; return q.X; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `Double` is a method, not a valid `with` target — `with` targets must be positional-field properties, auto-properties, or public fields
// CHECK-SAME: (§9.5, #877)
