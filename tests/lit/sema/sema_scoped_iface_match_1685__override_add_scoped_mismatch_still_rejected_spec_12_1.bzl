// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/scoped_iface_match_1685_test.cpp
//   (BEZEL_TEST sema_scoped_iface_match_1685, override_add_scoped_mismatch_still_rejected_spec_12_1).
//
// §12.1 (v0.184) CONTROL — the `override` direction (base class, not
// interface) was already enforced before #1685 (checkOverrideSignatureMatch's
// exact modifier comparison covers `scoped` as a plain string compare); this
// pins that it stays enforced alongside the new interface-side check.

module M; class Base public virtual method OnEvent(cb: () -> ()) end OnEvent; end Base; class D : Base public override method OnEvent(scoped cb: () -> ()) end OnEvent; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `OnEvent` does not match the base member's parameter modifiers
// CHECK-SAME: (§12.1)
