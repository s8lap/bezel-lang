// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_init_restrictions_test.cpp
//   (BEZEL_TEST sema_field_init, instance_member_ref_diagnoses).

module M; class C public _a: int := _b; public _b: int := 0; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a field initializer may not reference instance member `_b`
// CHECK-SAME: (§22.8)
