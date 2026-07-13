// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/getter_only_property_747_test.cpp
//   (BEZEL_TEST getter_only_property_747, expression_bodied_rejected).
//
// §23.3: assignment to an expression-bodied property is rejected.

module M; class C public _x: int := 5; public property Val: int => _x; public static method Main(): int using var c := new C(); c.Val := 9; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to read-only property `Val` — it has no setter
// CHECK-SAME: (§23.3)
