// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/scoped_type_identity_2712_test.cpp
//   (BEZEL_TEST scoped_type_identity_2712, func_type_non_function_position_rejected).
//
// A.6 (v0.175): `scoped` on a non-function position INSIDE a function type is
// rejected (the FuncParamMods carry the same function-typed-only rule).

module M; class C private _g: ((scoped [3]int) -> ())?; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `scoped` is valid only on a function-typed, class-typed, or interface-typed parameter
// CHECK-SAME: (§22.3, §28.14 v0.175/v0.180)
