// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/class_modifier_combos_test.cpp
//   (BEZEL_TEST sema_class_mods, member_sealed_alone_diagnoses_r39).
//
// `sealed` on a member is valid only as part of `sealed override`.

module M; class B public sealed method F(): int return 1; end F; end B;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `F` may not be `sealed` on its own — on a member `sealed` appears only as `sealed override`
// CHECK-SAME: (§12.1)
