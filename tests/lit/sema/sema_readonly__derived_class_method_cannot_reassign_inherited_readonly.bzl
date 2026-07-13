// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/readonly_field_test.cpp
//   (BEZEL_TEST sema_readonly, derived_class_method_cannot_reassign_inherited_readonly).

module M; class Base public readonly _x: int := 0; end Base; class Derived : Base public method M() this._x := 1; end M; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_x` outside the declaring class's constructor
// CHECK-SAME: (§22.8)
