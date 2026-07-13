// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/getter_only_property_747_test.cpp
//   (BEZEL_TEST getter_only_property_747, block_getter_only_rejected).
//
// §23.3: assignment to a block-form getter-only property is rejected.

module M; class C public _x: int := 0; public property Val: int get => _x; end; public static method Main(): int using var c := new C(); c.Val := 99; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to read-only property `Val` — it has no setter
// CHECK-SAME: (§23.3)
