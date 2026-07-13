// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lifted_operators_486_sema_test.cpp
//   (BEZEL_TEST sema_lifted_ops_486, char_bitand_is_error_6_5).

module M; class C public method M(a: char, b: char) _ := a & b; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `&` is not valid on `char` — bitwise and shift operators require integer operands
// CHECK-SAME: (§6.5)
