// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/class_modifier_combos_test.cpp
//   (BEZEL_TEST sema_class_mods, member_virtual_abstract_diagnoses_r39).

module M; class B public virtual abstract method F(): int; end B;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `F` may have at most one of `abstract`, `virtual`, `override`, or `sealed override` — they are mutually exclusive
// CHECK-SAME: (§12.1)
