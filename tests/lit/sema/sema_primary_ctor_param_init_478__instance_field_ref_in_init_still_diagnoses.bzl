// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/primary_ctor_param_init_478_test.cpp
//   (BEZEL_TEST sema_primary_ctor_param_init_478, instance_field_ref_in_init_still_diagnoses).
//
// §22.8: referencing another instance FIELD from a field initializer must
// still produce an error after the fix. The fix is surgical: only
// primary-ctor params (posParam != null, parent kind == Class) are allowed.

module M; class C public _a: int := _b; public _b: int := 0; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a field initializer may not reference instance member `_b`
// CHECK-SAME: (§22.8)
