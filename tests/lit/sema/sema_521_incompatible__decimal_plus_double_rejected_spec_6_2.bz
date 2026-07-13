// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/decimal_diagnostics_521_test.cpp
//   (BEZEL_TEST sema_521_incompatible, decimal_plus_double_rejected_spec_6_2).
//
// §6.2: decimal + double → rejected.

module M; class C public static method M(a: decimal, b: double): int var _ := a + b; return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` has no implicit conversion between operand types `decimal` and `double`
// CHECK-SAME: (§6.2)
