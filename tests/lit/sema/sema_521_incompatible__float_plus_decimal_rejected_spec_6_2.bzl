// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_incompatible, float_plus_decimal_rejected_spec_6_2).
//
// §6.2: float + decimal → rejected (float and decimal don't implicitly convert).

module M; class C public static method M(a: float, b: decimal): int _ := a + b; return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` has no implicit conversion between operand types `float` and `decimal`
// CHECK-SAME: (§6.2)
