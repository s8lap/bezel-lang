// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lifted_operators_486_sema_test.cpp
//   (BEZEL_TEST sema_lifted_ops_486, int_add_result_needs_narrowing_spec_7_11).
//
// §7.11: the lifted result is int?; assigning it to a plain `int` local (with
// explicit non-nullable type annotation) must be a compile error.

module M; class C public method M(a: int?, b: int?) var x: int := a + b; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` is nullable but the target type is non-nullable
// CHECK-SAME: (§7)
