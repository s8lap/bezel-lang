// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, computed_property_target_rejected).
//
// §9.5, #878: a computed (expression-bodied) property is not a valid `with`
// target because it has no backing storage.

module M; record ColorVal(R: int, G: int, B: int) public property Luminance: int => (R * 3 + G * 6 + B) / 10; end ColorVal; class C public static method Main(): int var c := new ColorVal(10, 20, 30); var d := c with(Luminance := 99); return d.R; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `Luminance` is a computed property and cannot be a `with` target — `with` requires backing storage (auto-property)
// CHECK-SAME: (§9.5, #878)
