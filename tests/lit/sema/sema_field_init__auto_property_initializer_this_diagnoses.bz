// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_init_restrictions_test.cpp
//   (BEZEL_TEST sema_field_init, auto_property_initializer_this_diagnoses).

module M; class C public _x: int := 0; public property P: int := this._x; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a field initializer may not reference `this`
// CHECK-SAME: (§22.8)
