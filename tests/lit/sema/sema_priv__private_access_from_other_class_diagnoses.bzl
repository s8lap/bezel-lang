// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/private_visibility_test.cpp
//   (BEZEL_TEST sema_priv, private_access_from_other_class_diagnoses).

module M; class A private _x: int := 1; end A; class B public method M(a: A) _ := a._x; end M; end B;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `_x` is private to type `A` and not accessible from `B`
// CHECK-SAME: (§13.1 — `private` is visible to the declaring class only)
