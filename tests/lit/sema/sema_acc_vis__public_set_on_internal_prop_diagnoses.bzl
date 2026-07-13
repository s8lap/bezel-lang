// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/accessor_visibility_test.cpp
//   (BEZEL_TEST sema_acc_vis, public_set_on_internal_prop_diagnoses).

module M; class C internal property X: int public set := 0; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: accessor of property `X` is less restrictive than the property's own visibility
// CHECK-SAME: (§13.6)
