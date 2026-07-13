// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/scoped_iface_match_1685_test.cpp
//   (BEZEL_TEST sema_scoped_iface_match_1685, inherited_impl_scoped_mismatch_rejected).
//
// §11.2 — the interface obligation may be satisfied by a method INHERITED
// from a base class rather than redeclared on the conforming class itself;
// the `scoped` match must still be enforced against that inherited method,
// not silently skipped because the conforming class has no member of its own
// by that name.

module M; interface IEventOwner method OnEvent(scoped cb: () -> ()); end IEventOwner; class Base public method OnEvent(cb: () -> ()) end OnEvent; end Base; class D : Base, IEventOwner end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `OnEvent` does not match interface member `OnEvent`'s parameter modifier on parameter 1 — `ref`/`out`/`owned`/`scoped` must match exactly
// CHECK-SAME: (§11.2)
