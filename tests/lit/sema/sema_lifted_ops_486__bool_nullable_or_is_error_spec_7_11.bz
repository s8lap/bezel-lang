// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lifted_operators_486_sema_test.cpp
//   (BEZEL_TEST sema_lifted_ops_486, bool_nullable_or_is_error_spec_7_11).
//
// §7.11: bool? `or` bool? is a compile error.

module M; class C public method M(a: bool?, b: bool?) _ := a or b; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `or` requires non-nullable `bool` operands
// CHECK-SAME: (§7.11)
