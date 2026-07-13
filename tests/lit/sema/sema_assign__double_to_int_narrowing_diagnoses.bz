// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/primitive_assignability_test.cpp
//   (BEZEL_TEST sema_assign, double_to_int_narrowing_diagnoses).

module M; class C public method M() var x: int := 3.14; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
