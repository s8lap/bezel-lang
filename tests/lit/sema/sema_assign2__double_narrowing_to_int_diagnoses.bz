// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/assignment_assignability_test.cpp
//   (BEZEL_TEST sema_assign2, double_narrowing_to_int_diagnoses).

module M; class C public method M(p: int, b: bool) var x: int := 0; x := 3.14; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the assigned value has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
