// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/mixed_numeric_test.cpp
//   (BEZEL_TEST sema_mixed_numeric, int_bitand_uint_is_error_spec_6_2).
//
// The bitwise operators flow through the same §6.2 promotion path; an
// incompatible signed/unsigned pair is rejected there too.

module M; class C public static method M(a: int, b: uint): int var r := a & b; return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `&` has no implicit conversion between operand types `int` and `uint`
// CHECK-SAME: (§6.2)
