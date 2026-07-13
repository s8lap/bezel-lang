// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/mixed_numeric_test.cpp
//   (BEZEL_TEST sema_mixed_numeric, error_cites_both_types_spec_6_2).
//
// The diagnostic cites both operand type names (so the programmer knows
// which cast to add).

module M; class C public static method M(a: double, b: decimal): int var r := a + b; return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` has no implicit conversion between operand types `double` and `decimal`
// CHECK-SAME: (§6.2)
