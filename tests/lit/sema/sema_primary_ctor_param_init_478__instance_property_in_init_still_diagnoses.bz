// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/primary_ctor_param_init_478_test.cpp
//   (BEZEL_TEST sema_primary_ctor_param_init_478, instance_property_in_init_still_diagnoses).
//
// §22.8: referencing an instance property (not a primary-ctor param) from
// a property initializer must still be rejected.

module M; class C public property A: int := 0; public property B: int := A; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a field initializer may not reference instance member `A`
// CHECK-SAME: (§22.8)
