// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/toplevel_visibility_test.cpp
//   (BEZEL_TEST sema_toplevel_vis, protected_diagnoses).

module M; protected class C end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: top-level type `C` may only be `public` or `internal`
// CHECK-SAME: (§13.2)
