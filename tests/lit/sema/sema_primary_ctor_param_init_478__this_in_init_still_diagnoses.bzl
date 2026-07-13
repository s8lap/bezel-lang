// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/primary_ctor_param_init_478_test.cpp
//   (BEZEL_TEST sema_primary_ctor_param_init_478, this_in_init_still_diagnoses).
//
// §22.8: `this` in an initializer must still be rejected.

module M; class C public _x: int := 0; public _y: int := this._x; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a field initializer may not reference `this`
// CHECK-SAME: (§22.8)
