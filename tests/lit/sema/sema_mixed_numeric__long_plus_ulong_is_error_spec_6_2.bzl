// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/mixed_numeric_test.cpp
//   (BEZEL_TEST sema_mixed_numeric, long_plus_ulong_is_error_spec_6_2).

module M; class C public static method M(a: long, b: ulong): int var r := a - b; return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `-` has no implicit conversion between operand types `long` and `ulong`
// CHECK-SAME: (§6.2)
