// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §11.7 / §11.5 (v0.242, #3052): `closed` is a class-shape modifier. It is
// collected by the shared member-modifier prefix only so a NESTED type can
// carry it (§11.6 reuses the A.3 type-declaration productions); on an ordinary
// member it is meaningless — the same posture §11.5 states for `sealed`
// ("`sealed` alone is a class-level modifier … it does not appear on
// individual members").

module M; class C public closed method Area(): int => 1; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `Area` may not be `closed`
// CHECK-SAME: (§11.7)
