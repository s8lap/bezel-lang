// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/hiding_override_test.cpp
//   (BEZEL_TEST sema_hide, sealed_override_cannot_be_overridden_again).

module M; class A public virtual method M() _ := 1; end M; end A; class B : A public sealed override method M() _ := 2; end M; end B; class C : B public override method M() _ := 3; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot override `M`: it is declared `sealed` in `B`
// CHECK-SAME: (§12.4)
