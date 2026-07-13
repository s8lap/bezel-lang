// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/mixed_numeric_test.cpp
//   (BEZEL_TEST sema_mixed_numeric, byte_plus_sbyte_is_error_spec_6_2).

module M; class C public static method M(a: byte, b: sbyte): int var r := a + b; return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` has no implicit conversion between operand types `byte` and `sbyte`
// CHECK-SAME: (§6.2)
