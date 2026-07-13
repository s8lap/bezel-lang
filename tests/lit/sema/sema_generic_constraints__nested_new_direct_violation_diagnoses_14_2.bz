// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_constraints_test.cpp
//   (BEZEL_TEST sema_generic_constraints, nested_new_direct_violation_diagnoses_14_2).

module M; class C<T> where T: new() private _x: T?; end C; interface IFoo end IFoo; class App private _u: C<IFoo>?; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `IFoo` does not satisfy the constraint `where T: new()`
// CHECK-SAME: (§14.2)
