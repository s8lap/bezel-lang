// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/readonly_field_test.cpp
//   (BEZEL_TEST sema_readonly, assigned_in_method_diagnoses).

module M; class C public readonly _x: int := 0; public method M() this._x := 1; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_x` outside the declaring class's constructor
// CHECK-SAME: (§22.8)
