// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lifted_operators_486_sema_test.cpp
//   (BEZEL_TEST sema_lifted_ops_486, bool_nullable_not_is_error_spec_7_11).
//
// §7.11: `not bool?` is a compile error.

module M; class C public method M(a: bool?) _ := not a; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `not` requires a non-nullable `bool` operand
// CHECK-SAME: (§7.11)
