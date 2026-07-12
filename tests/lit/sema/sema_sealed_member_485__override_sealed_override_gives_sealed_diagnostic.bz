// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/sealed_member_override_485_test.cpp
//   (BEZEL_TEST sema_sealed_member_485, override_sealed_override_gives_sealed_diagnostic).
//
// §12.4 — test 1: repro from bug #485.
// Overriding a `sealed override` member must be rejected with a §12.4
// "sealed" diagnostic, not a §12.1 "not virtual/abstract/override" error.

module M; class A public virtual method Get(): int => 1; destructor end destructor; end A; class B : A public sealed override method Get(): int => 2; destructor end destructor; end B; class C : B public override method Get(): int => 3; destructor end destructor; public static method Main(): int => 0; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot override `Get`: it is declared `sealed` in `B`
// CHECK-SAME: (§12.4)
