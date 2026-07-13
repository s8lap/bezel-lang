// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_constraints_test.cpp
//   (BEZEL_TEST sema_generic_constraints, iface_bound_nonconforming_diagnoses_14_2).

module M; interface IFoo method F(); end IFoo; class Plain end Plain; class C<T> where T: IFoo private _x: T?; end C; class U private _u: C<Plain>?; end U;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Plain` does not satisfy the constraint `where T: IFoo`
// CHECK-SAME: (§14.2)
