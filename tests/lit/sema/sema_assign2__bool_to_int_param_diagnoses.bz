// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/assignment_assignability_test.cpp
//   (BEZEL_TEST sema_assign2, bool_to_int_param_diagnoses).

module M; class C public method M(p: int, b: bool) p := b; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the assigned value has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
