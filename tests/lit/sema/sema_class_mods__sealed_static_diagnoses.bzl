// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/class_modifier_combos_test.cpp
//   (BEZEL_TEST sema_class_mods, sealed_static_diagnoses).

module M; sealed static class C end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `C` may not be both `sealed` and `static` (mutually exclusive — §11.7)
