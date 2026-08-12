// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §11.7 / §11.5 (v0.242, #3052): `closed` is a class-shape modifier. It is
// collected by the shared member-modifier prefix only so a NESTED type can
// carry it (§11.6 reuses the A.3 type-declaration productions); on an ordinary
// member it is meaningless — the same posture §11.5 states for `sealed`
// ("`sealed` alone is a class-level modifier … it does not appear on
// individual members").

// Both a CLASS member and a RECORD member are checked: the member scan runs
// ahead of the type-kind gate, so the modifier cannot ride along inertly on a
// type kind that never reaches the class rules.

module M; class C public closed method Area(): int => 1; end C; record R(X: int) public closed method Twice(): int => X * 2; end R;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `Area` may not be `closed`
// CHECK-SAME: (§11.7)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `Twice` may not be `closed`
// CHECK-SAME: (§11.7)
