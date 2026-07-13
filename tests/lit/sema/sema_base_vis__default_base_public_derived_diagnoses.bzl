// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/base_visibility_test.cpp
//   (BEZEL_TEST sema_base_vis, default_base_public_derived_diagnoses).

module M; class B end B; public class D : B end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: base type `B` is less visible than `D` — a base class or implemented interface must be at least as visible as the type
// CHECK-SAME: (§13.7)
