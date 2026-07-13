// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_ctor_params_test.cpp
//   (BEZEL_TEST sema_static_ctor, with_params_diagnoses).

module M; class C static constructor(a: int) end constructor; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: static constructor of `C` may not declare parameters
// CHECK-SAME: (§22.7)
