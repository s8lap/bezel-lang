// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/primitive_assignability_test.cpp
//   (BEZEL_TEST sema_assign, int_var_to_byte_assign_narrowing_rejected_738).

module M; class C public method M() var x: int := 300; var b: byte := 1; b := x; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the assigned value has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
