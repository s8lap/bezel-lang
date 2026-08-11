// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_arith, double_plus_decimal_6_2_still_rejected).
//
// §6.2: double + decimal (no implicit conv either way per §5.1) → §6.2
// error still applies; this pre-existed in mixed_numeric_test — confirm
// the incompatibility check is preserved even after decimal is implemented.

module M; class C public static method M(a: double, b: decimal): int _ := a + b; return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` has no implicit conversion between operand types `double` and `decimal`
// CHECK-SAME: (§6.2)
