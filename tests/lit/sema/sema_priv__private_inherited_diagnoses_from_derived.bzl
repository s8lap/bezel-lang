// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/private_visibility_test.cpp
//   (BEZEL_TEST sema_priv, private_inherited_diagnoses_from_derived).

module M; class Base private _x: int := 1; end Base; class Derived : Base public method M() _ := this._x; end M; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `_x` is private to type `Base` and not accessible from `Derived`
// CHECK-SAME: (§13.1 — `private` is visible to the declaring class only)
