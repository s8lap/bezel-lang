// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_constraints_test.cpp
//   (BEZEL_TEST sema_generic_constraints, notnull_with_nullable_ref_arg_diagnoses_14_2).

module M; class SomeClass end SomeClass; class C<T> where T: notnull private _x: T?; end C; class U private _u: C<SomeClass?>?; end U;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `SomeClass?` does not satisfy the constraint `where T: notnull`
// CHECK-SAME: (§14.2)
