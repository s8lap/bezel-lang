// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/readonly_auto_property_747_test.cpp
//   (BEZEL_TEST readonly_auto_property_747, assign_outside_ctor_rejected).
//
// §23.1: a readonly auto-property write in a normal method is rejected.

module M; class C public readonly property Version: int := 1; public static method Main(): int using var c := new C(); c.Version := 99; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly property `Version` outside a field initializer or constructor
// CHECK-SAME: (§23.1)
