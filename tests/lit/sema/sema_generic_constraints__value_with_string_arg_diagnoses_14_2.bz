// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_constraints_test.cpp
//   (BEZEL_TEST sema_generic_constraints, value_with_string_arg_diagnoses_14_2).

module M; class C<T> where T: value private _x: T?; end C; class U private _u: C<string>?; end U;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `string` does not satisfy the constraint `where T: value`
// CHECK-SAME: (§14.2)
