// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/override_signature_test.cpp
//   (BEZEL_TEST sema_override, param_modifier_mismatch_diagnoses).

module M; class Base public virtual method F(a: int) _ := 1; end F; end Base; class D : Base public override method F(ref a: int) _ := 2; end F; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `F` does not match the base member's parameter modifiers
// CHECK-SAME: (§12.1)
