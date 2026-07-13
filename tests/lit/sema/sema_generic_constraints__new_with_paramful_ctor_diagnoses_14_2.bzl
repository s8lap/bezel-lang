// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_constraints_test.cpp
//   (BEZEL_TEST sema_generic_constraints, new_with_paramful_ctor_diagnoses_14_2).

module M; class NeedsArg public constructor(x: int) end constructor; end NeedsArg; class C<T> where T: new() private _x: T?; end C; class U private _u: C<NeedsArg>?; end U;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `NeedsArg` does not satisfy the constraint `where T: new()`
// CHECK-SAME: (§14.2)
