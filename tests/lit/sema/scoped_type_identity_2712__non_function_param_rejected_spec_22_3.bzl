// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/scoped_type_identity_2712_test.cpp
//   (BEZEL_TEST scoped_type_identity_2712, non_function_param_rejected_spec_22_3).
//
// §22.3 (v0.175): `scoped` on a NON-function-typed parameter is rejected.

module M; class C public method M(scoped x: int) end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `scoped` is valid only on a function-typed, class-typed, or interface-typed parameter
// CHECK-SAME: (§22.3, §28.14 v0.175/v0.180)
