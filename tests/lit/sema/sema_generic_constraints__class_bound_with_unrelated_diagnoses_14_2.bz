// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_constraints_test.cpp
//   (BEZEL_TEST sema_generic_constraints, class_bound_with_unrelated_diagnoses_14_2).

module M; class Base end Base; class Other end Other; class C<T> where T: Base private _x: T?; end C; class U private _u: C<Other>?; end U;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Other` does not satisfy the constraint `where T: Base`
// CHECK-SAME: (§14.2)
