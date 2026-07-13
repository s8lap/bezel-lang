// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/sealed_member_override_485_test.cpp
//   (BEZEL_TEST sema_sealed_member_485, sealed_override_three_levels_deep_gives_sealed_diagnostic).
//
// §12.4 — test 2: sealed override at a deeper level (A → B → C → D).
// D attempts to override Get() which is sealed in B (and inherited via C).

module M; class A public virtual method Get(): int => 1; destructor end destructor; end A; class B : A public sealed override method Get(): int => 2; destructor end destructor; end B; class C : B destructor end destructor; end C; class D : C public override method Get(): int => 4; destructor end destructor; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot override `Get`: it is declared `sealed` in `B`
// CHECK-SAME: (§12.4)
