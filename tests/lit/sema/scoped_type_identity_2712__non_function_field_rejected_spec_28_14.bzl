// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/scoped_type_identity_2712_test.cpp
//   (BEZEL_TEST scoped_type_identity_2712, non_function_field_rejected_spec_28_14).
//
// §28.14 (v0.175): a `scoped` FIELD must be function-typed too.

module M; class C private scoped _x: int; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `scoped` is valid only on a function-typed, class-typed, or interface-typed field
// CHECK-SAME: (§22.3, §28.14 v0.175/v0.180)
