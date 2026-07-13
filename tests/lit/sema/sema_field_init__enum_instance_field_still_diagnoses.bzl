// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_init_restrictions_test.cpp
//   (BEZEL_TEST sema_field_init, enum_instance_field_still_diagnoses).

module M; class C public _x: int := 0; public static _y: int := _x; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a field initializer may not reference instance member `_x`
// CHECK-SAME: (§22.8)
