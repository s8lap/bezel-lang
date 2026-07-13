// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_constraints_test.cpp
//   (BEZEL_TEST sema_generic_constraints, nested_class_indirect_violation_diagnoses_14_2).

module M; class C<T> where T: class private _x: T?; end C; class Box<W> private _b: C<W>?; end Box; class App private _u: Box<int>?; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `int` does not satisfy the constraint `where T: class`
// CHECK-SAME: (§14.2)
