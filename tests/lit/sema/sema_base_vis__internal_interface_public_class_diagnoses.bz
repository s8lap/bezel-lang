// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/base_visibility_test.cpp
//   (BEZEL_TEST sema_base_vis, internal_interface_public_class_diagnoses).

module M; internal interface I method F(); end I; public class C : I public method F() _ := 1; end F; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: base type `I` is less visible than `C` — a base class or implemented interface must be at least as visible as the type
// CHECK-SAME: (§13.7)
