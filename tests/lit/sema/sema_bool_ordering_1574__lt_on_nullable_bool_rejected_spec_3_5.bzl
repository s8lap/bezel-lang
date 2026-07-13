// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/bool_ordering_1574_test.cpp
//   (BEZEL_TEST sema_bool_ordering_1574, lt_on_nullable_bool_rejected_spec_3_5).
//
// §7.11 boundary note (v0.182): `bool?` does not lift for ordering either,
// since bool itself has no ordering to lift.

module M; class C public method M(a: bool?, b: bool?) _ := a < b; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `<` is not defined for `bool` operands
// CHECK-SAME: (§3.5)
