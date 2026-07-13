// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/class_modifier_combos_test.cpp
//   (BEZEL_TEST sema_class_mods, abstract_static_diagnoses).

module M; abstract static class C end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `C` may not be both `abstract` and `static` (mutually exclusive — §11.7)
